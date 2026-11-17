import { boundedGraph } from "./graph.mjs";

const byId = id => document.getElementById(id);
const idPattern = /^[0-9a-f]{64}$/;
const state = {
  nodes: new Map(), unavailable: new Set(), expanded: new Set(), positions: new Map(),
  root: "", selected: "", token: "", sources: [], bytes: 0, busy: false, abort: null,
  x: 32, y: 32, scale: 1, graph: null, revision: 0, rendered: "", index: null,
  explorationLimited: false, cacheLimited: false, rowStep: 0,
};
const board = byId("board");
const nodesLayer = byId("nodes");
const edgeLayer = byId("edges");
const nodeElements = new Map();
let drawFrame = 0;
const human = value => value.toLowerCase().replaceAll("_", " ").replace(/^./, c => c.toUpperCase());
const short = id => id.slice(0, 10);
function element(tag, text, className) {
  const value = document.createElement(tag);
  if (text !== undefined) value.textContent = text;
  if (className) value.className = className;
  return value;
}
function message(text, error = false) {
  byId("read-error").hidden = !error;
  if (error) {
    byId("read-error").textContent = text;
    byId("read-status").textContent = "The read did not complete.";
  }
  else byId("read-status").textContent = text;
}
function updateBusy() {
  document.querySelectorAll("[data-read]").forEach(button => { button.disabled = state.busy; });
  byId("inspect").setAttribute("aria-busy", String(state.busy));
  byId("cancel").hidden = !state.busy;
}
async function read(path, parameters, consume) {
  if (state.busy) return;
  state.busy = true;
  state.abort = new AbortController();
  message("Reading the configured local sources...");
  updateBusy();
  try {
    const response = await fetch(path + "?" + new URLSearchParams(parameters), {
      headers: { "x-gammathink-view": state.token }, signal: state.abort.signal, cache: "no-store",
    });
    const value = await response.json();
    if (!response.ok) throw new Error(value.error);
    await consume(value);
    message("Observed locally at " + new Date().toLocaleTimeString() + ". No new verification was performed.");
  } catch (error) {
    message(error.name === "AbortError" ? "Read cancelled. The previous view remains available." : error.message, true);
  } finally {
    state.busy = false;
    state.abort = null;
    updateBusy();
  }
}
function queueDraw() {
  if (drawFrame) return;
  drawFrame = requestAnimationFrame(() => { drawFrame = 0; draw(); });
}
function rebuildGraph() {
  state.graph = boundedGraph(state.nodes, state.root, state.expanded);
  const displayed = new Set(state.graph.ids);
  for (const id of state.unavailable) if (!displayed.has(id) && id !== state.selected) state.unavailable.delete(id);
  for (const id of state.expanded) if (!state.nodes.has(id)) state.expanded.delete(id);
  const occupied = new Map();
  for (const [id, point] of state.positions) {
    if (!state.nodes.has(id) && !state.graph.groups.has(id) && !state.graph.ids.includes(id)) { state.positions.delete(id); continue; }
    occupied.set(point.column, Math.max(occupied.get(point.column) ?? 0, point.row + 1));
  }
  const css = getComputedStyle(document.documentElement);
  const columnStep = parseFloat(css.getPropertyValue("--column-step"));
  const rowStep = state.rowStep = Math.max(state.rowStep, parseFloat(css.getPropertyValue("--row-step")));
  for (const id of state.graph.ids) {
    if (state.positions.has(id)) continue;
    const column = state.graph.depths.get(id);
    const row = occupied.get(column) ?? 0;
    occupied.set(column, row + 1);
    state.positions.set(id, { x: column * columnStep, y: row * rowStep, column, row });
  }
  ++state.revision;
  state.rendered = "";
  queueDraw();
}
function select(id) {
  const fromDetail = byId("detail").contains(document.activeElement);
  state.selected = id;
  renderDetail();
  if (fromDetail) {
    const heading = byId("detail").querySelector("h3");
    if (heading) {
      heading.tabIndex = -1;
      heading.scrollIntoView({ block: "start", behavior: "instant" });
      heading.focus({ preventScroll: true });
    }
  }
  message("Selected " + (state.nodes.get(id)?.label || short(id)) + " in the existing view.");
  state.rendered = "";
  queueDraw();
}
function objectButton(id, label, asRoot = false) {
  const button = element("button", label, "object-row");
  button.type = "button";
  button.dataset.read = "";
  button.dataset.objectId = id;
  button.disabled = state.busy;
  button.addEventListener("click", () => {
    if (asRoot) loadNeighborhood(id, true);
    else if (state.nodes.has(id)) select(id);
    else loadNeighborhood(id, false);
  });
  return button;
}
function draw() {
  if (!state.graph) return;
  const width = board.clientWidth, height = board.clientHeight;
  const transform = `translate(${state.x}px,${state.y}px) scale(${state.scale})`;
  nodesLayer.style.transform = transform;
  edgeLayer.setAttribute("transform", `translate(${state.x} ${state.y}) scale(${state.scale})`);
  byId("zoom-value").textContent = Math.round(state.scale * 100) + "%";
  const visible = state.graph.ids.filter(id => {
    const p = state.positions.get(id);
    const x = p.x * state.scale + state.x, y = p.y * state.scale + state.y;
    return x < width + 64 && y < height + 64 && x + 296 * state.scale > -64 && y + 168 * state.scale > -64;
  });
  const signature = state.revision + ":" + state.selected + ":" + visible.join();
  board.dataset.nodeCount = String(visible.length);
  const gap = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--s8"));
  if (signature === state.rendered
    && [...nodeElements.values()].every(button => button.offsetHeight + gap <= state.rowStep)) return;
  state.rendered = signature;
  const visibleSet = new Set(visible);
  for (const [id, button] of nodeElements) {
    if (!visibleSet.has(id)) { button.remove(); nodeElements.delete(id); }
  }
  for (const id of visible) {
    const node = state.nodes.get(id);
    const group = state.graph.groups.get(id);
    let button = nodeElements.get(id);
    if (!button) {
      button = element("button", undefined, "node");
      button.type = "button";
      button.dataset.read = "";
      button.dataset.objectId = id;
      button.append(element("span", "", "node-kind"), " ",
        element("span", "", "node-label"), " ",
        element("code", short(id), "meta"));
      button.addEventListener("click", () => {
        const group = state.graph.groups.get(id);
        if (group) {
          select(group.owner);
          const ordered = byId("detail").querySelector('[data-reference-group="PREMISE"]');
          ordered.open = true;
          const summary = ordered.querySelector("summary");
          summary.scrollIntoView({ block: "start", behavior: "instant" });
          summary.focus({ preventScroll: true });
        } else if (state.nodes.has(id)) select(id);
        else loadNeighborhood(id, false);
      });
      nodeElements.set(id, button);
      nodesLayer.append(button);
    }
    button.disabled = state.busy;
    button.dataset.kind = group ? "VIEW_GROUP" : node?.kind ?? "UNLOADED";
    button.dataset.availability = group ? "VIEW_ONLY" : node ? "CACHED" : state.unavailable.has(id) ? "UNAVAILABLE" : "NOT_LOADED";
    button.querySelector(".node-kind").textContent = group ? "View group" : node ? human(node.kind) : state.unavailable.has(id) ? "Unavailable" : "Not loaded";
    button.querySelector(".node-label").textContent = group ? `Joint premises (${group.members.length})` : node?.label || (node ? human(node.kind) : "Read this object to inspect it");
    button.querySelector("code").textContent = group ? "Not an object" : short(id);
    button.setAttribute("aria-label", [...button.children].map(child => child.textContent).join(" ")
      + (group ? `; open ordered premise occurrences for ${group.owner}` : "; full object ID " + id));
    button.setAttribute("aria-pressed", String(id === state.selected));
    button.classList.toggle("selected", id === state.selected);
    const p = state.positions.get(id);
    button.style.transform = `translate(${p.x}px,${p.y}px)`;
  }
  const rowStep = Math.max(state.rowStep, ...[...nodeElements.values()].map(button => button.offsetHeight + gap));
  if (rowStep > state.rowStep) {
    state.rowStep = rowStep;
    for (const point of state.positions.values()) point.y = point.row * rowStep;
    ++state.revision;
    queueDraw();
    return;
  }
  edgeLayer.replaceChildren();
  for (const edge of state.graph.edges) {
    if (!visibleSet.has(edge.source) && !visibleSet.has(edge.target)) continue;
    const a = state.positions.get(edge.source), b = state.positions.get(edge.target);
    const forward = b.x >= a.x;
    const ax = a.x + (forward ? 232 : 0), bx = b.x + (forward ? 0 : 232);
    const ay = a.y + 52, by = b.y + 52;
    const bend = Math.max(48, Math.abs(bx - ax) / 2);
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", edge.source === edge.target
      ? `M ${ax} ${ay} C ${ax + 80} ${ay - 96}, ${ax + 80} ${ay + 96}, ${ax} ${ay}`
      : `M ${ax} ${ay} C ${ax + (forward ? bend : -bend)} ${ay}, ${bx + (forward ? -bend : bend)} ${by}, ${bx} ${by}`);
    path.setAttribute("marker-end", "url(#arrow)");
    path.dataset.role = edge.role;
    if (edge.role === "CONTRADICTS") path.classList.add("contradicts");
    const title = document.createElementNS("http://www.w3.org/2000/svg", "title");
    title.textContent = human(edge.role) + (edge.joint ? " occurrence " + (edge.ordinal + 1) + "; jointly required" : "");
    path.append(title);
    edgeLayer.append(path);
  }
  byId("diagram-count").textContent = `${visible.length} nodes in view; ${state.graph.ids.length} in this branch; ${state.nodes.size} objects cached.`
    + (state.graph.limited ? " Diagram limit reached; use the full ordered lists and indexed navigation." : "")
    + (state.explorationLimited ? " Some reads reached exploration bounds; use focused reads or incoming pages." : "")
    + (state.cacheLimited ? " Older objects were evicted from the bounded cache." : "");
}
function field(list, label, value) {
  list.append(element("dt", label), element("dd", value));
}
function disclosure(title, open = false) {
  const details = element("details");
  details.open = open;
  details.append(element("summary", title));
  return details;
}
function showReferences(parent, title, ids, role) {
  const details = disclosure(title + " (" + ids.length + ")");
  if (role) details.dataset.referenceGroup = role;
  const list = element("ol");
  let position = 0;
  const previous = element("button", "Previous occurrences");
  const more = element("button", "Show next occurrences");
  previous.type = more.type = "button";
  const render = () => {
    list.replaceChildren();
    list.start = position + 1;
    const end = Math.min(ids.length, position + 16);
    for (let i = position; i < end; ++i) {
      const id = ids[i];
      const item = element("li");
      item.append(objectButton(id, (state.nodes.get(id)?.label || short(id)) + " - occurrence " + (i + 1)));
      list.append(item);
    }
    previous.disabled = position === 0;
    more.disabled = end === ids.length;
    previous.hidden = more.hidden = ids.length <= 16;
  };
  previous.addEventListener("click", () => { position -= 16; render(); });
  more.addEventListener("click", () => { position += 16; render(); });
  render();
  const controls = element("div", undefined, "cluster");
  controls.append(previous, more);
  details.append(list, controls);
  parent.append(details);
}
function renderRaw(parent, node) {
  const details = disclosure("Raw archived payload");
  const bytes = Uint8Array.from(node.payload_hex.match(/../g) ?? [], value => parseInt(value, 16));
  let text = null;
  try {
    const decoded = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    if (!/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(decoded)) text = decoded;
  } catch { /* Binary payloads remain available byte-for-byte in hex. */ }
  const label = element("label", "Raw data representation");
  label.htmlFor = "raw-mode";
  const mode = element("select");
  mode.id = "raw-mode";
  for (const [value, title] of [["text", "UTF-8 text"], ["hex", "Hexadecimal bytes"]]) {
    const option = element("option", title);
    option.value = value;
    option.disabled = value === "text" && text === null;
    mode.append(option);
  }
  mode.value = text === null ? "hex" : "text";
  const pre = element("pre");
  pre.id = "raw-data";
  const range = element("p", "", "meta");
  const controls = element("div", undefined, "cluster");
  const previous = element("button", "Previous chunk"), next = element("button", "Next chunk");
  previous.type = next.type = "button";
  controls.append(previous, next);
  let position = 0, end = 0;
  const render = () => {
    const length = mode.value === "text" ? text.length : bytes.length;
    if (mode.value === "text") {
      if (position && /[\udc00-\udfff]/.test(text[position])) --position;
      end = Math.min(length, position + 4096);
      if (end < length && /[\ud800-\udbff]/.test(text[end - 1])) --end;
      pre.textContent = text.slice(position, end);
      range.textContent = `Text offsets ${position} to ${end}; ${bytes.length} original payload bytes.`;
    } else {
      end = Math.min(length, position + 4096);
      pre.textContent = [...bytes.slice(position, end)].map(value => value.toString(16).padStart(2, "0")).join(" ");
      range.textContent = `Bytes ${position} to ${end} of ${length}.`;
    }
    previous.disabled = position === 0;
    next.disabled = end === length;
  };
  previous.addEventListener("click", () => { position = Math.max(0, position - 4096); render(); });
  next.addEventListener("click", () => { position = end; render(); });
  mode.addEventListener("change", () => { position = 0; render(); });
  render();
  details.append(label, mode, range, pre, controls);
  parent.append(details);
}
function renderDetail() {
  const parent = byId("detail");
  parent.replaceChildren();
  delete parent.dataset.kind;
  const node = state.nodes.get(state.selected);
  const availability = node ? "CACHED" : state.unavailable.has(state.selected) ? "UNAVAILABLE" : "NOT_LOADED";
  parent.dataset.availability = availability;
  byId("selection-context").textContent = (node ? human(node.kind) : human(availability)) + " / " + short(state.selected);
  byId("selection-context").title = state.selected;
  if (!node) {
    parent.append(element("p", state.unavailable.has(state.selected)
      ? "This object is unavailable in the configured sources. This is not a false claim."
      : "This object has not been loaded into the current view."));
    const load = element("button", "Read neighborhood");
    load.dataset.read = "";
    load.addEventListener("click", () => loadNeighborhood(state.selected, false));
    parent.append(load, element("code", state.selected, "break"));
    return;
  }
  parent.dataset.kind = node.kind;
  parent.append(element("h3", node.label || human(node.kind)), element("code", node.id, "break"));
  const controls = element("div", undefined, "cluster");
  const load = element("button", "Read neighborhood");
  load.dataset.read = "";
  load.addEventListener("click", () => loadNeighborhood(node.id, false));
  const focus = element("button", "Focus graph here");
  focus.addEventListener("click", () => {
    state.root = node.id;
    state.positions.clear();
    state.expanded.add(node.id);
    state.x = state.y = 32; state.scale = 1;
    byId("object-id").value = node.id;
    rebuildGraph();
  });
  const expand = element("button", state.expanded.has(node.id) ? "Collapse branch" : "Expand branch");
  expand.addEventListener("click", () => {
    if (state.expanded.has(node.id)) state.expanded.delete(node.id); else state.expanded.add(node.id);
    rebuildGraph(); renderDetail(); updateBusy();
  });
  controls.append(load, focus, expand);
  parent.append(controls);
  const identity = element("dl");
  field(identity, "Object kind", human(node.kind));
  field(identity, "Source", state.sources[node.archive]);
  field(identity, "Source visibility", human(node.visibility));
  field(identity, "Payload contract", node.format);
  parent.append(identity);
  const assessment = node.assessment;
  const statuses = element("div", undefined, "cluster");
  for (const [label, value, variant] of [
    ["Logical", assessment?.logical ?? "UNCHECKED", assessment?.logical === "VALID" ? "valid" : "unknown"],
    ["Evidence", assessment?.evidence ?? "UNASSESSED", ["CONFLICTING", "CHALLENGED"].includes(assessment?.evidence) ? "conflict" : "unknown"],
    ["State", assessment?.state ?? "UNASSESSED", assessment?.state === "DISPUTED" ? "conflict" : ""],
  ]) {
    const tag = element("span", label + ": " + human(value), "tag " + variant);
    tag.dataset.assessment = value;
    statuses.append(tag);
  }
  parent.append(statuses, element("p", "Observed metadata is not a fresh verification. Logical validity does not establish empirical truth.", "meta"));
  if (assessment) {
    const list = element("dl");
    field(list, "Assessment revision", assessment.version);
    field(list, "Checker", assessment.checker);
    field(list, "Recorded reason", assessment.reason);
    parent.append(list);
    showReferences(parent, "Checked dependencies", assessment.checked_dependencies);
    if (assessment.certificate) showReferences(parent, "Certificate reference", [assessment.certificate]);
  }
  if (node.derivation) {
    parent.append(element("p", "All premises in this derivation are jointly necessary. Alternative proofs remain separate derivation objects."));
    showReferences(parent, "Conclusion", [node.derivation.conclusion]);
    showReferences(parent, "Rule", [node.derivation.rule]);
    showReferences(parent, "Joint premises", node.derivation.premises, "PREMISE");
    showReferences(parent, "Assumptions", node.derivation.assumptions);
    showReferences(parent, "Evidence", node.derivation.evidence);
  }
  const links = disclosure("Recorded links (" + node.links.length + ")");
  const list = element("div", undefined, "stack");
  let linkCount = 0;
  const more = element("button", "Show next links");
  more.type = "button";
  const appendLinks = () => {
    const end = Math.min(node.links.length, linkCount + 16);
    for (; linkCount < end; ++linkCount) {
      const link = node.links[linkCount];
      list.append(objectButton(link.target, human(link.relationship) + ": " + (state.nodes.get(link.target)?.label || short(link.target))));
    }
    more.hidden = linkCount === node.links.length;
  };
  more.addEventListener("click", appendLinks);
  appendLinks();
  links.append(list, more);
  parent.append(links);
  const sources = disclosure("Source provenance (" + node.sources.length + ")");
  const spans = element("div", undefined, "stack");
  const previousSource = element("button", "Previous source spans"), nextSource = element("button", "Next source spans");
  previousSource.type = nextSource.type = "button";
  let sourcePosition = 0;
  const renderSources = () => {
    spans.replaceChildren();
    for (const source of node.sources.slice(sourcePosition, sourcePosition + 16)) {
      const record = element("dl");
      for (const [label, value] of [["Archive", source.archive], ["Archive revision", source.version],
        ["Entry", source.entry], ["Extractor", source.extractor], ["Source digest", source.digest],
        ["Byte offset", source.offset], ["Byte length", source.length]]) field(record, label, value);
      spans.append(record);
    }
    previousSource.disabled = sourcePosition === 0;
    nextSource.disabled = sourcePosition + 16 >= node.sources.length;
    previousSource.hidden = nextSource.hidden = node.sources.length <= 16;
  };
  previousSource.addEventListener("click", () => { sourcePosition -= 16; renderSources(); });
  nextSource.addEventListener("click", () => { sourcePosition += 16; renderSources(); });
  renderSources();
  const sourceControls = element("div", undefined, "cluster");
  sourceControls.append(previousSource, nextSource);
  sources.append(spans, sourceControls);
  if (!node.sources.length) sources.append(element("p", "No direct source span is recorded on this object. Inspect its dependencies."));
  parent.append(sources);
  renderRaw(parent, node);
}
function renderGroups() {
  const parent = byId("loaded-groups");
  parent.replaceChildren();
  const groups = new Map();
  for (const node of state.nodes.values()) {
    if (!groups.has(node.kind)) groups.set(node.kind, []);
    groups.get(node.kind).push(node);
  }
  for (const [kind, nodes] of [...groups].sort()) {
    const details = disclosure(human(kind) + " (" + nodes.length + ")");
    details.append(element("p", "Display grouping only, not a logical premise group.", "meta"));
    for (const node of nodes) details.append(objectButton(node.id, node.label || short(node.id)));
    parent.append(details);
  }
}
async function loadNeighborhood(id, reset) {
  if (!idPattern.test(id)) { message("Enter the complete 64-character lowercase object ID.", true); return; }
  await read("/api/inspect", { id }, async output => {
    if (output.format !== "gammathink.inspection.v1" || output.nodes.length > 256) throw new Error("Unknown inspection response");
    if (reset) {
      state.nodes.clear(); state.positions.clear(); state.unavailable.clear(); state.expanded.clear();
      state.bytes = 0; state.root = id; state.x = state.y = 32; state.scale = 1; state.rowStep = 0;
      state.explorationLimited = false; state.cacheLimited = false;
      nodesLayer.replaceChildren(); nodeElements.clear();
    }
    state.selected = id;
    state.explorationLimited ||= output.exploration_limited;
    for (const node of output.nodes) {
      const encoded = Uint8Array.from(node.canonical_hex.match(/../g), value => parseInt(value, 16));
      const digest = [...new Uint8Array(await crypto.subtle.digest("SHA-256", encoded))]
        .map(value => value.toString(16).padStart(2, "0")).join("");
      if (digest !== node.id) throw new Error("An object failed its content-identity check");
      const old = state.nodes.get(node.id);
      if (old) state.bytes -= new TextEncoder().encode(JSON.stringify(old)).length;
      state.nodes.delete(node.id);
      state.nodes.set(node.id, node);
      state.bytes += new TextEncoder().encode(JSON.stringify(node)).length;
    }
    for (const missing of output.unavailable) state.unavailable.add(missing);
    for (const [candidate, node] of state.nodes) {
      if (state.nodes.size <= 128 && state.bytes <= 16 * 1024 * 1024) break;
      if (candidate === state.root || candidate === state.selected) continue;
      state.bytes -= new TextEncoder().encode(JSON.stringify(node)).length;
      state.nodes.delete(candidate);
      state.positions.delete(candidate);
      state.cacheLimited = true;
    }
    state.expanded.add(id);
    byId("object-id").value = state.root;
    rebuildGraph(); renderDetail(); renderGroups();
    byId("board").dataset.explorationLimited = String(output.exploration_limited);
  });
}
async function readIndex(path, parameters, title) {
  await read(path, parameters, output => {
    if (output.format !== "gammathink.inspection-index.v1") throw new Error("Unknown index response");
    state.index = { path, parameters, title, next: output.next };
    byId("index-panel").hidden = false;
    byId("index-scope").textContent = title;
    const results = byId("index-results");
    results.replaceChildren();
    for (const record of output.records) {
      const button = objectButton(record.id, (record.label || short(record.id)) + (record.kind ? " - " + human(record.kind) : " - unavailable"), true);
      results.append(button);
    }
    if (!output.records.length) results.append(element("p", "No indexed objects matched this page."));
    byId("next-index").hidden = output.complete;
  });
}
function zoom(factor, x = board.clientWidth / 2, y = board.clientHeight / 2) {
  const next = Math.min(2.5, Math.max(.25, state.scale * factor));
  const ratio = next / state.scale;
  state.x = x - (x - state.x) * ratio;
  state.y = y - (y - state.y) * ratio;
  state.scale = next;
  queueDraw();
}
function fit() {
  if (!state.graph?.ids.length) return;
  const points = state.graph.ids.map(id => state.positions.get(id));
  const left = Math.min(...points.map(p => p.x)), top = Math.min(...points.map(p => p.y));
  const width = Math.max(...points.map(p => p.x)) - left + 232;
  const height = Math.max(...points.map(p => p.y)) - top + 168;
  state.scale = Math.min(1.25, Math.max(.25, Math.min((board.clientWidth - 64) / width, (board.clientHeight - 64) / height)));
  state.x = (board.clientWidth - width * state.scale) / 2 - left * state.scale;
  state.y = (board.clientHeight - height * state.scale) / 2 - top * state.scale;
  queueDraw();
}
byId("object-form").addEventListener("submit", event => { event.preventDefault(); loadNeighborhood(byId("object-id").value.trim(), true); });
byId("search-form").addEventListener("submit", event => {
  event.preventDefault();
  readIndex("/api/search", { q: byId("search").value, after: "-" }, "Literal phrase: " + byId("search").value);
});
byId("find-incoming").addEventListener("click", () => readIndex("/api/related",
  { id: state.selected, relationship: byId("relationship").value, after: "-" },
  human(byId("relationship").value) + " / " + state.selected));
byId("browse-kind").addEventListener("click", () => readIndex("/api/objects",
  { kind: byId("object-kind").value, after: "-" }, "Object kind: " + byId("object-kind").value));
byId("next-index").addEventListener("click", () => readIndex(state.index.path,
  { ...state.index.parameters, after: state.index.next }, state.index.title));
byId("cancel").addEventListener("click", () => state.abort?.abort());
byId("zoom-in").addEventListener("click", () => zoom(1.25));
byId("zoom-out").addEventListener("click", () => zoom(1 / 1.25));
byId("fit").addEventListener("click", fit);
byId("theme").addEventListener("change", event => {
  if (event.target.value === "system") delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = event.target.value;
});
let dragging = null;
board.addEventListener("pointerdown", event => {
  if (event.button !== 0 || event.target.closest("button")) return;
  dragging = { id: event.pointerId, x: event.clientX, y: event.clientY };
  board.setPointerCapture(event.pointerId);
  board.classList.add("dragging");
});
board.addEventListener("pointermove", event => {
  if (!dragging || event.pointerId !== dragging.id) return;
  state.x += event.clientX - dragging.x; state.y += event.clientY - dragging.y;
  dragging.x = event.clientX; dragging.y = event.clientY; queueDraw();
});
const finishDrag = () => { dragging = null; board.classList.remove("dragging"); };
board.addEventListener("pointerup", finishDrag);
board.addEventListener("pointercancel", finishDrag);
board.addEventListener("focusin", event => {
  const button = event.target.closest(".node");
  if (!button || !button.matches(":focus-visible")) return;
  const point = state.positions.get(button.dataset.objectId);
  const margin = parseFloat(getComputedStyle(document.documentElement).getPropertyValue("--s4"));
  const x = point.x * state.scale + state.x, y = point.y * state.scale + state.y;
  const width = button.offsetWidth * state.scale, height = button.offsetHeight * state.scale;
  if (x < margin) state.x += margin - x;
  else if (x + width > board.clientWidth - margin) state.x -= x + width - board.clientWidth + margin;
  if (y < margin) state.y += margin - y;
  else if (y + height > board.clientHeight - margin) state.y -= y + height - board.clientHeight + margin;
  queueDraw();
});
board.addEventListener("wheel", event => {
  event.preventDefault();
  const rect = board.getBoundingClientRect();
  zoom(Math.exp(-event.deltaY * .001), event.clientX - rect.x, event.clientY - rect.y);
}, { passive: false });
board.addEventListener("keydown", event => {
  if (event.target !== board || event.metaKey || event.ctrlKey || event.altKey) return;
  const directions = { ArrowLeft: [48, 0], ArrowRight: [-48, 0], ArrowUp: [0, 48], ArrowDown: [0, -48] };
  if (directions[event.key]) {
    event.preventDefault(); state.x += directions[event.key][0]; state.y += directions[event.key][1]; queueDraw();
  } else if (["+", "=", "-", "0"].includes(event.key)) {
    event.preventDefault();
    if (event.key === "0") fit(); else zoom(event.key === "-" ? 1 / 1.25 : 1.25);
  }
});
new ResizeObserver(queueDraw).observe(board);
const inspector = byId("inspector");
const contextHeader = inspector.querySelector(":scope > header");
new ResizeObserver(() => {
  const style = getComputedStyle(inspector);
  const gap = parseFloat(style.paddingBlockStart) + parseFloat(style.getPropertyValue("--s4"));
  inspector.style.setProperty("--context-offset", contextHeader.getBoundingClientRect().height + gap + "px");
}).observe(contextHeader);
try {
  const response = await fetch("/session", { cache: "no-store" });
  const session = await response.json();
  if (!response.ok) throw new Error(session.error);
  state.token = session.token; state.sources = session.sources;
  await loadNeighborhood(session.root, true);
} catch (error) { message(error.message, true); }
finally { delete document.body.dataset.initializing; }
