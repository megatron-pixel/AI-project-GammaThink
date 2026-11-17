import { expect, test } from "bun:test";
import { chmod, copyFile, mkdir, mkdtemp, readdir, rm } from "node:fs/promises";
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

test("retention plans preserve evidence and cache pruning requires explicit apply", async () => {
  const temporary = await mkdtemp(join(tmpdir(), "gammathink-maintenance-"));
  try {
    const journal = join(temporary, "journal.db");
    const first = await invoke("gammathink", ["solve", join(directory, "examples/task-context.json"),
      join(directory, "examples/task-request.dsl"), journal]);
    const root = first.slots[0].request;
    const plan = await invoke("knowledge-maintain", ["plan", journal, root]);
    expect(plan.status).toBe("complete");
    expect(plan.retained.map(record => record.id)).toContain(root);
    expect(plan.deletion_authorized).toBe(false);
    const missing = await invoke("knowledge-maintain", ["plan", journal, "0".repeat(64), root]);
    expect(missing.status).toBe("dependencies_unavailable");
    expect(missing.unavailable).toEqual(["0".repeat(64)]);
    expect(missing.optional_omissions).toEqual([]);
    const snapshot = join(temporary, "snapshot");
    const compiled = await invoke("gammathink", ["snapshot", journal, snapshot, "maintenance-test", "1"]);
    const archived = await invoke("knowledge-maintain", ["plan", journal, root,
      "--snapshot", snapshot, compiled.root]);
    expect(archived.retained).toEqual(plan.retained);
    const cache = join(temporary, "cache");
    await mkdir(cache, { mode: 0o700 });
    const names = (await readdir(snapshot)).filter(name => name.endsWith(".gksb"));
    expect(names.length).toBeGreaterThan(0);
    expect(names.length).toBeLessThanOrEqual(32);
    for (const name of names) {
      await copyFile(join(snapshot, name), join(cache, name));
      await chmod(join(cache, name), 0o400);
    }
    const dry = await invoke("knowledge-maintain", ["prune", cache, "-"]);
    expect(dry.dry_run).toBe(true);
    expect(dry.evicted).toBe(0);
    expect(dry.evictable.length).toBe(names.length);
    expect((await readdir(cache)).filter(name => name.endsWith(".gksb")).length).toBe(names.length);
    const applied = await invoke("knowledge-maintain", ["prune", cache, "-", "--apply"]);
    expect(applied.dry_run).toBe(false);
    expect(applied.evicted).toBe(names.length);
    expect(applied.complete).toBe(true);
    expect((await readdir(cache)).filter(name => name.endsWith(".gksb"))).toEqual([]);
    expect((await readdir(snapshot)).filter(name => name.endsWith(".gksb"))).toEqual(names);
    const after = await invoke("knowledge-maintain", ["plan", journal, root]);
    expect(after.retained).toEqual(plan.retained);
    const compacted = await invoke("knowledge-maintain", ["compact", journal, join(temporary, "cold")]);
    expect(compacted.original_untouched).toBe(true);
    expect(compacted.published).toBe(false);
    expect(compacted.assessments).toBeGreaterThan(0);
    const cold = await invoke("knowledge-maintain", ["plan", compacted.database, root]);
    expect(cold.retained).toEqual(plan.retained);
    const replayed = await invoke("gammathink", ["resume", root, compacted.database, snapshot, compiled.root]);
    expect(replayed.status).toBe("checked");
    expect(replayed.reused).toBe(true);
    expect(replayed.native_executions).toBe(0);
    await invoke("knowledge-maintain", ["compact", journal, join(temporary, "cold")], false);
    await invoke("knowledge-maintain", ["prune", cache, "-", "--delete"], false);
    await invoke("knowledge-maintain", ["plan", join(temporary, "absent.db"), root], false);
  } finally {
    await rm(temporary, { recursive: true });
  }
}, 60000);
