# GammaThink revision 2.9 component organization

The authoritative design remains `AI Agent Architecture and topology.md`. This tree groups existing implementations by ownership and maps all 73 section 47 requirements into eleven `phases/` folders. Components spanning phases are stored once, not copied into competing implementations. The original `ai-design` implementation is retained. Dedicated `component-*` development branches share this tested baseline; the project README links to each component on its branch.

Start with [BUILDING.md](BUILDING.md), [direct core usage](verification/DIRECT_REASONING.md), and [verification boundaries](verification/README.md). The clean `reasoning-core` is independent of interpreter execution; the main composition deliberately shares non-core include paths. Stable production serialization contracts previously located under training directories are promoted into `contracts/`, not replaced. Training scripts, datasets, model weights, private histories and sealed evaluations are excluded.

## Development phase groups

- [PHASE 1 — STRUCTURED FOUNDATIONS](phases/01-structured-foundations/README.md)
- [PHASE 2 — THREE-LAYER LOCAL MEMORY](phases/02-three-layer-local-memory/README.md)
- [PHASE 3 — SYMBOLIC REASONING](phases/03-symbolic-reasoning/README.md)
- [PHASE 4 — KNOWLEDGE INGESTION](phases/04-knowledge-ingestion/README.md)
- [PHASE 5 — GKS SNAPSHOTS](phases/05-gks-snapshots/README.md)
- [PHASE 6 — NEURAL MODEL MANAGER](phases/06-neural-model-manager/README.md)
- [PHASE 7 — IPFS VIRTUAL DRIVE](phases/07-ipfs-virtual-drive/README.md)
- [PHASE 8 — AGENT EXECUTION](phases/08-agent-execution/README.md)
- [PHASE 9 — HYPERGRAPH VISUALISATION](phases/09-hypergraph-visualisation/README.md)
- [PHASE 10 — DISTRIBUTED KNOWLEDGE](phases/10-distributed-knowledge/README.md)
- [PHASE 11 — PERFORMANCE AND SCALE](phases/11-performance-and-scale/README.md)

`source-manifest.json` records original paths, SHA-256 digests and the eight include-path-only adaptations. The staged-only formal CLI and CMake configuration are new organization glue. Existing project README, license and design are preserved.
