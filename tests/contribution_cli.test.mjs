import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const directory = join(dirname(fileURLToPath(import.meta.url)), "..");
async function invoke(binary, args, success = true) {
  const child = Bun.spawn([join(directory, "build", binary), ...args], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [stdout, stderr, code] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  if (!success) {
    expect(code).toBe(1);
    expect(stderr.length).toBeGreaterThan(0);
    expect(stdout).toBe("");
    return { stderr };
  }
  expect({ code, stderr }).toEqual({ code: 0, stderr: "" });
  return JSON.parse(stdout);
}
function rawCid(hex) {
  const bytes = Buffer.concat([Buffer.from([1, 0x55, 0x12, 0x20]), Buffer.from(hex, "hex")]);
  const alphabet = "abcdefghijklmnopqrstuvwxyz234567";
  let bits = 0, word = 0, result = "b";
  for (const byte of bytes) {
    word = (word << 8) | byte; bits += 8;
    while (bits >= 5) { bits -= 5; result += alphabet[(word >>> bits) & 31]; word &= (1 << bits) - 1; }
  }
  if (bits) result += alphabet[(word << (5 - bits)) & 31];
  return result;
}
test("explicit exact-artifact approval gates snapshot and signed-feed publication commands", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-contribution-cli-"));
  const received = [];
  let fail = false;
  const server = Bun.serve({
    hostname: "127.0.0.1", port: 0,
    async fetch(request) {
      const url = new URL(request.url);
      if (request.method !== "POST" || url.pathname !== "/api/v0/block/put"
        || url.search !== "?cid-codec=raw&mhtype=sha2-256&mhlen=32&pin=true&allow-big-block=false")
        return new Response("Unexpected test request", { status: 400 });
      const form = await request.formData();
      const file = form.get("file");
      if (!(file instanceof File)) return new Response("Missing test file", { status: 400 });
      const bytes = new Uint8Array(await file.arrayBuffer());
      received.push(bytes);
      if (fail) return new Response("Synthetic unavailable node", { status: 503 });
      return Response.json({ Key: rawCid(new Bun.CryptoHasher("sha256").update(bytes).digest("hex")), Size: bytes.length });
    },
  });
  try {
    const fixture = join(temporary, "fixture");
    const values = await invoke("contribution-fixture", [fixture]);
    const journal = join(fixture, "journal.db");
    const packet = join(fixture, "packet");
    const prepared = await invoke("knowledge-contribute", ["prepare", journal,
      join(temporary, "prepared"), "synthetic-cli", "1", values.root]);
    expect(prepared.root).toBe(values.snapshot);
    expect(prepared.objects.length).toBe(2);
    expect(prepared.objects).not.toContain(values.private);
    expect(prepared.published).toBe(false);
    const reviewed = await invoke("knowledge-contribute", ["review", packet, values.snapshot, values.root]);
    expect(reviewed.objects).toEqual(prepared.objects);
    expect(received).toEqual([]);
    await invoke("knowledge-contribute", ["prepare", journal,
      join(temporary, "private"), "private", "1", values.private], false);
    expect(await Bun.file(join(temporary, "private")).exists()).toBe(false);
    const publish = ["publish", packet, values.snapshot, values.root,
      "--approve-root", values.snapshot, "--node", server.url.origin];
    const mismatched = [...publish]; mismatched[5] = "0".repeat(64);
    await invoke("knowledge-contribute", mismatched, false);
    await invoke("knowledge-contribute", publish.slice(0, 4), false);
    expect(received).toEqual([]);
    const published = await invoke("knowledge-contribute", publish);
    expect(published.root_stored).toBe(true);
    expect(published.confirmed.length).toBe(reviewed.blocks);
    expect(published.attempted_requests).toBe(reviewed.blocks);
    expect(published.root_cid).toBe(rawCid(values.snapshot));
    expect(new Bun.CryptoHasher("sha256").update(received.at(-1)).digest("hex")).toBe(values.snapshot);
    const beforeFeed = received.length;
    const publishFeed = ["publish-feed", join(fixture, "feed.json"), values.publisher,
      "--approve-feed", values.feed, "--node", server.url.origin];
    const wrongFeed = [...publishFeed]; wrongFeed[4] = "0".repeat(64);
    await invoke("knowledge-contribute", wrongFeed, false);
    expect(received.length).toBe(beforeFeed);
    const metadata = await invoke("knowledge-contribute", publishFeed);
    expect(metadata.root_stored).toBe(true);
    expect(metadata.confirmed.length).toBe(1);
    expect(metadata.root_cid).toBe(rawCid(values.feed));
    expect(received.length).toBe(beforeFeed + 1);
    expect(received.at(-1)).toEqual(new Uint8Array(await Bun.file(join(fixture, "feed.json")).arrayBuffer()));
    fail = true;
    const failed = await invoke("knowledge-contribute", publish, false);
    expect(failed.stderr).toContain("publication_receipt ");
    const partial = JSON.parse(failed.stderr.split("publication_receipt ")[1]);
    expect(partial.attempted_requests).toBe(1);
    expect(partial.confirmed).toEqual([]);
    expect(partial.root_stored).toBe(false);
  } finally {
    await server.stop(true);
    await rm(temporary, { recursive: true });
  }
}, 60000);
