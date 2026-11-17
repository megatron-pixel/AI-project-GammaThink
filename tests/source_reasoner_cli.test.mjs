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
test("offline source queries retrieve actual scoped rules and replay through the original core", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-source-query-cli-"));
  try {
    const fixture = join(temporary, "fixture");
    const metadata = await invoke("source-reasoner-test", [fixture]);
    const journal = join(temporary, "journal.db");
    const snapshot = join(fixture, "snapshot");
    const query = ["query-source", metadata.module, join(fixture, "host.json"),
      join(fixture, "goal.json"), journal, snapshot, metadata.root];
    const result = await invoke("gammathink", query);
    expect(result.status).toBe("checked");
    expect(result.source_rule_queries).toBe(3);
    expect(result.source_work).toBeGreaterThan(3);
    expect(result.native_executions).toBe(1);
    expect(result.neural_calls).toBe(0);
    expect(result.english_meaning_certified).toBe(false);
    expect(result.empirical_truth_certified).toBe(false);
    expect(result.unavailable).toEqual([]);
    const replayed = await invoke("gammathink", ["resume", result.request, journal, snapshot, metadata.root]);
    expect(replayed.status).toBe("checked");
    expect(replayed.reused).toBe(true);
    expect(replayed.native_executions).toBe(0);
    const absent = [...query]; absent[1] = "0".repeat(64);
    const missing = await invoke("gammathink", absent);
    expect(missing.status).toBe("dependencies_unavailable");
    expect(missing.native_executions).toBe(0);
    expect(missing.source_rule_queries).toBe(0);
    const host = await Bun.file(join(fixture, "host.json")).json();
    host.registry[1] = "wrong-host-registry";
    const wrongHost = join(temporary, "wrong-host.json");
    await Bun.write(wrongHost, JSON.stringify(host));
    const wrong = [...query]; wrong[2] = wrongHost;
    await invoke("gammathink", wrong, false);
    await Bun.write(join(fixture, "goal.json"), '["atom",[2,false,[["v",0]]]]');
    await invoke("gammathink", query, false);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
