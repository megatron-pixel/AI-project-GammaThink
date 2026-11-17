# PHASE 6 — NEURAL MODEL MANAGER

Components: [`model-manager`](../../components/model-manager/), [`interpreter-interfaces`](../../components/interpreter-interfaces/).

## Original section 47 requirements

- Quantised model runtime
- On-demand loading
- Explicit unloading
- Memory profiling
- Interpreter integration
- Task IR evaluation
- Explanation generation

## Evidence and acceptance boundary

Checks: model-manager, model-prompt (existing runtime mechanisms only). See [verification](../../verification/README.md).

Selected-model semantic acceptance, Task IR quality and explanation fidelity are deferred. Existing loading/unloading mechanisms and previous measurements do not qualify the current candidate.
