# PHASE 2 — THREE-LAYER LOCAL MEMORY

Components: [`memory`](../../components/memory/).

## Original section 47 requirements

- Conversation persistence
- Recent-message retrieval
- Session summaries
- Pinned constraints
- SQLite FTS5 indexing
- Historical context retrieval
- Context assembly
- Source attribution

## Evidence and acceptance boundary

Checks: conversation, context, summary-maintenance, session-tasks, document-retrieval. See [verification](../../verification/README.md).

Persistent mechanism tests use synthetic conversations, not private chat exports.
