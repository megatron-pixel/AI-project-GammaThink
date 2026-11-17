import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const directory = join(dirname(fileURLToPath(import.meta.url)), "..");
async function invoke(binary, args, expected = 0) {
  const child = Bun.spawn([join(directory, "build", binary), ...args], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [stdout, stderr, code] = await Promise.all([
    new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited,
  ]);
  expect({ code, ...(code === expected ? {} : { stderr }) }).toEqual({ code: expected });
  return { stdout, stderr };
}

test("actual CLI captures complete ZIM input and stores only goal-free scoped source candidates", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-source-cli-"));
  try {
    const zim = join(temporary, "source.zim");
    await invoke("source-ingestion-test", ["--fixture", zim]);
    const preparedOutput = (await invoke("gammathink", [
      "source", zim, "article", join(directory, "examples/task-context.json"),
    ])).stdout;
    const prepared = JSON.parse(preparedOutput);
    expect(prepared.status).toBe("source_prepared");
    expect(prepared.english_meaning_certified).toBe(false);
    expect(prepared.context.views).toHaveLength(1);
    const capture = join(temporary, "capture.json");
    await Bun.write(capture, preparedOutput);
    const journal = join(temporary, "journal.db");
    const args = ["ingest", zim, "article", capture, join(directory, "examples/source-request.dsl"), journal];
    const stored = JSON.parse((await invoke("gammathink", args)).stdout);
    expect(stored.status).toBe("source_candidate_stored");
    expect(stored.new_objects).toBe(4);
    expect(stored.members).toHaveLength(1);
    expect(stored.neural_calls).toBe(0);
    expect(stored.empirical_truth_certified).toBe(false);
    expect(stored.published).toBe(false);
    const repeated = JSON.parse((await invoke("gammathink", args)).stdout);
    expect(repeated.new_objects).toBe(0);
    prepared.context_snapshot = "changed-since-translation";
    await Bun.write(capture, JSON.stringify(prepared));
    const rejected = await invoke("gammathink", args, 1);
    expect(rejected.stderr.length).toBeGreaterThan(0);
    const snapshot = JSON.parse((await invoke("gammathink", [
      "snapshot", journal, join(temporary, "snapshot"), "source-cli", "1",
    ])).stdout);
    expect(snapshot.objects).toBe(4);
    expect(snapshot.published).toBe(false);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
