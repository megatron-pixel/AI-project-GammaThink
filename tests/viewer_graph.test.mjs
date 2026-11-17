import { expect, test } from "bun:test";
import { boundedGraph, recordedEdges } from "../viewer/graph.mjs";

const node = (id, fields = {}) => ({ id, links: [], derivation: null, ...fields });
test("viewer retains joint premise occurrences and distinct alternative derivations", () => {
  const first = node("d", { derivation: {
    conclusion: "c", rule: "r", premises: ["b", "a", "b"], assumptions: ["s"], evidence: ["e"],
  } });
  const alternative = node("other", {
    links: [{ relationship: "ALTERNATIVE_TO", target: "d" }],
    derivation: { conclusion: "c", rule: "r", premises: ["a"], assumptions: [], evidence: [] },
  });
  const edges = [...recordedEdges(first)];
  expect(edges.filter(edge => edge.role === "PREMISE").map(edge => [edge.target, edge.ordinal, edge.joint]))
    .toEqual([["b", 0, "d"], ["a", 1, "d"], ["b", 2, "d"]]);
  const nodes = new Map([first, alternative, node("c")].map(value => [value.id, value]));
  const collapsed = boundedGraph(nodes, "c", new Set(["c"]));
  expect(collapsed.ids).toEqual(["c", "d", "other"]);
  const expanded = boundedGraph(nodes, "c", new Set(["c", "d", "other"]));
  expect(expanded.ids).toContain("s");
  expect(expanded.ids).toContain("e");
  expect(expanded.edges.filter(edge => edge.role === "PREMISE")).toHaveLength(4);
  expect(expanded.edges.find(edge => edge.role === "ALTERNATIVE_TO").source).toBe("other");
  expect(nodes.size).toBe(3);
});

test("viewer bounds cycles, unexpanded branches and graphical node/edge counts", () => {
  const nodes = new Map([
    node("a", { links: [{ relationship: "DEPENDS_ON", target: "b" }] }),
    node("b", { links: [{ relationship: "CONTRADICTS", target: "a" },
      { relationship: "DEPENDS_ON", target: "unloaded" }] }),
  ].map(value => [value.id, value]));
  expect(boundedGraph(nodes, "a", new Set()).ids).toEqual(["a"]);
  const graph = boundedGraph(nodes, "a", new Set(["a", "b"]));
  expect(graph.ids).toEqual(["a", "b", "unloaded"]);
  expect(graph.edges.find(edge => edge.role === "CONTRADICTS")).toEqual({
    source: "b", target: "a", role: "CONTRADICTS", ordinal: 0,
  });
  expect(boundedGraph(nodes, "a", new Set(["a", "b"]), 2).limited).toBe(true);
  expect(boundedGraph(nodes, "a", new Set(["a", "b"]), 80, 1).edges).toHaveLength(1);
});

test("large joint groups are view-only and retain all duplicate occurrences", () => {
  const premises = Array.from({ length: 128 }, (_, i) => "p" + i);
  premises.push("p0", "p1");
  const root = node("root", { derivation: {
    conclusion: "conclusion", rule: "rule", premises, assumptions: ["assumption"], evidence: ["evidence"],
  } });
  const nodes = new Map([[root.id, root]]);
  const graph = boundedGraph(nodes, "root", new Set(["root"]));
  expect(graph.ids[1]).toBe("view:premises:root");
  expect(graph.groups.get(graph.ids[1]).members).toEqual(premises);
  expect(graph.groups.get(graph.ids[1]).owner).toBe("root");
  expect(graph.edges.find(edge => edge.role === "JOINT_PREMISES").viewOnly).toBe(true);
  expect(graph.edges.some(edge => edge.role === "ASSUMPTION")).toBe(true);
  expect(graph.edges.some(edge => edge.role === "EVIDENCE")).toBe(true);
  expect(nodes.size).toBe(1);
  expect(nodes.get("root").derivation.premises).toHaveLength(130);
  expect([...recordedEdges(root)].filter(edge => edge.role === "PREMISE")).toHaveLength(130);
  expect(graph.limited).toBe(false);
});
