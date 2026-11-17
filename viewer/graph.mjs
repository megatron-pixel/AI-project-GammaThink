// Presentation only: preserve recorded roles and ordered occurrences. Nothing
// here infers a claim, evaluates a rule, or converts missing data to false.
export function* recordedEdges(node) {
  for (let i = 0; i < node.links.length; ++i) {
    const link = node.links[i];
    yield { source: node.id, target: link.target, role: link.relationship, ordinal: i };
  }
  if (!node.derivation) return;
  const d = node.derivation;
  yield { source: node.id, target: d.conclusion, role: "PRODUCES", ordinal: 0 };
  yield { source: node.id, target: d.rule, role: "RULE", ordinal: 0 };
  for (const [role, ids] of [["PREMISE", d.premises], ["ASSUMPTION", d.assumptions], ["EVIDENCE", d.evidence]])
    for (let i = 0; i < ids.length; ++i)
      yield { source: node.id, target: ids[i], role, ordinal: i, joint: node.id };
}

export function boundedGraph(nodes, root, expanded, maximumNodes = 80, maximumEdges = 768) {
  const edges = [];
  const neighbors = new Map();
  const groups = new Map();
  let limited = false;
  const addEdge = edge => {
    if (edges.length === maximumEdges) { limited = true; return false; }
    edges.push(edge);
    for (const [from, to] of [[edge.source, edge.target], [edge.target, edge.source]]) {
      if (!neighbors.has(from)) neighbors.set(from, new Set());
      neighbors.get(from).add(to);
    }
    return true;
  };
  for (const node of nodes.values()) {
    let view = node;
    if (node.derivation && node.derivation.premises.length > 16) {
      const id = "view:premises:" + node.id;
      groups.set(id, { owner: node.id, members: node.derivation.premises });
      if (!addEdge({ source: node.id, target: id, role: "JOINT_PREMISES", ordinal: 0, viewOnly: true })) break;
      view = { ...node, derivation: { ...node.derivation, premises: [] } };
    }
    for (const edge of recordedEdges(view)) if (!addEdge(edge)) break;
    if (limited) break;
  }
  const ids = [root];
  const seen = new Set(ids);
  const depths = new Map([[root, 0]]);
  for (let i = 0; i < ids.length; ++i) {
    const id = ids[i];
    if (!expanded.has(id)) continue;
    for (const next of [...(neighbors.get(id) ?? [])].sort((a, b) =>
      Number(groups.has(b)) - Number(groups.has(a)) || (a < b ? -1 : a > b ? 1 : 0))) {
      if (seen.has(next)) continue;
      if (ids.length === maximumNodes) { limited = true; continue; }
      seen.add(next);
      ids.push(next);
      depths.set(next, depths.get(id) + 1);
    }
  }
  return {
    ids, depths, limited, groups,
    edges: edges.filter(edge => seen.has(edge.source) && seen.has(edge.target)),
  };
}
