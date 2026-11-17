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
for (const profile of ["logic", "quantity"]) {
  test(`offline explicitly bound ${profile} requires current assumption acceptance`, async () => {
    const temporary = await mkdtemp(join(tmpdir(), "gammathink-bound-cli-"));
    try {
      const fixture = join(temporary, "fixture");
      const metadata = await invoke("bound-sources-test",
        profile === "quantity" ? [fixture, "quantity"] : [fixture]);
      const journal = join(temporary, "journal.db");
      const snapshot = join(fixture, "snapshot");
      const hosts = join(fixture, "hosts.json");
      const args = ["query-bound", metadata.binding, hosts, metadata.goal_module,
        join(fixture, "goal.json"), journal, snapshot, metadata.root];
      await invoke("gammathink", args, false);
      expect(await Bun.file(journal).exists()).toBe(false);
      await invoke("gammathink", [...args, "--accept-binding", "0".repeat(64)], false);
      const accepted = [...args, "--accept-binding", metadata.binding];
      const result = await invoke("gammathink", accepted);
      expect(result.status).toBe("checked");
      expect(result.source_profile).toBe("explicit-bound-sources-v1");
      expect(result.conditional).toBe(true);
      expect(result.binding_assumption).toBe(metadata.binding);
      expect(result.source_scopes).toBe(2);
      expect(result.source_member_queries).toBe(profile === "quantity" ? 2 : 3);
      expect(result.native_executions).toBe(1);
      expect(result.neural_calls).toBe(0);
      expect(result.english_meaning_certified).toBe(false);
      expect(result.empirical_truth_certified).toBe(false);
      const withoutAcceptance = await invoke("gammathink",
        ["resume", result.request, journal, snapshot, metadata.root]);
      expect(withoutAcceptance.status).toBe("revalidation_required");
      expect(withoutAcceptance.native_executions).toBe(0);
      expect(withoutAcceptance.revalidation_required).toEqual([metadata.binding]);
      const replay = await invoke("gammathink", accepted);
      expect(replay.status).toBe("checked");
      expect(replay.reused).toBe(true);
      expect(replay.native_executions).toBe(0);
      const altered = await Bun.file(hosts).json();
      altered[0][1].registry[1] = "wrong-host-version";
      await Bun.write(hosts, JSON.stringify(altered));
      await invoke("gammathink", accepted, false);
    } finally {
      await rm(temporary, { recursive: true });
    }
  }, 60000);
}
