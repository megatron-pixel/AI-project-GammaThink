# LFM Q4 fresh end-to-end accuracy

The trained LFM Q4 did not complete any of twelve fresh end-to-end cases.
Only two knowledge translations preserved the complete source meaning.
No generated Task IR passed admission, so no final response was delivered.

## Actual model and path

Used the saved ordinary-continuation LFM2.5-1.2B-Instruct checkpoint at
768 cumulative updates, exported as Q4_K_M:

- Retained parameters: 1,170,340,608.
- Model file: 730,899,040 bytes.
- SHA256: `0b7721451ee44b0aa0d60c4c43be65f749024d702a095e8cde1a9d0eb51ee721`.
- CPU-only native model manager, two threads, 8,192-token context.
- Maximum 2,048 generated tokens per translation; 6,144 reserved input tokens
  for Task IR. No output repairs, retries, grammar adjustments or training.

Each case started with fresh free-flow synthetic English in a real ZIM entry.
LFM translated the complete source using the production Knowledge DSL v2
prompt. The native compiler admitted and privately persisted executable
proposals, unloaded the model and compiled an immutable GKS snapshot.
For each admitted source, the real conversation store received the English
question. LFM then used the production Task IR prompt to describe the requested
source query and explanation. Native TaskSession admission guarded the path
to source reasoning, certificate checking and response delivery.

The intended path was therefore:

```text
English source -> LFM Knowledge DSL -> native admission -> private GKS
English question + actual module -> LFM Task IR -> native admission
-> unload -> C++ source reasoning -> certificate check -> stored response
```

All twelve independent reference DSL/Task IR controls reached correct checked
native outcomes and response delivery before model captures began. The LFM
outputs were then captured once and sealed before scoring. Expected DSL and
answers never entered model messages. The exact existing NVIDIA Nemotron
development worker invoked only the fixed local capture tool; it did not
translate the cases or replace LFM.

## Measured results

| Measure | Result |
|---|---:|
| Fresh cases | 12 |
| Reference native pipeline passed | 12/12 |
| LFM Knowledge DSL admitted | 10/12 |
| Complete faithful knowledge translation | 2/12 (16.7%) |
| LFM Task IR generations actually attempted | 9 |
| LFM Task IR admitted | 0/9 attempted |
| Final responses delivered | 0/12 |
| Complete faithful end-to-end successes | 0/12 |
| Actual LFM generations | 21: 12 source + 9 task |
| Maximum whole-process lifetime RSS | 1,216,266,240 bytes |

Two invalid source translations prevented their task stages. Another admitted
source produced a complete Task IR prompt larger than the reserved input
budget, so that case stopped before task generation. These are failures in
the twelve-case end-to-end denominator, not omitted observations.

The nine attempted Task IR outputs used invalid selectors, wrong
current-question citations, invented `summarize` actions or ill-typed
explanation inputs. None encoded an admitted source-reasoning request.
The model path therefore stopped before native reasoning; it did not return
nine wrong English answers or produce any checked final answer.

## Source meaning review

All twelve source outputs and all nine Task IR outputs were independently read
after sealing. Native source admission alone was not counted as fidelity.

| Case | Source translation finding |
|---|---|
| Positive fact | Faithful |
| Explicit negative fact | Faithful |
| Two-step implication | Both universal rules omitted |
| Joint prerequisites | Invalid variable binding; conjunctive rule omitted |
| Binary implication | Consequent relation and implication omitted |
| Conflicting assertions | Negative assertion dropped |
| Open-world query | Implication omitted |
| Implication direction | Glowing fact changed into humming fact |
| Unsupported cycle | Wrong implication added; another omitted |
| Separate entities | Second entity, negative fact and rule omitted |
| Free-flow paraphrase | Fact preserved, universal rule omitted |
| Missing prerequisite | Unterminated rule; intended implication omitted |

The source scorer also passed 36 reference/negative checks: all twelve
admitted references matched, and changed polarity and omitted assertions were
detected for each case. Main-agent semantic review agreed with the two
faithful scores. This review is not human certification.

## Separate diagnostic, not end-to-end success

After the captures sealed, the predeclared formal queries were independently
applied to the ten admitted model-produced source modules, without repairing
their knowledge or Task IR.

Five modules omitted the requested predicate or entity. Of the other five,
three formal outcomes matched expectations: the two faithful single facts and
a coincidental UNKNOWN from an unfaithful cycle translation. Two were wrong:
the dropped negative assertion hid a contradiction, and the changed
glowing-to-humming fact produced an incorrect positive proof.

These results show why a valid native certificate cannot certify English
meaning: it correctly reasons over the supplied theory, including a theory
that the interpreter translated incorrectly. None of these diagnostic runs
is credited as an unassisted end-to-end success.

## Evidence, limits and bookkeeping

Local evidence:
`../../.omo/evidence/lfm-end-to-end-20261118/`.
`FREEZE.json` binds the cases, private references, harness, actual model and
native dependencies before capture. `captures/SEAL.json` binds the raw
outputs; `RESULT.json` and `INDEPENDENT_REVIEW.json` record final scoring.
The native strict build and C++ diagnostics passed, all reference controls
passed, the actual capture worker exited 0, and scoring/diagnostics exited 0.
A successful test-process exit is not model accuracy.

The isolated harness recorded detailed generation metrics only after admission
succeeded. Consequently its first structural summary incorrectly counted
zero attempted task generations. The corrected result counts the nine
already sealed `task.raw` files; the original summary is retained, and no
inference was repeated. Detailed task timing/token/stop metrics are missing
for those nine rejection paths, as are source metrics for two source
rejections. Whole-process lifetime peaks were retained for all twelve.
Missing stage measurements were not reconstructed or claimed as verified.

The complete capture seal was verified before diagnostic reads. Reopening
the ten admitted SQLite libraries then reinitialized their volatile
`library.db-shm` sidecars. Final accounting records those ten exact hash
changes separately; all main databases, WAL files, raw outputs, prompts,
receipts and GKS blocks still match the original seal.

This is a small, agent-authored finite-Horn evaluation, not a random sample,
broad English accuracy estimate, whole-library test, production interpreter
selection or actual 8 GB-host qualification. The earlier 55/64 templated DEV
result measured a different task distribution and admission/fidelity target,
not this complete source-plus-request execution path.

No production code, model weights or reasoning-core parameters were changed.
The model ran with OS network denial; all knowledge stayed private and local.
