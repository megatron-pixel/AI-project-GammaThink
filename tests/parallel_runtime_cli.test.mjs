import { afterEach, expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const binary = resolve("build/gammathink");
const fixture = resolve("build/parallel-runtime-fixture");
const directories = [];
afterEach(async () => {
  for (const path of directories.splice(0)) await rm(path, { recursive: true, force: true });
});
async function execute(args) {
  const child = Bun.spawn(args, { stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000) });
  const [code, stdout, stderr] = await Promise.all([
    child.exited, new Response(child.stdout).text(), new Response(child.stderr).text(),
  ]);
  return { code, stdout, stderr };
}
async function setup() {
  const directory = await mkdtemp(join(tmpdir(), "parallel-runtime-"));
  directories.push(directory);
  const made = await execute([fixture, directory]);
  expect(made.code, made.stderr).toBe(0);
  return directory;
}
async function batch(directory, manifest) {
  const path = join(directory, "manifest.json");
  await Bun.write(path, JSON.stringify(manifest));
  return execute([binary, "prove-batch", path]);
}
test("application proof batches preserve serial bytes, duplicate identity and unknown outcomes", async () => {
  const directory = await setup();
  const requests = ["proved", "unknown", "proved"].map(name => join(directory, name + ".axrm"));
  const serial = await batch(directory, { version: 1, requests });
  const parallel = await batch(directory, { version: 1, workers: 2, requests });
  expect(serial.code, serial.stderr).toBe(0);
  expect(parallel.code, parallel.stderr).toBe(0);
  const one = JSON.parse(serial.stdout), two = JSON.parse(parallel.stdout);
  expect(two.profile).toBe("gammathink.native-proof-batch.v1");
  expect(two.neural_calls).toBe(0);
  expect(two.english_meaning_certified).toBe(false);
  expect(two.empirical_truth_certified).toBe(false);
  expect(two.executions).toBe(2);
  expect(two.memo_hits).toBe(1);
  expect(one.workers_started).toBe(1);
  expect(two.workers_started).toBe(2);
  expect(two.peak_active_workers).toBeGreaterThanOrEqual(1);
  expect(two.peak_active_workers).toBeLessThanOrEqual(2);
  expect(two.replies).toEqual(one.replies);
  expect(two.replies.map(reply => reply.producer)).toEqual([0, 1, 0]);
  expect(two.replies.map(reply => reply.valid)).toEqual([true, true, true]);
  expect(two.replies.map(reply => reply.conclusive)).toEqual([true, false, true]);
  expect(two.replies[0].native_reply_hex).toBe(two.replies[2].native_reply_hex);
});
test("application batch admission rejects nonproof input and invalid host budgets or fields", async () => {
  const directory = await setup();
  for (const manifest of [
    { version: 1, workers: 0, requests: [join(directory, "proved.axrm")] },
    { version: 1, workers: 5, requests: [join(directory, "proved.axrm")] },
    { version: 1, requests: [] },
    { version: 1, requests: Array(33).fill(join(directory, "proved.axrm")) },
    { version: 1, requests: [join(directory, "unsupported.axrm")] },
    { version: 1, requests: [0] },
    { version: 2, requests: [join(directory, "proved.axrm")] },
    { version: 1, permission: true, requests: [join(directory, "proved.axrm")] },
  ]) {
    const result = await batch(directory, manifest);
    expect(result.code).toBe(1);
    expect(result.stdout).toBe("");
  }
});
