import { afterEach, expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const binary = resolve(process.env.GAMMATHINK_SESSION_BINARY ?? "build/gammathink-session");
const fixtures = [];
afterEach(async () => {
  for (const path of fixtures.splice(0)) await rm(path, { recursive: true, force: true });
});
async function invoke(state, command, success = true) {
  const child = Bun.spawn([binary, state, "-"], {
    stdin: "pipe", stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(30000),
  });
  const done = child.exited;
  const stdout = new Response(child.stdout).text();
  const stderr = new Response(child.stderr).text();
  child.stdin.write(JSON.stringify({ version: 1, session: "a", ...command }));
  child.stdin.end();
  const [code, output, error] = await Promise.all([done, stdout, stderr]);
  expect(code, error || output).toBe(success ? 0 : 1);
  return JSON.parse(output);
}
async function setup() {
  const parent = await mkdtemp(join(tmpdir(), "context-documents-cli-"));
  fixtures.push(parent);
  const state = join(parent, "state");
  await invoke(state, { command: "init", project: "project" });
  await invoke(state, { command: "message", timestamp: "1", text: "Review the archive design." });
  return state;
}
const document = {
  command: "import-context", key: "design", uri: "file:///private/design.txt",
  timestamp: "2026-11-16", text: "The archive keeps source provenance.",
  kind: 1, attribution: 1, document_revision: 0, shared: true, active: true,
};
test("private documents enter attributed bounded context without becoming instructions or knowledge", async () => {
  const state = await setup();
  const imported = await invoke(state, document);
  expect(imported.document_revision).toBe(1);
  expect(imported.knowledge_accepted).toBe(false);
  const found = await invoke(state, { command: "search-context", query: "archive" });
  expect(found.complete).toBe(true);
  expect(found.excerpts).toHaveLength(1);
  const source = found.excerpts[0];
  expect(source.source_hash).toBe(imported.content_hash);
  expect(source.content_hash).toBe(imported.content_hash);
  expect(source.source_bytes).toBe(Buffer.byteLength(document.text));
  expect(source.uri).toBe(document.uri);
  expect(source.offset).toBe(0);
  const capture = await invoke(state, { command: "capture-structured", document_query: "archive" });
  expect(capture.model_calls).toBe(0);
  const context = JSON.parse(capture.translation_input.conversation);
  expect(context.schema).toBe(2);
  expect(context.entries.map(entry => entry.priority)).toEqual([1, 7]);
  expect(context.entries[1].document.uri).toBe(document.uri);
  expect(context.entries[1].document.source_hash).toBe(imported.content_hash);
  expect(context.entries[1].document.attribution).toBe(1);
  expect(context.entries[1].message).toBe(0);
  expect(context.entries[1].role).toBeUndefined();
  expect((await invoke(state, { command: "history" })).events).toEqual([]);
  await invoke(state, { ...document, document_revision: 0 }, false);
  const changed = await invoke(state, {
    ...document, document_revision: 1, text: "The archive has a corrected source.", attribution: 2,
  });
  expect(changed.document_revision).toBe(2);
  const newer = await invoke(state, { command: "capture-structured", document_query: "archive" });
  expect(newer.snapshot).not.toBe(capture.snapshot);
  expect((await invoke(state, { command: "search-context", query: "provenance" })).excerpts).toEqual([]);
  await invoke(state, { ...document, document_revision: 2, active: false });
  expect((await invoke(state, { command: "search-context", query: "archive" })).excerpts).toEqual([]);
});
test("document recall isolates projects and sessions while shared project records remain visible", async () => {
  const state = await setup();
  await invoke(state, document);
  await invoke(state, { ...document, key: "private", shared: false, text: "private archive preference", kind: 2 });
  await invoke(state, { command: "open", session: "b", project: "project" });
  const peer = await invoke(state, { command: "search-context", session: "b", query: "archive" });
  expect(peer.excerpts).toHaveLength(1);
  expect(peer.excerpts[0].key).toBe("design");
  await invoke(state, { command: "open", session: "c", project: "other-project" });
  expect((await invoke(state, { command: "search-context", session: "c", query: "archive" })).excerpts).toEqual([]);
  const limited = await invoke(state, { command: "search-context", query: "archive", limit: 1 });
  expect(limited.complete).toBe(false);
  expect(limited.excerpts[0].key).toBe("private");
  expect((await invoke(state, { command: "search-context", query: "archive OR private" })).excerpts).toEqual([]);
  await invoke(state, { ...document, key: "forged", project: "other-project" }, false);
});
