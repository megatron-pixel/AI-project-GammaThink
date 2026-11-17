import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve, join } from "node:path";
import { startInspector } from "../viewer/server.mjs";

const directory = resolve(import.meta.dir, "..");
async function invoke(args) {
  const child = Bun.spawn([join(directory, "build/gammathink"), ...args], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [output, diagnostics, code] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  expect({ code, diagnostics }).toEqual({ code: 0, diagnostics: "" });
  return JSON.parse(output);
}

test("loopback inspector reads real native objects without granting effects", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "knowledge-viewer-"));
  let server;
  try {
    const database = join(temporary, "journal.db");
    const solved = await invoke(["solve", join(directory, "examples/task-context.json"),
      join(directory, "examples/task-request.dsl"), database]);
    expect(solved.status).toBe("checked");
    const root = solved.slots[0].request;
    server = await startInspector({ database, root });
    const unauthenticated = await fetch(server.url + "/api/inspect?id=" + root);
    expect(unauthenticated.status).toBe(403);
    const sessionResponse = await fetch(server.url + "/session");
    expect(sessionResponse.headers.get("cache-control")).toBe("no-store");
    expect(sessionResponse.headers.get("access-control-allow-origin")).toBeNull();
    expect(sessionResponse.headers.get("content-security-policy")).toContain("frame-ancestors 'none'");
    const session = await sessionResponse.json();
    expect(session.root).toBe(root);
    expect(session.neural_calls).toBe(0);
    const headers = { "x-gammathink-view": session.token };
    const inspectedResponse = await fetch(server.url + "/api/inspect?id=" + root, { headers });
    expect(inspectedResponse.status).toBe(200);
    expect(inspectedResponse.headers.get("vary")).toBe("Accept-Encoding");
    const inspected = await inspectedResponse.json();
    expect(inspected.format).toBe("gammathink.inspection.v1");
    expect(inspected.observational_only).toBe(true);
    expect(inspected.published).toBe(false);
    const focus = inspected.nodes.find(node => node.id === root);
    expect(focus.kind).toBe("GOAL");
    expect(new Bun.CryptoHasher("sha256").update(Buffer.from(focus.canonical_hex, "hex")).digest("hex")).toBe(root);
    const missing = await fetch(server.url + "/api/inspect?id=" + "0".repeat(64), { headers }).then(r => r.json());
    expect(missing.unavailable).toEqual(["0".repeat(64)]);
    expect(missing.has_unavailable).toBe(true);
    const search = await fetch(server.url + "/api/search?q=unfindableviewerfixture", { headers }).then(r => r.json());
    expect(search.format).toBe("gammathink.inspection-index.v1");
    expect(search.records).toEqual([]);
    const related = await fetch(server.url + `/api/related?id=${root}&relationship=PRODUCES`, { headers }).then(r => r.json());
    expect(related.format).toBe("gammathink.inspection-index.v1");
    expect(Array.isArray(related.records)).toBe(true);
    const all = await fetch(server.url + "/api/objects?kind=ALL", { headers }).then(r => r.json());
    expect(all.format).toBe("gammathink.inspection-index.v1");
    expect(all.records.length).toBeGreaterThan(0);
    const actions = await fetch(server.url + "/api/objects?kind=ACTION", { headers }).then(r => r.json());
    expect(actions.records.length).toBeGreaterThan(0);
    expect(actions.records.every(record => record.kind === "ACTION")).toBe(true);
    expect((await fetch(server.url + "/api/objects?kind=EXECUTE", { headers })).status).toBe(400);
    expect((await fetch(server.url + "/api/inspect?id=bad", { headers })).status).toBe(400);
    expect((await fetch(server.url + "/api/related?id=" + root + "&relationship=EXECUTE", { headers })).status).toBe(400);
    expect((await fetch(server.url + "/api/search?q=x&after=bad", { headers })).status).toBe(400);
    expect((await fetch(server.url + "/api/inspect?id=" + root, { method: "POST", headers })).status).toBe(405);
    expect((await fetch(server.url + "/session", { headers: { origin: "https://example.invalid" } })).status).toBe(403);
    expect((await fetch(server.url + "/session", { headers: { host: "rebinding.invalid" } })).status).toBe(403);
    expect((await fetch(server.url + "/session", { headers: { "sec-fetch-site": "cross-site" } })).status).toBe(403);
    expect((await fetch(server.url + "/api/inspect?id=" + root, {
      headers: { "x-gammathink-view": "\u00e9".repeat(session.token.length) },
    })).status).toBe(403);
    expect((await fetch(server.url + "/api/execute", { headers })).status).toBe(404);
    expect((await fetch(server.url + "/unknown")).status).toBe(404);
    expect((await fetch(server.url + "/favicon.ico")).status).toBe(204);
    const replay = await invoke(["solve", join(directory, "examples/task-context.json"),
      join(directory, "examples/task-request.dsl"), database]);
    expect(replay.native_executions).toBe(0);
    expect(replay.status).toBe("checked");
  } finally {
    server?.stop();
    await rm(temporary, { recursive: true });
  }
}, 60000);
