import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const directory = join(dirname(fileURLToPath(import.meta.url)), "..");
async function invoke(args) {
  const child = Bun.spawn([join(directory, "build/gammathink"), ...args], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [stdout, stderr, code] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  expect({ code, stderr }).toEqual({ code: 0, stderr: "" });
  return JSON.parse(stdout);
}

test("offline DSL, persisted proof, private snapshot and fresh-journal archive replay", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-cli-"));
  try {
    const journal = join(temporary, "journal.db");
    const solve = ["solve", join(directory, "examples/task-context.json"),
      join(directory, "examples/task-request.dsl"), journal];
    const first = await invoke(solve);
    expect(first.status).toBe("checked");
    expect(first.native_executions).toBe(1);
    expect(first.slots.map(row => row.ordinal)).toEqual([0, 1]);
    expect(first.slots[0].request).toBe(first.slots[1].request);
    const repeated = await invoke(solve);
    expect(repeated.native_executions).toBe(0);
    const snapshot = join(temporary, "snapshot");
    const compiled = await invoke(["snapshot", journal, snapshot, "test", "1"]);
    expect(compiled.published).toBe(false);
    expect(compiled.objects).toBeGreaterThan(0);
    const fresh = join(temporary, "fresh.db");
    const resume = ["resume", first.slots[0].request, fresh, snapshot, compiled.root];
    const restored = await invoke(resume);
    expect(restored.status).toBe("checked");
    expect(restored.imported_objects).toBeGreaterThan(1);
    expect(restored.object_lookups).toBeGreaterThan(1);
    expect(restored.native_executions).toBe(1);
    expect(restored.neural_calls).toBe(0);
    expect(restored.english_meaning_certified).toBe(false);
    expect(restored.empirical_truth_certified).toBe(false);
    const reused = await invoke(resume);
    expect(reused.reused).toBe(true);
    expect(reused.native_executions).toBe(0);
    const missing = await invoke(["resume", "0".repeat(64), fresh, snapshot, compiled.root]);
    expect(missing.status).toBe("dependencies_unavailable");
    expect(missing.native_executions).toBe(0);
    expect(missing.unavailable).toEqual(["0".repeat(64)]);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
