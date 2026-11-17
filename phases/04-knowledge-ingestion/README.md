# PHASE 4 — KNOWLEDGE INGESTION

Components: [`knowledge-ingestion`](../../components/knowledge-ingestion/), [`knowledge-dsl`](../../components/knowledge-dsl/), [`interpreter-interfaces`](../../components/interpreter-interfaces/).

## Original section 47 requirements

- Kiwix ZIM integration
- Source extraction
- Knowledge compiler
- Candidate DSL generation
- Provenance tracking
- Review pipeline

## Evidence and acceptance boundary

Checks: source-archive, source-compiler, source-knowledge, source-ingestion, source-bindings, bound-sources. See [verification](../../verification/README.md).

Candidate DSL semantic quality is interpreter-dependent and explicitly deferred. Parser, provenance, compiler and review mechanisms were checked.
