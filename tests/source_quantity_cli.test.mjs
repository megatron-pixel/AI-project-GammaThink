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
test("offline library quantity queries retain scoped premises and original-core replay", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-quantity-cli-"));
  try {
    const fixture = join(temporary, "fixture");
    const metadata = await invoke("source-quantity-test", [fixture]);
    const journal = join(temporary, "journal.db");
    const snapshot = join(fixture, "snapshot");
    const goal = join(fixture, "goal.json");
    const args = ["query-source", metadata.module, join(fixture, "host.json"),
      goal, journal, snapshot, metadata.root];
    const result = await invoke("gammathink", args);
    expect(result.status).toBe("checked");
    expect(result.source_profile).toBe("bounded-module-quantity-v1");
    expect(result.source_member_queries).toBe(2);
    expect(result.native_executions).toBe(1);
    expect(result.neural_calls).toBe(0);
    expect(result.english_meaning_certified).toBe(false);
    expect(result.empirical_truth_certified).toBe(false);
    expect(result.unavailable).toEqual([]);
    const repeated = await invoke("gammathink", args);
    expect(repeated.status).toBe("checked");
    expect(repeated.reused).toBe(true);
    expect(repeated.native_executions).toBe(0);
    const replay = await invoke("gammathink",
      ["resume", result.request, journal, snapshot, metadata.root]);
    expect(replay.status).toBe("checked");
    expect(replay.reused).toBe(true);
    const absent = [...args]; absent[1] = "0".repeat(64);
    const missing = await invoke("gammathink", absent);
    expect(missing.status).toBe("dependencies_unavailable");
    expect(missing.native_executions).toBe(0);
    for (const invalid of ['["q",-1,0]', '["q",1,2]', '["q",99,0]',
      '["q",1.0,0]', '["q",1,0,0]']) {
      await Bun.write(goal, invalid);
      await invoke("gammathink", args, false);
    }
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
