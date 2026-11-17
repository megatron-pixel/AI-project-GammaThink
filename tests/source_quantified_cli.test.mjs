import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const directory = join(dirname(fileURLToPath(import.meta.url)), "..");
async function invoke(binary, args) {
  const child = Bun.spawn([join(directory, "build", binary), ...args], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [stdout, stderr, code] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  expect({ code, stderr }).toEqual({ code: 0, stderr: "" });
  return JSON.parse(stdout);
}

test("version2 quantified source snapshots query and replay through the offline CLI", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-quantified-cli-"));
  try {
    const fixture = join(temporary, "fixture");
    const metadata = await invoke("source-quantified-v2-test", [fixture]);
    const journal = join(temporary, "journal.db");
    const snapshot = join(fixture, "snapshot");
    const args = ["query-source", metadata.module, join(fixture, "host.json"),
      join(fixture, "goal.json"), journal, snapshot, metadata.root];
    const result = await invoke("gammathink-quantified-source-v2", args);
    expect(result.status).toBe("checked");
    expect(result.source_profile).toBe("bounded-module-quantity-v1");
    expect(result.source_member_queries).toBe(4);
    expect(result.native_executions).toBe(1);
    expect(result.neural_calls).toBe(0);
    expect(result.english_meaning_certified).toBe(false);
    expect(result.empirical_truth_certified).toBe(false);
    expect(result.unavailable).toEqual([]);
    const repeated = await invoke("gammathink-quantified-source-v2", args);
    expect(repeated.status).toBe("checked");
    expect(repeated.reused).toBe(true);
    expect(repeated.native_executions).toBe(0);
    const replay = await invoke("gammathink-quantified-source-v2",
      ["resume", result.request, journal, snapshot, metadata.root]);
    expect(replay.status).toBe("checked");
    expect(replay.reused).toBe(true);
    const absentGoal = join(fixture, "absent.json");
    await Bun.write(absentGoal, '["q",3,0]');
    const unresolved = [...args];
    unresolved[3] = absentGoal;
    const absent = await invoke("gammathink-quantified-source-v2", unresolved);
    expect(absent.status).toBe("unresolved");
    expect(absent.unavailable).toEqual([]);
    expect(absent.neural_calls).toBe(0);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
