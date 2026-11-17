# Archive-backed native reasoning

`run_knowledge_goal` uses the actual unified knowledge interface to retrieve a
stored native formal goal and iteratively resolve its explicit dependency
closure. It validates the existing native request, copies complete immutable
objects into the private writable overlay, executes or independently replays
the original C++ core, and stores its typed CoK derivations.

The input is an existing scoped native request, not raw English or a new
meaning schema. Independent modules are not joined by guessed predicate,
entity or quantity names. Proof-directed discovery of additional source
modules and their explicit semantic bindings remains a separate increment.
The implemented multi-query loop covers exact declared dependencies.

Missing dependencies and unavailable transport return an explicit incomplete
state. Retrieval/materialization budget failures remain resource-limited,
not false. Observed invalidation requires revalidation. Corrupt identities
and malformed formal requests remain errors. Stored snapshot assessments
are observations: the runtime never inserts them as authoritative journal
assessments. The local native checker makes its own request-bound records,
with empirical evidence still unassessed.

Writes are immutable and idempotent, not an all-or-nothing transaction across
an entire solve. Failure after valid imports or a checked certificate may
leave those useful partial records without claiming successful completion.
Provider-specific network deadlines remain the drive backend's responsibility;
this component does not claim an OS-enforced time/RSS bound.

## Actual offline workflow

```sh
make build/gammathink
./build/gammathink solve CONTEXT.json REQUEST.dsl journal.db
./build/gammathink snapshot journal.db NEW_DIRECTORY archive-name revision
./build/gammathink resume NATIVE_GOAL_ID fresh-journal.db NEW_DIRECTORY ROOT_HASH
```

The solve output includes each ordered slot's `request` identity. The snapshot
command returns the storage root hash and always reports `published:false`.
The `resume` command resolves that exact slot request through the local
overlay and supplied immutable snapshot. Neither command publishes data,
uses a neural model or treats source identity as empirical truth.

Verification uses an actual compressed GKS snapshot, archive registry and
virtual-drive mount, a fresh journal, private source provenance, independent
native certificates, restart/offline reuse, absent dependencies, invalidation,
contradictions and bounded retrieval. The focused C++ suite passes 28 checks.
The end-to-end CLI test passes 25 expectations across solve, restart, private
snapshot compilation, fresh-journal replay and missing-object handling.
The full 26-suite architecture target exits 0.
