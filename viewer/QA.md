# Evidence viewer acceptance evidence

This is a bounded optional viewer increment for revision 2.9 sections 24-25,
not acceptance of the complete GammaThink application or a trained interpreter.
All paths below are relative to `ai-design/`. The enclosing workspace has no
Git history, so the reviewed source set is listed explicitly.

## Source set

Native additions: `include/inspection.hpp`, `src/inspection.cpp`,
`src/inspection_main.cpp`, `tests/inspection_test.cpp`,
`tests/viewer_fixture.cpp`; Makefile inspector/fixture/test targets.
Presentation additions: `viewer/{server,graph,app}.mjs`, `viewer/style.css`,
`viewer/{index,primitives}.html`, `viewer/DESIGN.md`.
Checks: `tests/viewer_{graph,server}.test.mjs`,
`tests/viewer_{primitives,product,interaction}_qa.mjs`,
`tests/viewer_lighthouse.mjs`, `tests/viewer_smoke.mjs`.
Run contract: `examples/INSPECTION.md`.

## Main-agent QA

The main agent drove the real Chrome surface and inspected the screenshots,
including every phone/desktop/wide-screen light/dark top, premise and scrolled-end
capture. Native proof objects came from the existing curated DSL example with
zero model calls. The large immutable GKS fixture is explicitly synthetic:
130 ordered joint-premise occurrences, repeated final occurrences, an independent
alternative, contradiction, unavailable dependency, hostile text and a source
offset above JavaScript's exact integer range. Its observed assessment is not
a core certificate. The browser API used a different journal as overlay, so
snapshot reads exercised the immutable provider without copying its objects.

| Scenario | Exact surface/check | Observed | Verdict | Evidence |
| --- | --- | --- | --- | --- |
| Native objects, canonical identity, ordered joint premises, alternatives, independent assessments, unavailable/budget markers and keyset indexes | `make inspection-test` | 113 checks | PASS | `tests/inspection_test.cpp`; integrated monitor `mon_7JX9KB63MJX5MKMS` exit 0 |
| Private loopback API and real native integration | `make viewer-test` | Four tests, no failures; Host/Origin/token/method/enumeration/cursor checks; no effect route | PASS | `tests/viewer_server.test.mjs`, `tests/viewer_graph.test.mjs`; monitor `mon_5VN5ZQX6D2H4ZBBQ` exit 0 |
| Existing architecture, session and CLI behavior | `make architecture-test viewer-test session-test runtime-cli-test` | 31 architecture suites; session integration; four viewer and two CLI tests passed | PASS | Monitor `mon_7JX9KB63MJX5MKMS` exit 0 |
| Responsive layouts and source/graph context | Product harness, 390 DPR 3, 1440 and 1920 CSS pixels, both themes | No page overflow or duplicate IDs; 44px controls, explicit diagram clipping and body/inspector scroll ownership | PASS | `build/viewer-product-qa-v9/report.json`; 18 paired top/premise/end PNGs |
| Joint occurrences, raw data and source identity | Real UI navigation and API-to-displayed-byte comparison | All 130 ordered occurrences match, including repeats; UTF-8 chunks and hex match original bytes; exact large offset; hostile text inert | PASS | Same report; `raw-source-desktop-dark.png` |
| Separate alternatives, contradictions and missing dependencies | Incoming indexes and actual object selection | Separate derivation object, dashed contradiction edges, unavailable is not false | PASS | Same report; `contradiction-desktop.png`, `missing-source-desktop.png` |
| Original archive DSL | Browse actual Action objects, inspect raw payload | Display equals original canonical object's payload, not a regenerated translation | PASS | Same report; `retained-dsl-desktop.png` |
| Keyboard access and 200% body/metadata text | Tab, arrows, zoom/Fit and enlarged inspector | Focus remains visible, no native diagram scroll drift, measured pinned header clearance, no enlarged-node overlap | PASS | Same report; `keyboard-focus-desktop.png`, `enlarged-text-desktop.png` |
| Pointer pan, wheel zoom, branch collapse/expand and explicit/system themes | Trusted Chrome input | Each changes actual state; collapse leaves one root | PASS | `build/viewer-interaction-qa-v2/report.json` |
| Loading, cancel and failed transport | Subscribe to exact CDP request pause before triggering; cancel; then real offline failure | Duplicates disabled while pending; previous object retained; controls recover; loading message clears | PASS | Same report; `loading-desktop.png`, `cancelled-desktop.png`, `transport-error-desktop.png` |
| Primitive interaction, contrast and reduced motion | Real Chrome state matrix | 27 initial captures; a found primary-hover contrast defect was fixed and rechecked | PASS | `build/viewer-primitives-qa-v3/report.json` and PNGs |
| Accessibility and performance audit | Lighthouse 13.6.0, real mobile and desktop presets against loopback API | 100 performance/accessibility/best-practices/SEO on both; accessible-name advisory passes after literal text separation | PASS | `build/viewer-lighthouse-v4/{mobile,desktop}.{json,html}` |
| Native snapshot inspection resource sample | `/usr/bin/time -l build/knowledge-inspect ... --snapshot ...` | 0.02 seconds external wall; 10,158,080 bytes peak RSS; exit 0 | PASS | Monitor `mon_DQ7QFA70MFB7HMTZ` |
| Static diagnostics | C++ language server on inspector/exporter/fixture | No diagnostics | PASS | Session tool results; JavaScript LSP unavailable, actual Bun/Chrome checks used |

The final product matrix passes 51 assertions. The supplemental interaction
matrix passes 10. The final node-label text separators and corrected read-error
status do not change the data contracts; the relevant accessibility audit and
read-state matrix were rerun after those edits.

The supplemental cancellation test pauses a real request at the browser's
transport boundary, without fabricating a response. It proves browser abort
and view retention; it is not a stress test of an already-running native
subprocess. Native process shutdown is separately bounded by the server's
AbortSignal and exit handling.

## Residual limits

The audit retains no-store back/forward-cache advisories deliberately: private
knowledge is not put in a browser history cache to improve a score. It also
reports the small render-blocking stylesheet and the real local request chain.
Mobile CLS was 0.046986, desktop 0.000917; zero blocking time in both runs.
These are observations on this host, not universal performance guarantees.

The supplemental renderer sample used about 1.54 MB JS heap, with a 4.72 MB
allocated heap. Those are JavaScript-engine counters, not whole Chrome RSS.
The 128-object/16-MiB record cache, 80-node/768-edge display, native query and
output limits do not by themselves establish whole-application 3 GB compliance
or large public-library throughput.

Snapshots remain local and immutable; no contribution is published. The viewer
does not extend the Task IR grammar, run coding artifacts, interpret English,
train the core or qualify any model. Public contribution workflows and broader
frontend integration remain separate tasks.

## Independent gate

The single independent reviewer returned **APPROVE**, with no blockers, in
`../.omo/evidence/bounded-gammathink-evidence-viewer-gate-review.md`.
It independently reproduced live journal/GKS identities and semantics, ran
the three pure graph tests and audited the real QA artifacts. Repeated-read
stress and native subprocess timing interleavings remain untested; approval
does not extend to whole-application RAM or model qualification.
