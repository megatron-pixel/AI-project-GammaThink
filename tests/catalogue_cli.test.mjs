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
    return;
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
test("independently signed feed discovery never accepts before the host command", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-catalogue-cli-"));
  try {
    const journal = join(temporary, "journal.db");
    const first = await invoke("gammathink", ["solve", join(directory, "examples/task-context.json"),
      join(directory, "examples/task-request.dsl"), journal]);
    const snapshot = join(temporary, "snapshot");
    const compiled = await invoke("gammathink", ["snapshot", journal, snapshot, "catalogue-cli", "1"]);
    const publisher = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    // Independent WebCrypto Ed25519 implementation and a public RFC 8032 seed.
    const pkcs8 = Buffer.from("302e020100300506032b657004220420"
      + "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "hex");
    const privateKey = await crypto.subtle.importKey("pkcs8", pkcs8, "Ed25519", false, ["sign"]);
    const payload = { format: "gammathink.publisher-feed.v1", publisher, channel: "stable",
      sequence: 1, previous: null, offers: [{ name: "catalogue-cli", revision: "1",
        snapshot: rawCid(compiled.root), roots: [first.slots[0].request], parents: [] }] };
    const signature = Buffer.from(await crypto.subtle.sign("Ed25519", privateKey,
      new TextEncoder().encode(JSON.stringify(payload)))).toString("hex");
    const envelope = { payload, signature };
    const feedPath = join(temporary, "feed.json");
    await Bun.write(feedPath, JSON.stringify(envelope));
    const database = join(temporary, "catalogue.db");
    const found = await invoke("knowledge-catalogue", ["discover", database, publisher, feedPath]);
    expect(found.identity_verified).toBe(true);
    expect(found.accepted).toBe(false);
    expect(found.feed).toBe(new Bun.CryptoHasher("sha256").update(JSON.stringify(envelope)).digest("hex"));
    const list = await invoke("knowledge-catalogue", ["list", database, "-"]);
    expect(list.feeds).toEqual([found.feed]);
    expect(list.complete).toBe(true);
    const before = await invoke("knowledge-catalogue", ["resolve", database, "selected"]);
    expect(before).toEqual({ alias: "selected", generation: 0, mounted: false });
    const shown = await invoke("knowledge-catalogue", ["show", database, publisher, found.feed]);
    expect(shown).toEqual(envelope);
    const accept = ["accept", database, publisher, found.feed, "0", "selected", snapshot, "0"];
    const accepted = await invoke("knowledge-catalogue", accept);
    expect(accepted.root).toBe(compiled.root);
    expect(accepted.generation).toBe(1);
    expect(accepted.mounted).toBe(true);
    const resumed = await invoke("gammathink", ["resume-catalogue", first.slots[0].request,
      join(temporary, "receiving-journal.db"), database]);
    expect(resumed.status).toBe("checked");
    expect(resumed.native_executions).toBe(1);
    expect(resumed.neural_calls).toBe(0);
    expect(resumed.english_meaning_certified).toBe(false);
    expect(resumed.empirical_truth_certified).toBe(false);
    await invoke("knowledge-catalogue", accept, false);
    await invoke("knowledge-catalogue", [...accept, "extra"], false);
    await invoke("knowledge-catalogue", ["show", database, "0".repeat(64), found.feed], false);
    const removed = await invoke("knowledge-catalogue", ["unmount", database, "selected", "1"]);
    expect(removed).toEqual({ alias: "selected", generation: 2, mounted: false });
    const unavailable = await invoke("gammathink", ["resume-catalogue", first.slots[0].request,
      join(temporary, "unmounted-journal.db"), database]);
    expect(unavailable.status).toBe("dependencies_unavailable");
    expect(unavailable.native_executions).toBe(0);
    expect((await invoke("knowledge-catalogue", ["list", database, "-"])).feeds).toEqual([found.feed]);
    await Bun.write(feedPath, JSON.stringify({ payload: { ...payload, channel: "tampered" }, signature }));
    const absent = join(temporary, "rejected.db");
    await invoke("knowledge-catalogue", ["discover", absent, publisher, feedPath], false);
    expect(await Bun.file(absent).exists()).toBe(false);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
