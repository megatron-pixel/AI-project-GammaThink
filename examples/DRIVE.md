# Structured knowledge drive

`VirtualDrive` combines the live writable journal with registered GKS
mounts. It uses the existing `UnifiedKnowledge` contract, not another meaning
schema or reasoning engine. Raw Kiwix text still enters through the separate
source adapter and translation-only compiler.

```cpp
VirtualDrive drive(journal, registry);
auto view = drive.open_view();
KnowledgeQuery query;
query.kind = QueryKind::Text;
query.value = "thermal equilibrium";
KnowledgeStream results(view->knowledge(), query);
while (auto item = results.next()) {
    // Preserve unavailable identities; do not infer a negative fact.
}
```

The journal must outlive the view. The view must outlive streams using it.
Opening a view reads the active mount list in one catalogue statement and
admits every root. Failure throws instead of returning a partial mount set.
The journal is provider zero, followed by mounts in registry alias order.
It takes precedence for duplicate identities and observed local assessments.
At most 15 snapshot providers fit alongside it; excess mounts fail admission.

Mount roots and their generations are pinned in a view. New views see current
mount/unmount/hot-swap operations; old views and streams retain their original
archives. The journal remains live, so this is not cross-store transaction
isolation. Unmounting does not delete snapshots or modify the truth/assessment
state of dependent journal objects. `resolve` on a new view lists absent
dependency identities explicitly. A still-mounted archive's missing block is
an `Unavailable` error, not a silently omitted provider.

## Indexed queries

`GksArchive` implements all existing query families: identity enumeration,
kind, exact concept, source with optional version, literal text phrase,
relationship and required-by. It checks index profiles, child ranges and
levels. Querying reads index nodes, not object payloads; `get` separately
validates object identity and its chunk ordinal.

Text uses the same SQLite unicode61 normalization and literal phrase
semantics as the journal. The first term selects a bounded index range;
subsequent terms are point-probed at consecutive token positions in the
same object. Phrase matches do not require loading document bodies.
Queries do not treat text as a Boolean query language.

Exact-key results stream in identity order. Source queries spanning versions
and text postings may have several key ranges per object; they scan the
matching bounded range and retain only the smallest `limit + 1` identities.
This preserves global identity ordering and honest completion/cursors without
materializing the full matching set. Very broad ranges can exhaust a budget;
they never return a truncated page marked complete.

Per-query limits cover index-node reads, examined node entries, stored bytes,
work and phrase tokens. The stored-byte limit is checked against file size
before allocating a node's stored buffer. It is not a process-RSS guarantee;
each node also has the GKS decompression/record/key limits. Default phrase
length is 64 tokens. There is no implicit retry or inference. Explicitly
authorized remote sources may fetch blocks on demand; `REMOTE_MOUNTS.md`
describes the capability and lifetime rules.

## Verification boundary

`make architecture-test` runs `virtual-drive-test`. It compares all indexed
query families and paginated cursors against a real journal, including exact
case, empty concept/version, repeated words, quoted phrases, accent/case
normalization and structured derivation dependencies. It also exercises
mount hot-swap, local-overlay precedence, unmount availability, pinned
streams, missing index blocks and quota failures.

Observed assessments consistently accept the journal's 16,385 dependency
limit; traversal edge/object/byte budgets remain independent. The drive does
not make imported assessment rows authoritative. `GATEWAY.md` describes the
opt-in remote block source usable by `GksArchive`. Whole-application
interpreter/core wiring, real-library scaling,
physical-memory qualification and measured frontier comparisons remain open.
`CACHE.md` and `PINS.md` describe verified block caching and explicit local
retention; decoded-object caching and full dependency prefetch remain separate.
`REMOTE_QUERY.md` describes the snapshot-verified query provider, also usable
as a persisted remote binding with explicit runtime authorization.
