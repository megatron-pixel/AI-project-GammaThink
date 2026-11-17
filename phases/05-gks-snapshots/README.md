# PHASE 5 — GKS SNAPSHOTS

Components: [`snapshots`](../../components/snapshots/).

## Original section 47 requirements

- Canonical serialisation
- Chunked storage
- Compression
- Index generation
- Archive mounting
- Cross-archive references
- Snapshot versioning

## Evidence and acceptance boundary

Checks: gks, gks-snapshot, archive-registry, retrieval, knowledge-stream. See [verification](../../verification/README.md).

Snapshot and restart controls are local bounded fixtures, not all possible archives.
