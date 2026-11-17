import { afterEach, expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const binary = resolve(process.env.GAMMATHINK_SESSION_BINARY ?? "build/gammathink-session");
const fixtures = [];
afterEach(async () => {
  for (const path of fixtures.splice(0)) await rm(path, { recursive: true, force: true });
});
async function invoke(state, request, success = true) {
  const process = Bun.spawn([binary, state, "-"], {
    stdin: "pipe", stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  // Subscribe before supplying the command; there is no sleep or status polling.
  const exited = process.exited;
  const stdout = new Response(process.stdout).text();
  const stderr = new Response(process.stderr).text();
  process.stdin.write(JSON.stringify({ version: 1, session: "test", ...request }));
  process.stdin.end();
  const [code, output, diagnostics] = await Promise.all([exited, stdout, stderr]);
  expect(code, diagnostics || output).toBe(success ? 0 : 1);
  const result = JSON.parse(output);
  expect(result.profile).toBe("gammathink.session-result.v1");
  if (!success) expect(result.status).toBe("error");
  return result;
}
async function setup(text) {
  const parent = await mkdtemp(join(tmpdir(), "gammathink-session-cli-"));
  fixtures.push(parent);
  const state = join(parent, "state");
  await invoke(state, { command: "init", project: "synthetic-test" });
  const message = await invoke(state, { command: "message", timestamp: "1", text });
  return { state, message };
}
function plan(capture, kind, payload) {
  const support = ["s0"];
  return [
    "GAMMATHINK_TASK_IR 1",
    `CONTEXT ${JSON.stringify(capture.snapshot)}`,
    'ROOT "report"',
    `ENTITY ${JSON.stringify(["input", kind === "execute-code" ? "text" : kind === "retrieve" ? "query" : kind === "source-reason" ? "source-query" : ["compile-source", "visualize", "request-publication"].includes(kind) ? "object" : "problem", payload, support])}`,
    `ENTITY ${JSON.stringify(["result", "artifact", null, support])}`,
    `ENTITY ${JSON.stringify(["reply", "artifact", null, support])}`,
    `GOAL ${JSON.stringify(["work", [], [], kind === "execute-code" ? [["exit_code", "eq", 0]]
      : kind === "compile-source" ? [["source_materialized", "eq", true]] : [], [], support])}`,
    `GOAL ${JSON.stringify(["report", [], [], [], [], support])}`,
    `ACTION ${JSON.stringify(["compute", "work", kind, ["input"], ["result"], support])}`,
    `ACTION ${JSON.stringify(["describe", "report", "explain", ["result"], ["reply"], support])}`,
    "END", "",
  ].join("\n");
}
test("resource observations report real process and disk scope without task or model authority", async () => {
  const { state } = await setup("Observe local resources without executing a task.");
  const before = await invoke(state, { command: "capture-structured" });
  const result = await invoke(state, { command: "resources" });
  expect(result.model_loaded).toBe(false);
  expect(result.model_generations).toBe(0);
  expect(result.authority_changed).toBe(false);
  const resources = result.resources;
  expect(resources.profile).toBe("gammathink.application-resource-snapshot.v1");
  expect(resources.scope).toBe("current-command-process");
  expect(resources.process_resident_bytes).toBeGreaterThan(0);
  expect(resources.process_peak_rss_bytes).toBeGreaterThan(0);
  expect(resources.process_user_cpu_microseconds).toBeGreaterThanOrEqual(0);
  expect(resources.process_system_cpu_microseconds).toBeGreaterThanOrEqual(0);
  expect(resources.disk_capacity_bytes).toBeGreaterThan(0);
  expect(resources.disk_free_bytes).toBeGreaterThanOrEqual(0);
  expect(resources.disk_free_bytes).toBeLessThanOrEqual(resources.disk_capacity_bytes);
  expect(resources.disk_available_bytes).toBeGreaterThanOrEqual(0);
  expect(resources.disk_available_bytes).toBeLessThanOrEqual(resources.disk_free_bytes);
  expect(resources.other_processes_included).toBe(false);
  expect(resources.aggregate_memory_qualified).toBe(false);
  const after = await invoke(state, { command: "capture-structured" });
  expect(after.context).toEqual(before.context);
  expect(after.snapshot).toBe(before.snapshot);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(0);
  await invoke(state, { command: "resources", network_access: {} }, false);
  await invoke(state, { command: "resources", model: {} }, false);
});
async function admit(state, message, capture, proposal) {
  return invoke(state, {
    command: "admit", revision: 0, source_message: message.message,
    context: capture.context, snapshot: capture.snapshot, proposal,
  });
}
function editRecord(proposal, tag, id, change) {
  return proposal.split("\n").map(line => {
    if (!line.startsWith(`${tag} `)) return line;
    const row = JSON.parse(line.slice(tag.length + 1));
    if (row[0] !== id) return line;
    change(row);
    return `${tag} ${JSON.stringify(row)}`;
  }).join("\n");
}

test("native reasoning survives separate commands and delivers an indexed private response", async () => {
  const { state, message } = await setup("Robin flies. Check it twice and report the result.");
  const capture = await invoke(state, { command: "capture-structured" });
  expect(capture.counter).toBe("utf8-bytes-structured-only");
  expect(capture.model_calls).toBe(0);
  const problem = [
    "GAMMATHINK_DSL 1", 'ENTITY "robin"', 'PREDICATE ["flies",1]',
    'RULE [0,[0,false,[["c",0]]],[],["s0"]]',
    'GOAL {"goal":["atom",[0,false,[["c",0]]]],"supports":["s0"]}',
    'GOAL {"goal":["atom",[0,false,[["c",0]]]],"supports":["s0"]}', "END", "",
  ].join("\n");
  const accepted = await admit(state, message, capture, plan(capture, "reason", problem));
  const result = await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context, action: "compute",
  });
  expect(result.succeeded).toBe(true);
  expect(result.native_executions).toBe(1);
  expect(result.native_certificates).toHaveLength(2);
  expect(result.model_loaded).toBe(false);
  const explained = await invoke(state, {
    command: "step", revision: result.revision, context: capture.context, action: "describe",
  });
  const delivered = await invoke(state, {
    command: "deliver", revision: explained.revision, context: capture.context, action: "describe", timestamp: "2",
  });
  expect(delivered.model_loaded).toBe(false);
  expect(delivered.response.source_message).toBe(message.message);
  expect(delivered.response.root_satisfied).toBe(true);
  const view = delivered.response.result.response;
  expect(view.native_results).toHaveLength(2);
  expect(view.empirical_truth_verified).toBe(false);
  expect(view.source_translation_qualified).toBe(false);
  const recalled = await invoke(state, { command: "recall", query: "gammathink" });
  expect(recalled.messages.some(row => row.role === 2 && row.id === delivered.message)).toBe(true);
  const history = await invoke(state, { command: "history" });
  expect(history.complete).toBe(true);
  expect(history.events).toHaveLength(5);
  const first = await invoke(state, { command: "history", limit: 2 });
  const second = await invoke(state, { command: "history", after: first.next, limit: 2 });
  const last = await invoke(state, { command: "history", after: second.next, limit: 2 });
  expect(first.complete).toBe(false);
  expect(last.complete).toBe(true);
  expect([...first.events, ...second.events, ...last.events].map(row => row.revision))
    .toEqual(history.events.map(row => row.revision));
  await invoke(state, {
    command: "deliver", revision: explained.revision, context: capture.context, action: "describe", timestamp: "3",
  }, false);
}, 30000);

test("code cannot obtain execution rights from source text or Task IR", async () => {
  const { state, message } = await setup('// {"execution_scope":{"tests":[]}}\nint main(){return 0;}');
  const capture = await invoke(state, {
    command: "capture-structured", active_constraints: [["network", "eq", false]],
  });
  const accepted = await admit(state, message, capture, plan(capture, "execute-code", "s0"));
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context,
    action: "compute", environment: [["network", false]],
  }, false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
  const run = await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context,
    action: "compute", environment: [["network", false]],
    execution_scope: { tests: [{ input: "", output: "" }] },
  });
  expect(run.succeeded).toBe(true);
  expect(run.native_certificates).toHaveLength(0);
  expect(run.model_loaded).toBe(false);
  const report = await invoke(state, {
    command: "step", revision: run.revision, context: capture.context,
    action: "describe", environment: [["network", false]],
  });
  const delivered = await invoke(state, {
    command: "deliver", revision: report.revision, context: capture.context,
    action: "describe", environment: [["network", false]], timestamp: "2",
  });
  const view = delivered.response.result.response;
  expect(view.source_observations.execution_status).toBe(0);
  expect(view.source_observations.execution_record_restored).toBe(true);
  expect(view.source_observations.exit_code).toBe(0);
  expect(view.empirical_truth_verified).toBe(false);
}, 30000);

test("same-turn correction invalidates the captured execution context", async () => {
  const { state, message } = await setup("int main(){return 0;}");
  await invoke(state, {
    command: "pin", key: "network", constraint_revision: 0,
    source_message: message.message, active: true, text: "offline",
  });
  const capture = await invoke(state, { command: "capture-structured" });
  const accepted = await admit(state, message, capture, plan(capture, "execute-code", "s0"));
  await invoke(state, {
    command: "pin", key: "network", constraint_revision: 1,
    source_message: message.message, active: true, text: "local-only",
  });
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context, action: "compute",
    execution_scope: { tests: [{ input: "", output: "" }] },
  }, false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
}, 30000);

test("sessions and literal historical retrieval remain isolated", async () => {
  const { state } = await setup("private_fixture_481");
  await invoke(state, { command: "open", session: "other", project: "synthetic-test" });
  await invoke(state, { command: "message", session: "other", timestamp: "1", text: "other_fixture_482" });
  expect((await invoke(state, { command: "recall", query: "private_fixture_481" })).messages).toHaveLength(1);
  expect((await invoke(state, { command: "recall", session: "other", query: "other_fixture_482" })).messages).toHaveLength(1);
  expect((await invoke(state, { command: "recall", query: "other_fixture_482" })).messages).toHaveLength(0);
  expect((await invoke(state, { command: "recall", session: "other", query: "private_fixture_481" })).messages).toHaveLength(0);
  await invoke(state, { command: "message", timestamp: "2", text: "private_fixture_481" });
  const first = await invoke(state, { command: "recall", query: "private_fixture_481", limit: 1 });
  const second = await invoke(state, { command: "recall", query: "private_fixture_481", after: first.next, limit: 1 });
  expect(first.messages).toHaveLength(1);
  expect(first.complete).toBe(false);
  expect(second.messages).toHaveLength(1);
  expect(second.complete).toBe(true);
  expect(second.messages[0].id).toBeGreaterThan(first.messages[0].id);
}, 30000);

test("unknown fields and missing experimental models fail without executable task admission", async () => {
  const { state } = await setup("Translate this request faithfully.");
  await invoke(state, { command: "capture-structured", allow_everything: true }, false);
  await invoke(state, {
    command: "interpret", revision: 0,
    model: { id: "missing", path: join(state, "missing.gguf"), sha256: "01".repeat(32) },
  }, false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(0);
}, 30000);

test.each([1, 2])("complete ZIM input materializes scoped source knowledge without claiming fidelity or truth (profile %i)", async version => {
  const { state, message } = await setup("Compile the complete source and report.");
  const archive = join(state, "fixture.zim");
  const fixture = Bun.spawn([resolve("build/task-source-test"), "--fixture", archive], {
    stdout: "pipe", stderr: "pipe",
  });
  const finished = fixture.exited;
  const diagnostics = new Response(fixture.stderr).text();
  const fixtureOutput = new Response(fixture.stdout).text();
  const [status, error] = await Promise.all([finished, diagnostics, fixtureOutput]);
  expect(status, error).toBe(0);
  const imported = await invoke(state, { command: "read-source", archive, entry: "article" });
  const capture = await invoke(state, {
    command: "capture-structured", objects: { document: imported.source },
  });
  const accepted = await admit(state, message, capture, plan(capture, "compile-source", "document"));
  const sourceScope = {
    source: imported.source.id, context: imported.source_context, snapshot: imported.context_snapshot,
    ...(version === 2 ? { translation_profile: "gammathink.source-translation-input.v2" } : {}),
    archive_dsl: [
      `GAMMATHINK_DSL ${version}`, 'ENTITY "robin"', 'PREDICATE ["flies",1]',
      'RULE [0,[0,false,[["c",0]]],[],[[0,0,12,null]]]', "END", "",
    ].join("\n"),
  };
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context,
    action: "compute", source_scope: { ...sourceScope, snapshot: "changed" },
  }, false);
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context,
    action: "compute", source_scope: {
      ...sourceScope, translation_profile: "gammathink.source-translation-input.v3",
    },
  }, false);
  const { archive_dsl: _literal, ...translationScope } = sourceScope;
  await invoke(state, {
    command: "translate-source", revision: accepted.revision, context: capture.context,
    action: "compute", source_scope: translationScope,
    model: { id: "missing-source-control", path: join(state, "missing-source.gguf"), sha256: "01".repeat(32) },
  }, false);
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context, action: "compute",
    source_scope: { ...sourceScope, candidate: imported.source.id },
  }, false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
  const compiled = await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context,
    action: "compute", source_scope: sourceScope,
  });
  expect(compiled.succeeded).toBe(true);
  expect(compiled.artifact).not.toBe(compiled.receipt);
  expect(compiled.native_certificates).toHaveLength(0);
  expect(compiled.native_executions).toBe(0);
  expect(compiled.model_loaded).toBe(false);
  const reviewed = await invoke(state, {
    command: "review-source", revision: compiled.revision, context: capture.context, action: "compute",
  });
  expect(reviewed.review.complete_source).toBe("Robin flies.");
  expect(reviewed.review.archive_dsl).toBe(sourceScope.archive_dsl);
  expect(reviewed.review.module).toBe(compiled.artifact);
  expect(reviewed.review.receipt_format).toBe(`gammathink.task-source-compilation.v${version}`);
  expect(reviewed.review.parsed_json.theory.rules).toHaveLength(1);
  expect(reviewed.review.provenance).toHaveLength(1);
  expect(reviewed.review.provenance[0].length).toBe(12);
  expect(reviewed.review.source_translation_qualified).toBe(false);
  expect(reviewed.review.authority_changed).toBe(false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(3);
  const explained = await invoke(state, {
    command: "step", revision: compiled.revision, context: capture.context, action: "describe",
  });
  const delivered = await invoke(state, {
    command: "deliver", revision: explained.revision, context: capture.context, action: "describe", timestamp: "2",
  });
  const view = delivered.response.result.response;
  expect(view.source_observations.source_raw).toBe(imported.source.id);
  expect(view.source_observations.source_module).toBe(compiled.artifact);
  expect(view.source_observations.source_materialized).toBe(true);
  expect(view.source_observations.source_members).toBe(1);
  expect(view.source_observations.source_record_restored).toBe(true);
  expect(view.source_translation_qualified).toBe(false);
  expect(view.empirical_truth_verified).toBe(false);
  expect(view.native_results).toHaveLength(0);
}, 30000);

test("version2 source capture inspection stays private and rejects unknown formats", async () => {
  const { state } = await setup("Inspect locally recorded source capture metadata.");
  const fixture = Bun.spawn([resolve("build/source-profile-cli-fixture-v2"), join(state, "journal.db")], {
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const [status, stdout, diagnostics] = await Promise.all([
    fixture.exited, new Response(fixture.stdout).text(), new Response(fixture.stderr).text(),
  ]);
  expect({ status, diagnostics }).toEqual({ status: 0, diagnostics: "" });
  const records = JSON.parse(stdout);
  for (const record of records.private) {
    const observed = await invoke(state, { command: "read-artifact", id: record.id });
    expect(observed.artifact_format).toBe(record.format);
    expect(observed.payload.synthetic).toBe(true);
    expect(observed.historical_observation).toBe(true);
    expect(observed.publication_approved).toBe(false);
  }
  await invoke(state, { command: "read-artifact", id: records.public }, false);
  await invoke(state, { command: "read-artifact", id: records.unsupported }, false);
});

test.each(["visualize", "request-publication"])("private %s persists a readable artifact across command restarts", async kind => {
  const { state, message } = await setup("Inspect the selected source and report.");
  const archive = join(state, "review.zim");
  const fixture = Bun.spawn([resolve("build/task-source-test"), "--fixture", archive], {
    stdout: "pipe", stderr: "pipe",
  });
  const [status, diagnostics] = await Promise.all([
    fixture.exited, new Response(fixture.stderr).text(), new Response(fixture.stdout).text(),
  ]);
  expect(status, diagnostics).toBe(0);
  const imported = await invoke(state, { command: "read-source", archive, entry: "article" });
  const capture = await invoke(state, { command: "capture-structured", objects: { document: imported.source } });
  const accepted = await admit(state, message, capture, plan(capture, kind, "document"));
  await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context, action: "compute",
    execution_scope: { tests: [{ stdin: "", expected_stdout: "" }] },
  }, false);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
  const result = await invoke(state, {
    command: "step", revision: accepted.revision, context: capture.context, action: "compute",
  });
  expect(result.succeeded).toBe(true);
  expect(result.native_certificates).toHaveLength(0);
  expect(result.native_executions).toBe(0);
  expect(result.model_loaded).toBe(false);
  const view = await invoke(state, { command: "read-artifact", id: result.artifact });
  expect(view.historical_observation).toBe(true);
  expect(view.publication_approved).toBe(false);
  expect(view.payload.published).toBe(false);
  if (kind === "visualize") {
    expect(view.payload.root).toBe(imported.source.id);
    expect(view.payload.observational_only).toBe(true);
    expect(view.payload.nodes.find(node => node.id === imported.source.id).sources[0].entry).toBe("article");
  } else {
    expect(view.payload.selected_roots).toEqual([imported.source.id]);
    expect(view.payload.approved).toBe(false);
    expect(view.payload.status).toBe("pending-host-review");
  }
  const explained = await invoke(state, {
    command: "step", revision: result.revision, context: capture.context, action: "describe",
  });
  await invoke(state, {
    command: "deliver", revision: explained.revision, context: capture.context, action: "describe", timestamp: "2",
  });
  expect((await invoke(state, { command: "read-artifact", id: result.artifact })).payload).toEqual(view.payload);
  await invoke(state, { command: "read-artifact", id: imported.source.id }, false);
}, 30000);

test("local archive mounts persist and supply scoped reasoning without publication or network authority", async () => {
  // Given a complete source compiled in a different application store.
  const producer = await setup("Compile the complete source.");
  const archive = join(producer.state, "mounted.zim");
  const fixture = Bun.spawn([resolve("build/task-source-test"), "--fixture", archive], {
    stdout: "pipe", stderr: "pipe",
  });
  const [status, diagnostics] = await Promise.all([
    fixture.exited, new Response(fixture.stderr).text(), new Response(fixture.stdout).text(),
  ]);
  expect(status, diagnostics).toBe(0);
  const imported = await invoke(producer.state, { command: "read-source", archive, entry: "article" });
  const original = await invoke(producer.state, { command: "capture-structured", objects: { document: imported.source } });
  const admitted = await admit(producer.state, producer.message, original, plan(original, "compile-source", "document"));
  const compiled = await invoke(producer.state, {
    command: "step", revision: admitted.revision, context: original.context, action: "compute",
    source_scope: {
      source: imported.source.id, context: imported.source_context, snapshot: imported.context_snapshot,
      archive_dsl: [
        "GAMMATHINK_DSL 1", 'ENTITY "robin"', 'PREDICATE ["flies",1]',
        'RULE [0,[0,false,[["c",0]]],[],[[0,0,12,null]]]', "END", "",
      ].join("\n"),
    },
  });
  const directory = join(producer.state, "snapshot");
  const snapshot = await invoke(producer.state, {
    command: "snapshot-journal", destination: directory, name: "synthetic-library", revision: "1",
  });
  expect(snapshot.published).toBe(false);
  expect(snapshot.objects).toBeGreaterThan(3);
  await invoke(producer.state, {
    command: "snapshot-journal", destination: directory, name: "synthetic-library", revision: "1",
  }, false);
  const consumer = await setup("Check the source assertion that Robin flies and report.");
  expect((await invoke(consumer.state, { command: "mounts" })).mounts).toHaveLength(0);
  // When the host mounts an exact local immutable root.
  const mounted = await invoke(consumer.state, {
    command: "mount", alias: "library", directory, root: snapshot.root, generation: 0,
  });
  expect(mounted.generation).toBe(1);
  expect(mounted.root).toBe(snapshot.root);
  await invoke(consumer.state, {
    command: "mount", alias: "library", directory, root: "00".repeat(32), generation: 1,
  }, false);
  const current = await invoke(consumer.state, { command: "mounts" });
  expect(current.network_authorized).toBe(false);
  expect(current.mounts).toHaveLength(1);
  expect(current.mounts[0].root).toBe(snapshot.root);
  expect(current.mounts[0].generation).toBe(1);
  await invoke(consumer.state, { command: "unmount", alias: "library", generation: 0 }, false);
  const module = { id: compiled.artifact, kind: "CLAIM", format: "gammathink.archive-dsl.v1" };
  const capture = await invoke(consumer.state, { command: "capture-structured", objects: { module } });
  const task = await admit(consumer.state, consumer.message, capture,
    plan(capture, "source-reason", ["module", ["atom", [0, false, [["c", 0]]]]]));
  const native = { [compiled.artifact]: imported.source_context };
  const result = await invoke(consumer.state, {
    command: "step", revision: task.revision, context: capture.context, action: "compute", source_contexts: native,
  });
  // Then only the unchanged native core establishes a checked formal result.
  expect(result.succeeded).toBe(true);
  expect(result.native_executions).toBe(1);
  expect(result.native_certificates).toHaveLength(1);
  const explained = await invoke(consumer.state, {
    command: "step", revision: result.revision, context: capture.context, action: "describe", source_contexts: native,
  });
  await invoke(consumer.state, {
    command: "deliver", revision: explained.revision, context: capture.context, action: "describe",
    source_contexts: native, timestamp: "2",
  });
  const removed = await invoke(consumer.state, { command: "unmount", alias: "library", generation: 1 });
  expect(removed.mounted).toBe(false);
  expect(removed.generation).toBe(2);
  expect((await invoke(consumer.state, { command: "mounts" })).mounts).toHaveLength(0);
  await invoke(consumer.state, { command: "mount", alias: "library", directory, root: snapshot.root, generation: 0 }, false);
  const replaced = await invoke(consumer.state, { command: "mount", alias: "library", directory, root: snapshot.root, generation: 2 });
  expect(replaced.generation).toBe(3);
}, 30000);

test("same-turn refinement adds prerequisites without weakening original intent or reusing old scope", async () => {
  const { state, message } = await setup("int main(){return 0;}");
  await invoke(state, {
    command: "pin", key: "network", constraint_revision: 0,
    source_message: message.message, active: true, text: "offline",
  });
  const original = await invoke(state, {
    command: "capture-structured", active_constraints: [["network", "eq", false]],
  });
  const initial = editRecord(plan(original, "execute-code", "s0"), "GOAL", "report",
    row => { row[1] = [["work", "work"]]; });
  const accepted = await admit(state, message, original, initial);
  await invoke(state, {
    command: "pin", key: "network", constraint_revision: 1,
    source_message: message.message, active: true, text: "local-only",
  });
  const fresh = await invoke(state, {
    command: "capture-structured", active_constraints: [["network", "eq", false]],
  });
  let proposal = initial.split("\n").map(line => line.startsWith("CONTEXT ")
    ? `CONTEXT ${JSON.stringify(fresh.snapshot)}` : line).join("\n");
  proposal = editRecord(proposal, "GOAL", "report", row => { row[1] = [["audit", "work", "work"]]; });
  proposal = proposal.replace("\nEND\n", [
    '\nENTITY ["audit_result","artifact",null,["s0"]]',
    'GOAL ["audit",[],[],[["exit_code","eq",0]],[],["s0"]]',
    'ACTION ["auditcode","audit","execute-code",["input"],["audit_result"],["s0"]]',
    "END", "",
  ].join("\n"));
  const refine = changes => invoke(state, {
    command: "refine", revision: accepted.revision, context: fresh.context,
    snapshot: fresh.snapshot, proposal, ...changes,
  }, false);
  await refine({ proposal: editRecord(proposal, "GOAL", "work", row => { row[3] = [["exit_code", "eq", 1]]; }) });
  await refine({ proposal: editRecord(proposal, "GOAL", "report", row => { row[1] = [["audit", "work"]]; }) });
  await refine({ proposal: editRecord(proposal, "ACTION", "compute", row => { row[2] = "generate-code"; }) });
  const weaker = await invoke(state, { command: "capture-structured" });
  await refine({
    context: weaker.context, snapshot: weaker.snapshot,
    proposal: proposal.split("\n").map(line => line.startsWith("CONTEXT ")
      ? `CONTEXT ${JSON.stringify(weaker.snapshot)}` : line).join("\n"),
  });
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
  const refined = await invoke(state, {
    command: "refine", revision: accepted.revision, context: fresh.context, snapshot: fresh.snapshot, proposal,
  });
  expect(refined.source_message).toBe(message.message);
  expect(refined.task).not.toBe(accepted.task);
  expect(refined.revision).toBe(accepted.revision + 1);
  expect(refined.native_certificates).toHaveLength(0);
  await invoke(state, {
    command: "step", revision: accepted.revision, context: original.context, action: "compute",
    environment: [["network", false]], execution_scope: { tests: [{ input: "", output: "" }] },
  }, false);
  const audit = await invoke(state, {
    command: "step", revision: refined.revision, context: fresh.context, action: "auditcode",
    environment: [["network", false]], execution_scope: { tests: [{ input: "", output: "" }] },
  });
  const computed = await invoke(state, {
    command: "step", revision: audit.revision, context: fresh.context, action: "compute",
    environment: [["network", false]], execution_scope: { tests: [{ input: "", output: "" }] },
  });
  const explained = await invoke(state, {
    command: "step", revision: computed.revision, context: fresh.context, action: "describe", environment: [["network", false]],
  });
  const delivered = await invoke(state, {
    command: "deliver", revision: explained.revision, context: fresh.context,
    action: "describe", environment: [["network", false]], timestamp: "2",
  });
  expect(delivered.response.source_message).toBe(message.message);
  expect(delivered.response.root_satisfied).toBe(true);
}, 30000);

test("archive provider limits and missing mounted roots never silently drop a library", async () => {
  // Given one explicit private snapshot and the full bounded set of mounts.
  const { state, message } = await setup("Robin flies. Check the assertion.");
  const directory = join(state, "bounded-snapshot");
  const snapshot = await invoke(state, {
    command: "snapshot-journal", destination: directory, name: "empty-local-control", revision: "1",
  });
  expect(snapshot.objects).toBe(0);
  for (let index = 0; index < 15; index++) {
    await invoke(state, { command: "mount", alias: `library-${index}`, directory, root: snapshot.root, generation: 0 });
  }
  await invoke(state, { command: "mount", alias: "overflow", directory, root: snapshot.root, generation: 0 }, false);
  expect((await invoke(state, { command: "mounts" })).mounts).toHaveLength(15);
  const capture = await invoke(state, { command: "capture-structured" });
  const problem = [
    "GAMMATHINK_DSL 1", 'ENTITY "robin"', 'PREDICATE ["flies",1]',
    'RULE [0,[0,false,[["c",0]]],[],["s0"]]',
    'GOAL {"goal":["atom",[0,false,[["c",0]]]],"supports":["s0"]}', "END", "",
  ].join("\n");
  // Admission is local and preserves the intent even when a library vanishes.
  await rm(directory, { recursive: true });
  const task = await invoke(state, {
    command: "admit", revision: 0, source_message: message.message, context: capture.context,
    snapshot: capture.snapshot, proposal: plan(capture, "reason", problem),
  });
  // When execution opens the drive, it cannot return a partial provider set.
  await invoke(state, {
    command: "step", revision: task.revision, context: capture.context, action: "compute",
  }, false);
  // Then the original intent remains unexecuted and host management works.
  expect((await invoke(state, { command: "history" })).events).toHaveLength(1);
  expect((await invoke(state, { command: "mounts" })).mounts).toHaveLength(15);
  const removed = await invoke(state, { command: "unmount", alias: "library-0", generation: 1 });
  expect(removed.mounted).toBe(false);
  expect((await invoke(state, { command: "mounts" })).mounts).toHaveLength(14);
}, 30000);

test("remote reads require explicit capabilities, cache offline, and disclose queries only by approval", async () => {
  // Given a private synthetic snapshot served only by this loopback fixture.
  const producer = await setup("Store this synthetic source.");
  const archive = join(producer.state, "network.zim");
  const fixture = Bun.spawn([resolve("build/task-source-test"), "--fixture", archive], {
    stdout: "pipe", stderr: "pipe",
  });
  const [status, diagnostics] = await Promise.all([
    fixture.exited, new Response(fixture.stderr).text(), new Response(fixture.stdout).text(),
  ]);
  expect(status, diagnostics).toBe(0);
  const imported = await invoke(producer.state, { command: "read-source", archive, entry: "article" });
  const directory = join(producer.state, "network-snapshot");
  const snapshot = await invoke(producer.state, {
    command: "snapshot-journal", destination: directory, name: "synthetic-network", revision: "1",
  });
  const rawCid = hex => {
    const bytes = Buffer.concat([Buffer.from([1, 0x55, 0x12, 0x20]), Buffer.from(hex, "hex")]);
    const alphabet = "abcdefghijklmnopqrstuvwxyz234567";
    let bits = 0, word = 0, result = "b";
    for (const byte of bytes) {
      word = (word << 8) | byte; bits += 8;
      while (bits >= 5) { bits -= 5; result += alphabet[(word >>> bits) & 31]; word &= (1 << bits) - 1; }
    }
    if (bits) result += alphabet[(word << (5 - bits)) & 31];
    return result;
  };
  const blocks = new Map();
  for await (const name of new Bun.Glob("*.gksb").scan(directory))
    blocks.set(`/ipfs/${rawCid(name.slice(0, -5))}?format=raw`, Bun.file(join(directory, name)));
  const requests = [];
  let corruptQuery = false;
  const server = Bun.serve({
    hostname: "127.0.0.1", port: 0,
    async fetch(request) {
      const url = new URL(request.url);
      requests.push(request.method);
      if (request.method === "GET") {
        const block = blocks.get(url.pathname + url.search);
        return block ? new Response(block) : new Response("Missing synthetic block", { status: 404 });
      }
      if (request.method !== "POST" || url.pathname !== "/gammathink/query/v1")
        return new Response("Unexpected fixture request", { status: 400 });
      const child = Bun.spawn([resolve("build/remote-query-response-fixture"), directory, snapshot.root], {
        stdin: "pipe", stdout: "pipe", stderr: "pipe",
      });
      const pending = Promise.all([
        child.exited, new Response(child.stdout).arrayBuffer(), new Response(child.stderr).text(),
      ]);
      child.stdin.write(new Uint8Array(await request.arrayBuffer())); child.stdin.end();
      const [code, bytes, error] = await pending;
      if (code !== 0) return new Response(error, { status: 500 });
      return new Response(corruptQuery ? new Uint8Array([0]) : bytes);
    },
  });
  try {
    const access = {
      enabled: true,
      sources: [{ key: "library", origin: server.url.origin, network: true, allow_insecure_loopback: true }],
      cache_limits: { ram_bytes: 4 * 1024 * 1024, ssd_bytes: 16 * 1024 * 1024 },
    };
    const offline = { ...access, enabled: false };
    const consumer = await setup("Inspect this selected synthetic source and report.");
    const mount = { command: "mount-remote", alias: "library", root: snapshot.root, generation: 0,
      source: "library", backend: "blocks" };
    await invoke(consumer.state, mount, false);
    await invoke(consumer.state, { ...mount, network_access: offline }, false);
    await invoke(consumer.state, {
      ...mount, network_access: { ...access, sources: [{ ...access.sources[0], network: false }] },
    }, false);
    expect(requests).toEqual([]);
    expect((await invoke(consumer.state, { command: "mounts" })).mounts).toHaveLength(0);
    // When the host explicitly enables one exact read-only source.
    const mounted = await invoke(consumer.state, { ...mount, network_access: access });
    expect(mounted.network_usage.requests).toBeGreaterThan(0);
    expect(mounted.network_usage.query_bytes).toBe(0);
    const capture = await invoke(consumer.state, {
      command: "capture-structured", objects: { document: imported.source },
    });
    const admission = { command: "admit", revision: 0, source_message: consumer.message.message,
      context: capture.context, snapshot: capture.snapshot, proposal: plan(capture, "visualize", "document") };
    const before = requests.length;
    const task = await invoke(consumer.state, admission);
    expect(requests).toHaveLength(before);
    await invoke(consumer.state, {
      command: "step", revision: task.revision, context: capture.context, action: "compute",
    }, false);
    expect(requests).toHaveLength(before);
    const inspected = await invoke(consumer.state, {
      command: "step", revision: task.revision, context: capture.context, action: "compute", network_access: access,
    });
    expect(inspected.succeeded).toBe(true);
    expect(inspected.native_certificates).toHaveLength(0);
    expect(requests.every(method => method === "GET")).toBe(true);
    // Then cached blocks support actual restart and delivery with no network.
    const fetched = requests.length;
    const explained = await invoke(consumer.state, {
      command: "step", revision: inspected.revision, context: capture.context, action: "describe", network_access: offline,
    });
    expect(explained.network_usage.requests).toBe(0);
    expect(explained.network_usage.sources[0].ssd_hits).toBeGreaterThan(0);
    expect(explained.network_usage.sources[0].ssd_bytes).toBeLessThanOrEqual(access.cache_limits.ssd_bytes);
    const delivered = await invoke(consumer.state, {
      command: "deliver", revision: explained.revision, context: capture.context, action: "describe",
      timestamp: "2", network_access: offline,
    });
    expect(delivered.network_usage.requests).toBe(0);
    expect(requests).toHaveLength(fetched);

    const restricted = await setup("Inspect the source without network access.");
    await invoke(restricted.state, { ...mount, network_access: access });
    const restrictedCapture = await invoke(restricted.state, {
      command: "capture-structured", objects: { document: imported.source },
      active_constraints: [["network", "eq", false]],
    });
    const restrictedTask = await invoke(restricted.state, {
      command: "admit", revision: 0, source_message: restricted.message.message,
      context: restrictedCapture.context, snapshot: restrictedCapture.snapshot,
      proposal: plan(restrictedCapture, "visualize", "document"), network_access: access,
    });
    const beforeRestricted = requests.length;
    await invoke(restricted.state, {
      command: "step", revision: restrictedTask.revision, context: restrictedCapture.context,
      action: "compute", environment: [["network", false]], network_access: access,
    }, false);
    await invoke(restricted.state, {
      command: "step", revision: restrictedTask.revision, context: restrictedCapture.context,
      action: "compute", network_access: access,
    }, false);
    expect(requests).toHaveLength(beforeRestricted);
    expect((await invoke(restricted.state, { command: "history" })).events).toHaveLength(1);

    const queryUser = await setup("Retrieve the selected source records and report.");
    const beforeQueryAccess = requests.length;
    await invoke(queryUser.state, { ...mount, backend: "query", network_access: access }, false);
    expect(requests).toHaveLength(beforeQueryAccess);
    const disclosed = { ...access, sources: [{ ...access.sources[0], allow_query_disclosure: true }] };
    await invoke(queryUser.state, { ...mount, backend: "query", network_access: disclosed });
    const queryCapture = await invoke(queryUser.state, { command: "capture-structured" });
    const queryTask = await invoke(queryUser.state, {
      command: "admit", revision: 0, source_message: queryUser.message.message, context: queryCapture.context,
      snapshot: queryCapture.snapshot, network_access: disclosed,
      proposal: plan(queryCapture, "retrieve", ["source", imported.source_context.views[0][2], null]),
    });
    const queried = await invoke(queryUser.state, {
      command: "step", revision: queryTask.revision, context: queryCapture.context, action: "compute",
      network_access: disclosed,
    });
    expect(queried.succeeded).toBe(true);
    expect(queried.network_usage.query_bytes).toBeGreaterThan(0);
    expect(requests.some(method => method === "POST")).toBe(true);
    const history = await invoke(queryUser.state, { command: "history" });
    corruptQuery = true;
    await invoke(queryUser.state, {
      command: "step", revision: queried.revision, context: queryCapture.context, action: "describe",
      network_access: disclosed,
    }, false);
    expect((await invoke(queryUser.state, { command: "history" })).events).toEqual(history.events);
    corruptQuery = false;
    const beforeOffline = requests.length;
    await invoke(queryUser.state, {
      command: "step", revision: queried.revision, context: queryCapture.context, action: "describe",
      network_access: { ...disclosed, enabled: false },
    }, false);
    expect(requests).toHaveLength(beforeOffline);
    // A query binding never downgrades itself; only this explicit host remount
    // selects local evaluation of the cached immutable block indexes.
    await invoke(queryUser.state, { ...mount, generation: 1, network_access: offline });
    const cached = await invoke(queryUser.state, {
      command: "step", revision: queried.revision, context: queryCapture.context, action: "describe", network_access: offline,
    });
    expect(cached.network_usage.requests).toBe(0);
    expect(requests).toHaveLength(beforeOffline);
  } finally { server.stop(true); }
}, 30000);

test("actual observations remain separate from provisional hypotheses and scientific confirmation", async () => {
  // Given an explicit synthetic formal goal, not an English translation.
  const { state } = await setup("Propose an experiment and record native resource observations.");
  const fixture = Bun.spawn([resolve("build/experiments-test"), "--fixture", state], {
    stdout: "pipe", stderr: "pipe",
  });
  const [status, source, diagnostics] = await Promise.all([
    fixture.exited, new Response(fixture.stdout).text(), new Response(fixture.stderr).text(),
  ]);
  expect(status, diagnostics).toBe(0);
  const hypotheses = await invoke(state, { command: "hypothesize", goal: JSON.parse(source).goal });
  expect(hypotheses.formally_supported).toBe(false);
  expect(hypotheses.empirical_truth_verified).toBe(false);
  expect(hypotheses.hypotheses).toHaveLength(1);
  expect(hypotheses.alternatives[0].jointly_required).toEqual([
    hypotheses.hypotheses[0], hypotheses.hypotheses[0],
  ]);
  const proposed = await invoke(state, {
    command: "propose-experiment", hypotheses: hypotheses.alternatives[0].jointly_required,
    protocol: {
      name: "native-memory-observation", revision: "1", instrument: "process-memory-v1", trials: 2,
      expected: [["resident_bytes", "gt", 0], ["peak_bytes", "gt", 0]],
    },
  });
  expect(proposed.observation_authorized).toBe(false);
  // When no host instrument scope is supplied, the proposal cannot run.
  await invoke(state, { command: "measure-experiment", experiment: proposed.experiment }, false);
  await invoke(state, {
    command: "measure-experiment", experiment: proposed.experiment,
    observation_scope: { instrument: "arbitrary-command" },
  }, false);
  const measured = await invoke(state, {
    command: "measure-experiment", experiment: proposed.experiment,
    observation_scope: { instrument: "process-memory-v1", limits: { encoded_bytes: 1048576 } },
  });
  expect(measured.measurement_basis).toBe(1);
  expect(measured.experiment_stop).toBe(1);
  expect(measured.criterion_match).toBe(1);
  expect(measured.attempted_trials).toBe(2);
  expect(measured.observations).toHaveLength(2);
  expect(measured.logical_certificates).toHaveLength(0);
  expect(measured.empirical_truth_verified).toBe(false);
  expect(measured.hypothesis_promoted).toBe(false);
  expect(measured.domain_review_required).toBe(true);
  expect(measured.model_loaded).toBe(false);
  // Then a new process reviews exact recorded values without resampling.
  const reviewed = await invoke(state, {
    command: "review-experiment", experiment: proposed.experiment, receipt: measured.receipt,
  });
  expect(reviewed.observations).toEqual(measured.observations);
  expect(reviewed.scope).toBe(measured.scope);
  expect(reviewed.historical_observation).toBe(true);
  expect(reviewed.authority_changed).toBe(false);
  expect(reviewed.domain_review_required).toBe(true);
  for (const observation of reviewed.recorded_observations) {
    expect(observation.instrument).toBe("process-memory-v1");
    expect(new Map(observation.values).get("resident_bytes")).toBeGreaterThan(0);
    expect(new Map(observation.values).get("peak_bytes")).toBeGreaterThan(0);
    expect(observation.digest).toMatch(/^[0-9a-f]{64}$/);
  }
  const again = await invoke(state, {
    command: "review-experiment", experiment: proposed.experiment, receipt: measured.receipt,
  });
  expect(again.recorded_observations).toEqual(reviewed.recorded_observations);
  expect((await invoke(state, { command: "history" })).events).toHaveLength(0);
}, 30000);
