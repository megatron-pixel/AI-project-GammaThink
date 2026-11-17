# Validated task execution

`task_ir.hpp` connects the existing shared archive DSL to the original
untrained C++ core, checked native certificates, planner and SQLite journal.
It implements the existing bounded ordered theory/goals profile, not every
proposed action in revision 2.9 and not broad English interpretation.

The host owns an immutable `ProblemSourceCatalog`. `admit_task_dsl` requires
the input snapshot, decodes the existing DSL without repairs, resolves only
schema-declared source handles and admits every ordered goal before return.
Clarification and unsupported proposals are explicit variants. Knowledge-only
modules without goals remain source-compiler inputs, not executable tasks.
No solver, model or journal write occurs during admission.

`AdmittedTask` has no public unchecked constructor or mutable request access.
Its payload is the existing `ProblemGoalsRequest`; the adapter does not
introduce a competing meaning schema. The ordered-goal implementation remains
shared with `training/native-goals-v1/`, including its AXGL serialization and
fixed aggregate work/step partition. The existing AXRM protocols are unchanged.

`persist_task` records host context, the exact interpreted DSL, the ordered
request and each native slot before execution. All records are private.
Repeated goals remain repeated ordered occurrences, even when their identical
native requests share an object identity and checked certificate. The planner
retains their separate slots and joint task completion dependency.

`execute_task` uses `run_native`, independently replays each request-bound
certificate and materializes its typed CoK derivations, evidence and
contradictions. Reuse rechecks current dependencies and invalidation.
Materialization budgets are charged across the complete task, including
repeated occurrences. Immutable writes are idempotent, but the complete
multi-call task is not one SQLite transaction: an interrupted run may leave
valid partial records, never a fabricated completion.

Unresolved or unavailable outcomes do not satisfy the task. Conflicts remain
explicit. A completed query and formal validity do not establish empirical
truth, source-meaning fidelity or that every signed query was answered true.
The runtime does not promote imported assessments or derived CoK objects to
empirically supported knowledge.

## Offline command

```sh
make build/gammathink
./build/gammathink solve examples/task-context.json examples/task-request.dsl /tmp/my-task-journal.db
```

The example is a manually supplied synthetic translation. The command requires
an existing host context and DSL; it does not pretend a production English
interpreter has passed evaluation. It loads no model, calls no network and
trains nothing. Output is deterministic checked-result rendering rather than
a neural reconstruction of the proof.

The observed first invocation executed one native request for two repeated
goals. A second process using the same journal independently revalidated and
reused both certificates with zero new native executions. The first process
measured 10,878,976 bytes maximum RSS and 0.39 seconds external wall time.
This tiny formal workload is not whole-application or interpreter qualification.

The focused suite passes 44 deterministic checks, covering admission before
execution, repeated ordered goals, signed contradictions, unresolved proof
search, missing/invalidated dependencies, private durable proofs, restart,
stale snapshots, malformed DSL, clarification, host-owned unit conversion,
scope, precision and materialization budgets. The full 25-suite architecture
target passes. Neural lifecycle/session orchestration and iterative acquisition
of additional knowledge remain separate increments.
