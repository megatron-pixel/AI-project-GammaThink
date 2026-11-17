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
test("Task IR commands persist requested work without executing or granting permissions", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-task-plan-cli-"));
  try {
    const fixture = join(temporary, "fixture");
    const metadata = await invoke("task-plan-test", [fixture]);
    const context = join(fixture, "context.json");
    const proposal = join(fixture, "task.dsl");
    const environment = join(fixture, "environment.json");
    const journal = join(temporary, "journal.db");
    const planned = await invoke("knowledge-task-plan", ["inspect", context, proposal, journal, environment]);
    expect(planned.admitted).toBe(true);
    expect(planned.snapshot).toBe(metadata.snapshot);
    expect(planned.ready_actions).toEqual(["generate"]);
    expect(planned.actions).toBe(3);
    expect(planned.goals).toBe(5);
    expect(planned.entities).toBe(4);
    expect(planned.root_satisfied).toBe(false);
    expect(planned.missing_artifacts).toEqual(["code", "runlog"]);
    expect(planned.neural_calls).toBe(0);
    expect(planned.native_executions).toBe(0);
    expect(planned.action_executions).toBe(0);
    expect(planned.published).toBe(false);
    const restored = await invoke("knowledge-task-plan", ["restore", journal, planned.manifest, environment]);
    expect(restored.manifest).toBe(planned.manifest);
    expect(restored.ready_actions).toEqual(["generate"]);
    expect(restored.recorded_context_only).toBe(true);
    const pending = await invoke("knowledge-task-plan", ["inspect", context, proposal, journal]);
    expect(pending.ready_actions).toEqual([]);
    const declineJournal = join(temporary, "declined.db");
    const declined = await invoke("knowledge-task-plan", ["inspect", context,
      join(fixture, "clarify.dsl"), declineJournal, environment]);
    expect(declined.admitted).toBe(false);
    expect(declined.kind).toBe("clarify");
    expect(declined.action_executions).toBe(0);
    expect(await Bun.file(declineJournal).exists()).toBe(false);
    const freshContext = await invoke("knowledge-task-plan", ["context",
      join(fixture, "problem-context.json"), "0"]);
    expect(freshContext[0]).toBe("gammathink.task-ir-context.v1");
    expect(freshContext[3]).toBe(0);
    await invoke("knowledge-task-plan", ["context", join(fixture, "problem-context.json"), "999"], false);
    await Bun.write(environment, '[["network",true]]');
    expect((await invoke("knowledge-task-plan", ["restore", journal, planned.manifest, environment])).ready_actions).toEqual([]);
    const forged = join(temporary, "forged.dsl");
    await Bun.write(forged, (await Bun.file(proposal).text()).replace("END", 'OUTCOME ["respond",true]\nEND'));
    const rejectedJournal = join(temporary, "rejected.db");
    await invoke("knowledge-task-plan", ["inspect", context, forged, rejectedJournal], false);
    expect(await Bun.file(rejectedJournal).exists()).toBe(false);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
