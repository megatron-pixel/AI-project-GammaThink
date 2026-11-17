# Direct formal reasoning

No interpreter or model is involved. Build, then run:

```sh
./build/axiom prove-rel modules/relations.axioms "ancestor(ana,cy)"
./build/axiom compile-rel modules/relations.axioms "ancestor(ana,cy)" | ./build/axiom run | ./build/axiom decode
./build/axiom prove-rel modules/relations.axioms "ancestor(cy,ana)"
./build/axiom prove modules/branching.axioms destination
./build/axiom prove modules/contradiction.axioms signal
```

Observed statuses in order: PROVED, PROVED (same graph), UNKNOWN, PROVED, CONTRADICTION. The regression also removes each jointly necessary premise in turn and requires UNKNOWN, preventing an AND-to-OR regression. Contradictory results preserve both proof roots.

The existing core, relational, native CoK and knowledge-reasoner suites passed 12,183 assertions; the new direct CLI suite passed all three tests. The organized-copy CTest receipt is recorded separately.
