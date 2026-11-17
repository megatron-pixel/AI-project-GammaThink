# GammaThink evidence viewer design system

## 0. Research Log

- Embedded shortlist: Linear, Notion and Sentry. Selected the minimalist
  operational style with Linear's luminance hierarchy and compact controls.
  The installed brand/style files were absent. Read the complete pinned
  [minimalist reference](https://github.com/Leonxlnx/taste-skill/blob/b17742737e796305d829b3ad39eda3add0d79060/skills/minimalist-skill/SKILL.md)
  and [Linear reference](https://github.com/nexu-io/open-design/blob/6afe7eae156bfa29251a51fd0636649c257f7444/design-systems/linear-app/DESIGN.md)
  from their upstreams. The initial taste reference excludes product UI, so
  it is not the applied style. This is inspiration, not a Linear clone.
- Lazyweb: two searches, `knowledge graph explorer inspector` and `developer
  tool detail panel`; actually viewed three public screens: Sentry event
  details, Postman request/details and Diffbot knowledge-graph marketing.
  Adopt leading context, central evidence, adjacent detail and explicit raw
  data inspection from the two product screens. Do not adopt marketing
  sections, logos or imagery. No signed URLs or tokens are retained here.
- StyleGallery: read its catalog, supporting-pane, panel-layout and
  scroll-body-shell contracts. Use the
  [panel layout](https://github.com/changeroa/StyleGallery/blob/main/patterns/viewport-shell/panel-layout.md)
  for graph/detail adjacency, composed inside a
  [bounded shell](https://github.com/changeroa/StyleGallery/blob/main/patterns/viewport-shell/scroll-body-shell.md).
  The graph owns pan/zoom; the desktop inspector owns vertical scrolling.
  On narrow screens the body owns vertical scrolling.
- Interaction: read the actual `StatefulButton` source from
  [beui's registry](https://beui.dev/r/button). Adopt immediate busy state,
  disabled duplicate submits, named outcome and reduced-motion opacity
  feedback, not its React code, dependencies or letter cascades. Diagram
  pan/zoom is a recorded custom interaction, not an invented catalog match.
- Imagen concepts skipped: no Imagen tool exists in the available catalog.
  This operational interface contains no generated-image surface.
- UI/UX database sanity lookup unavailable: the installed skill's
  `ui-ux-db/scripts/search.py` is absent. No package/tooling repair is in scope.
- Read frontend design/perfection, layout, interaction, designpowers routing,
  orchestration and lane C, plus browser and visual QA instructions.
  Use one independent final gate reviewer after the main agent's actual QA,
  following the higher-level single-reviewer requirement.

## 1. Atmosphere & Identity

A quiet evidence workbench, not an answer dashboard. A spacious diagram sits
beside a precise source ledger. The signature is a derivation node that keeps
joint premises visibly grouped while logical validity and empirical evidence
remain separate. The default theme follows the system. GammaThink appears
only in the header; the page title is "Evidence graph".

The audience includes a keyboard-only investigator tracing premises, a
low-vision reader inspecting provenance at enlarged text sizes, and a phone
user checking a contradiction. Each must complete the same tasks without
requiring a drag gesture or interpreting color alone.

## 2. Color

| Role/token | Light | Dark | Use |
| --- | --- | --- | --- |
| `--canvas` | `#f7f6f3` | `#08090a` | Diagram ground |
| `--panel` | `#ffffff` | `#0f1011` | Header and inspector |
| `--surface` | `#f9f9f8` | `#191a1b` | Nodes and fields |
| `--wash` | `#eae9e5` | `#28282c` | Hover/selected wash |
| `--ink` | `#2f3437` | `#f7f8f8` | Primary text |
| `--muted` | `#5d6265` | `#b0b5be` | Secondary text |
| `--line` | `#85888c` | `#797d84` | Control and graph edges; measured >=3:1 |
| `--divider` | `#eaeaea` | `#2d3035` | Panel separation |
| `--primary` | `#111111` | `#f7f8f8` | Inspect button |
| `--on-primary` | `#ffffff` | `#111111` | Primary label |
| `--focus` | `#5456b8` | `#aaa9ff` | Keyboard focus only |
| `--conflict-ink` | `#9f2f2d` | `#ffb4b1` | Conflict label/diagram edge |
| `--conflict-fill` | `#fdebec` | `#382323` | Conflict tag |
| `--valid-ink` | `#346538` | `#b6d7b4` | Logical-valid label |
| `--valid-fill` | `#edf3ec` | `#233326` | Logical-valid tag |
| `--unknown-ink` | `#765513` | `#ead49a` | Unavailable/unassessed label |
| `--unknown-fill` | `#fbf3db` | `#373020` | Unknown tag |

Color never means truth by itself. Contradictions have a textual label and a
dashed graph edge. Selected controls use a wash plus a check glyph, not an
accent border. Status colors do not become decorative card stripes.

## 3. Typography

Use native `-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif` and
`ui-monospace, "SF Mono", Menlo, monospace`. No downloaded fonts, images or
third-party requests. These are original native-font choices, not a claim
to ship Linear's proprietary typography.

| Token | Desktop | Narrow | Weight / line-height |
| --- | --- | --- | --- |
| `--title-size` | 20px | 20px | 600 / 1.3 |
| `--heading-size` | 18px | 20px | 600 / 1.4 |
| `--body-size` | 14px | 17px | 400 / 1.6 |
| `--meta-size` | 12px | 15px | 400 / 1.5 |

Paragraph measure is at most 65ch. Hashes and paths wrap anywhere deliberately;
prose uses normal word boundaries. IDs are selectable. Abbreviated node IDs
have a full accessible name and an exact full value in the inspector.

## 4. Spacing & Layout

Base unit 4px: `--s1:4px`, `--s2:8px`, `--s3:12px`, `--s4:16px`,
`--s5:20px`, `--s6:24px`, `--s8:32px`, `--s12:48px`.
Controls have a minimum 44px hit area (`--target`).
Graph nodes are `--node-width:232px`, `--node-height:104px`;
layout steps are `--column-step:320px`, `--row-step:152px`.
The row step grows to the measured node height plus `--s8` when enlarged text
needs more room; cached positions retain their row and column.
Inspector width is `--inspector-width:384px`.
The primitive showcase is bounded by `--content-width:1200px`.
`--context-offset` is measured from the pinned inspector header, the inspector's
top padding and a `--s4` focus gap;
it follows text wrapping and enlarged text rather than assuming a fixed height.
The diagram uses `overflow:clip`, not a hidden native scroll
container; only the explicit pan transform moves its coordinates.

At 800px and above, the shell is `100dvh`, with a fixed header and toolbar,
then `minmax(0,1fr)` content. The graph has no CSS scrollbars. Only the
inspector scrolls vertically. Below 800px, the body scrolls as one column,
with a 40dvh diagram (minimum 280px) above the inspector; no nested vertical
scroll owner. Header controls wrap in source order. Safe-area padding is
added at the outside edges. Every grid/flex child has zero intrinsic minima.

The graph may pan in two dimensions as its explicit scientific workspace;
ordinary content, forms, raw-data panes and provenance never force horizontal
page scrolling. Body/inspector headings retain useful context at scrolled end.
The inspector heading pins the selected object's kind and abbreviated identity
while its raw payload or provenance is scrolled. The complete selectable
identity remains in the object details. This is distinct from the graph root.

## 5. Components

Build and visually verify `/primitives.html` before the product screen.

| Primitive | Structure / variants | Required states and access |
| --- | --- | --- |
| Action | Native button; primary, neutral, icon with name | Default, hover, pressed, focus, disabled, busy and failed; immediate wash/label feedback; 44px target |
| Field | Visible label, input/select, adjacent error | Empty, populated, invalid, disabled; no placeholder-only labels |
| Status | Text tag; neutral, logical, conflict, unknown | No color-only meanings; logical and empirical status stay separate |
| Knowledge node | Native button with kind, label, short ID | Normal, selected/check, unavailable and conflict; Enter/Space activates; no fabricated claim labels |
| Disclosure | Native details/summary and structured list | Collapsed/expanded; visible control alternative to diagram expansion |
| Data panel | Heading, definition list, selectable pre | Empty/loading/error/data; wrapped paths, byte-exact paginated raw view |
| Diagram viewport | SVG edges plus positioned node buttons | Pointer pan/wheel zoom; keyboard pan, zoom buttons and Fit; viewport culling |

Shared primitives use the same classes in the showcase and product.
The product must expose literal indexed search, exact-ID inspection, incoming
relationship pagination, provenance, raw archived payload, derivation groups,
alternative proofs, contradictions and observed verification metadata.
All values come from the actual native knowledge API. No screenshot or mock
stands in for live data.

Large joint-premise arrays use the already verified node primitive as a
presentation-only group: "View group", "Joint premises (N)" and "Not an
object". It is not an added knowledge object, certificate or inferred
relationship. Group IDs stay in a separate view namespace and never enter the
native API or object cache. Activating the group opens the actual owner's
ordered premise list, retaining every occurrence and duplicate. This replaces
an unreadable fan of overlapping edges without merging alternative proofs.

## 6. Motion & Interaction

`--micro:120ms`, `--standard:200ms`, `--ease:cubic-bezier(.2,.8,.2,1)`.
Only opacity and transform may transition. Press changes the control itself
immediately. Async requests disable only duplicate actions and expose
`aria-busy` plus a specific loading label; failure stays next to its action.
Success is the changed graph/detail state, not a toast.
The read-status row reserves its cancel-button height. Secondary inspector
navigation stays invisible during the initial read so its later placement does
not jump past the reader while the object details arrive.

Pan tracks the pointer 1:1 using pointer capture, without inertia or a delayed
tween. Wheel zoom is anchored to the pointer. Keyboard arrows pan only when
the diagram itself has focus, so text fields retain normal editing.
Zoom controls and Fit provide alternatives to gestures. Expanding knowledge
requires an explicit read action. Layout position is deterministic and cached
only inside the bounded current view; the browser never runs the reasoner.

Under reduced motion, press scale becomes an opacity change; continuous
gesture tracking remains direct. No looping decoration, blur animation,
entrance cascade, automatic scroll or focus movement.

Exception for explicit reference navigation: when activation replaces the
focused reference list, focus moves to the new object's visible heading.
A view-group activation focuses its ordered-list disclosure below the pinned
context. Keyboard focus on a partly visible diagram node pans that node into
view through the same coordinate transform. Ordinary graph selection keeps
focus on its original node button; initial loading never moves focus.

## 7. Depth & Surface

Use mixed restrained structure: panel luminance steps plus one-pixel neutral
dividers, and a barely perceptible node shadow `0 2px 8px rgb(0 0 0 / .04)`.
No floating marketing cards, glass effects or colored state borders.
Radii: `--radius-sm:4px`, `--radius-md:8px`; small status tags may use
`--radius-full:9999px`, never large containers.

Diagram edges have `--edge-width:1.5px`; the grid is a fine point lattice
using `--divider`, a coordinate cue rather than a hero background.

## 8. Accessibility Constraints & Accepted Debt

Target WCAG 2.2 AA: 4.5:1 text, 3:1 large text/control edges, visible keyboard
focus, reading-order focus, named landmarks, native controls and full
non-gesture task paths. Support enlarged text, both themes, reduced motion,
long labels, binary payloads, empty data, unavailable objects and failed reads.
Graph grouping is supplemented by an ordered textual premise/assumption/
evidence list, including duplicate occurrences.

The viewer is loopback-only, read-only and optional during reasoning.
It never executes artifacts, evaluates prompt text, uploads private data or
turns imported assessments into authoritative truth. Raw data uses text nodes,
not HTML. Provider origin and observation time are explicit.

Accepted debt: none. No visual, accessibility, performance or whole-application
memory compliance is claimed before actual measurements and browser QA.
