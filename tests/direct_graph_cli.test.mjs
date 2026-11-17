import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const root = resolve(import.meta.dir, "..");
const binary = join(root, "build/axiom");
async function execute(args, input) {
  const child = Bun.spawn([binary, ...args], {
    cwd: root, stdin: input === undefined ? "ignore" : new Blob([input]),
    stdout: "pipe", stderr: "pipe", signal: AbortSignal.timeout(10000),
  });
  const [code, bytes, stderr] = await Promise.all([
    child.exited, new Response(child.stdout).arrayBuffer(), new Response(child.stderr).text(),
  ]);
  expect({ code, stderr }).toEqual({ code: 0, stderr: "" });
  return new Uint8Array(bytes);
}
const text = bytes => new TextDecoder().decode(bytes);

test("recursive relational DSL produces the same checked graph through binary transport", async () => {
  const module = join(root, "modules/relations.axioms");
  const direct = text(await execute(["prove-rel", module, "ancestor(ana,cy)"]));
  const request = await execute(["compile-rel", module, "ancestor(ana,cy)"]);
  const reply = await execute(["run"], request);
  const decoded = text(await execute(["decode"], reply));
  expect(decoded).toBe(direct);
  expect(decoded).toContain("status: PROVED");
  expect(decoded).toContain("goal proof root:");
  expect(text(await execute(["prove-rel", module, "ancestor(cy,ana)"]))).toContain("status: UNKNOWN");
});

test("removing either jointly necessary premise changes the direct graph outcome", async () => {
  const directory = await mkdtemp(join(tmpdir(), "direct-graph-"));
  try {
    const source = [
      "axioms 1 joint_graph 1.0",
      "rule cycle -> destination | cyclic branch",
      "rule destination -> cycle | cycle has no originating evidence",
      "rule absent -> destination | missing alternative",
      "rule start & bridge -> destination | both premises required",
      "fact start | explicit source A",
      "fact bridge | explicit source B",
      "",
    ].join("\n");
    const module = join(directory, "joint.axioms");
    await Bun.write(module, source);
    expect(text(await execute(["prove", module, "destination"]))).toContain("status: PROVED");
    for (const premise of ["fact start | explicit source A\n", "fact bridge | explicit source B\n"]) {
      await Bun.write(module, source.replace(premise, ""));
      expect(text(await execute(["prove", module, "destination"]))).toContain("status: UNKNOWN");
    }
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});

test("explicit conflicting evidence preserves both roots without inventing missing facts", async () => {
  const module = join(root, "modules/contradiction.axioms");
  const result = text(await execute(["prove", module, "signal"]));
  expect(result).toContain("status: CONTRADICTION");
  expect(result).toContain("goal proof root:");
  expect(result).toContain("opposite proof root:");
  const missing = text(await execute(["prove", join(root, "modules/branching.axioms"), "missing"]));
  expect(missing).toContain("status: UNKNOWN");
});
