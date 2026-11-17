import { realpath, stat } from "node:fs/promises";
import { resolve } from "node:path";
import { timingSafeEqual } from "node:crypto";

const objectId = /^[0-9a-f]{64}$/;
const relationships = new Set([
  "REQUIRES", "IMPLIES", "SUPPORTS", "CONTRADICTS",
  "DEPENDS_ON", "PRODUCES", "TESTS", "ALTERNATIVE_TO",
]);
const objectKinds = new Set(["ALL", "CLAIM", "FACT", "RULE", "ASSUMPTION", "HYPOTHESIS",
  "DERIVATION", "EVIDENCE", "GOAL", "ACTION", "EXPERIMENT", "TEST_RESULT", "CONTRADICTION"]);
const assetPaths = new Map([
  ["/", ["index.html", "text/html; charset=utf-8"]],
  ["/primitives.html", ["primitives.html", "text/html; charset=utf-8"]],
  ["/style.css", ["style.css", "text/css; charset=utf-8"]],
  ["/app.mjs", ["app.mjs", "text/javascript; charset=utf-8"]],
  ["/graph.mjs", ["graph.mjs", "text/javascript; charset=utf-8"]],
]);
const headers = {
  "Cache-Control": "no-store",
  "X-Content-Type-Options": "nosniff",
  "Referrer-Policy": "no-referrer",
  "Cross-Origin-Resource-Policy": "same-origin",
  "Cross-Origin-Opener-Policy": "same-origin",
  "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self' data:; base-uri 'none'; frame-ancestors 'none'; form-action 'self'",
};
async function bounded(stream, maximum) {
  const chunks = [];
  let bytes = 0;
  for await (const chunk of stream) {
    bytes += chunk.byteLength;
    if (bytes > maximum) throw new Error("Native inspection output budget exceeded");
    chunks.push(chunk);
  }
  return Buffer.concat(chunks, bytes).toString("utf8");
}

export async function startInspector({ database, root, snapshot, port = 0 }) {
  if (!objectId.test(root)) throw new Error("Expected an exact lowercase object ID");
  database = await realpath(database);
  if (!(await stat(database)).isFile()) throw new Error("Expected an existing knowledge journal");
  const binary = await realpath(resolve(import.meta.dir, "../build/knowledge-inspect"));
  const archiveArgs = [];
  if (snapshot) {
    if (!objectId.test(snapshot.root)) throw new Error("Expected an exact snapshot root");
    archiveArgs.push("--snapshot", await realpath(snapshot.directory), snapshot.root);
  }
  const token = crypto.randomUUID();
  const tokenBytes = Buffer.from(token);
  const shutdown = new AbortController();
  let busy = false;
  const json = (value, status = 200) => Response.json(value, { status, headers });
  const server = Bun.serve({
    hostname: "127.0.0.1", port,
    maxRequestBodySize: 1024,
    async fetch(request, serving) {
      const bodyResponse = (body, type) => {
        const gzip = /(?:^|,)\s*gzip\s*(?:,|$)/.test(request.headers.get("accept-encoding") ?? "");
        return new Response(gzip ? Bun.gzipSync(body) : body, { headers: {
          ...headers, "Content-Type": type, Vary: "Accept-Encoding",
          ...(gzip ? { "Content-Encoding": "gzip" } : {}),
        } });
      };
      const origin = `http://127.0.0.1:${serving.port}`;
      if (request.headers.get("host") !== `127.0.0.1:${serving.port}`
        || (request.headers.has("origin") && request.headers.get("origin") !== origin))
        return json({ error: "This private viewer accepts only its loopback origin" }, 403);
      if (request.method !== "GET") return json({ error: "The viewer has no write operations" }, 405);
      const url = new URL(request.url);
      if (url.pathname === "/favicon.ico") return new Response(null, { status: 204, headers });
      const api = url.pathname.startsWith("/api/") || url.pathname === "/session";
      if (api && request.headers.get("sec-fetch-site") === "cross-site")
        return json({ error: "Cross-site inspection is not allowed" }, 403);
      if (url.pathname === "/session") return json({
        token, root, observational_only: true, neural_calls: 0,
        sources: snapshot ? ["Local journal", "Pinned local snapshot"] : ["Local journal"],
      });
      if (!url.pathname.startsWith("/api/")) {
        const asset = assetPaths.get(url.pathname);
        if (!asset) return json({ error: "Unknown viewer resource" }, 404);
        const file = Bun.file(resolve(import.meta.dir, asset[0]));
        if (!await file.exists()) return json({ error: "Viewer resource is unavailable" }, 404);
        return bodyResponse(await file.arrayBuffer(), asset[1]);
      }
      const supplied = request.headers.get("x-gammathink-view") ?? "";
      const suppliedBytes = Buffer.from(supplied);
      if (suppliedBytes.length !== tokenBytes.length
        || !timingSafeEqual(suppliedBytes, tokenBytes))
        return json({ error: "Open the viewer page before requesting private knowledge" }, 403);
      const cursor = url.searchParams.get("after") ?? "-";
      if (cursor !== "-" && !objectId.test(cursor)) return json({ error: "Invalid inspection cursor" }, 400);
      let args;
      if (url.pathname === "/api/inspect") {
        const id = url.searchParams.get("id");
        if (!objectId.test(id ?? "")) return json({ error: "Invalid object ID" }, 400);
        args = [id];
      } else if (url.pathname === "/api/search") {
        const query = url.searchParams.get("q") ?? "";
        if (!query || new TextEncoder().encode(query).length > 256)
          return json({ error: "Use a literal search phrase of 1 to 256 bytes" }, 400);
        args = ["--search", query, cursor];
      } else if (url.pathname === "/api/related") {
        const id = url.searchParams.get("id");
        const relation = url.searchParams.get("relationship");
        if (!objectId.test(id ?? "") || !relationships.has(relation))
          return json({ error: "Invalid incoming relationship query" }, 400);
        args = ["--related", id, relation, cursor];
      } else if (url.pathname === "/api/objects") {
        const kind = url.searchParams.get("kind") ?? "ALL";
        if (!objectKinds.has(kind)) return json({ error: "Unknown object kind" }, 400);
        args = ["--kind", kind, cursor];
      } else return json({ error: "Unknown inspection operation" }, 404);
      if (busy) return json({ error: "An inspection is already running; finish or cancel it before another read" }, 429);
      busy = true;
      let child;
      let ended = false;
      let completion;
      try {
        child = Bun.spawn([binary, database, ...args, ...archiveArgs], {
          stdin: "ignore", stdout: "pipe", stderr: "pipe",
          env: { PATH: "/usr/bin:/bin", LC_ALL: "C" },
          signal: AbortSignal.any([shutdown.signal, request.signal, AbortSignal.timeout(30000)]),
        });
        completion = child.exited.then(code => { ended = true; return code; });
        const [output, diagnostics, code] = await Promise.all([
          bounded(child.stdout, 9 * 1024 * 1024), bounded(child.stderr, 8192), completion,
        ]);
        if (code !== 0) throw new Error(diagnostics || "Native inspection did not complete");
        // Validate framing; object data remains observational, not executable.
        JSON.parse(output);
        return bodyResponse(output, "application/json; charset=utf-8");
      } catch (error) {
        return json({ error: error.message }, 422);
      } finally {
        if (child && !ended) child.kill();
        if (completion) await completion;
        busy = false;
      }
    },
  });
  return {
    url: `http://127.0.0.1:${server.port}`,
    stop() { shutdown.abort(); server.stop(true); },
  };
}

if (import.meta.main) {
  const [database, root, snapshotDirectory, snapshotRoot] = process.argv.slice(2);
  const viewer = await startInspector({
    database, root,
    snapshot: snapshotDirectory ? { directory: snapshotDirectory, root: snapshotRoot } : undefined,
  });
  console.log(`VIEWER_READY ${viewer.url}/`);
  for (const signal of ["SIGINT", "SIGTERM"]) process.on(signal, () => { viewer.stop(); process.exit(0); });
}
