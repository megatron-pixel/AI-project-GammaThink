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

test("native expansion capacity is resource-limited, never a false or partial source answer", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-quantified-limit-"));
  try {
    for (const oversized of [false, true]) {
      const fixture = join(temporary, oversized ? "oversized" : "normal");
      const metadata = await invoke("source-quantified-test",
        oversized ? [fixture, "--oversized"] : [fixture]);
      const result = await invoke("gammathink", [
        "query-source", metadata.module, join(fixture, "host.json"), join(fixture, "goal.json"),
        join(temporary, oversized ? "limited.db" : "normal.db"),
        join(fixture, metadata.snapshot), metadata.root,
      ]);
      expect(result.status).toBe(oversized ? "resource_limited" : "checked");
      expect(result.native_executions).toBe(oversized ? 0 : 1);
      expect(result.source_member_queries).toBe(oversized ? 0 : 4);
      expect(result.neural_calls).toBe(0);
      expect(result.english_meaning_certified).toBe(false);
      expect(result.empirical_truth_certified).toBe(false);
      expect(result.unavailable).toEqual([]);
    }
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
