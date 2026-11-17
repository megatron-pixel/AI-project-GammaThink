# Persistent conversation storage

Revision 2.9 sections 06-10 introduce conversation memory distinct from the
knowledge journal. `context::ConversationMemory` uses its own SQLite database
and application identity. Neither database can be opened as the other.
It does not create claims, proofs or authoritative knowledge assessments.

`open_session` binds an immutable session ID to a project. `append` stores
the original UTF-8 message, role, caller-supplied timestamp label and SHA-256
content digest. IDs are database-assigned, globally increasing insertion
identifiers. Timestamps do not determine precedence and are not validated
as calendar instants. The API has no message update or deletion operation;
database triggers also reject direct message updates and deletes.

Messages persist with atomic FTS5 index insertion. `recent` retrieves a
bounded latest-message window in chronological insertion order. Its
`complete` field says whether older messages were omitted; it has no forward
cursor. `search` retrieves literal unicode61 token phrases, scoped to an exact
session, in ascending message-ID order. Double quotes are escaped before
passing the phrase to FTS5. Operator words such as `OR` are ordinary tokens.
It is not substring search, semantic search or a relevance-ranked RAG system.
Keyset cursors do not promise snapshot isolation across separate page calls.

`save_summary` appends a derived summary with a source-message range and
expected prior version. Both endpoint messages must belong to the session.
The range is inclusive within that session, not every intervening global ID.
Old summaries remain addressable by version. No model is called and summary
faithfulness is not automatically certified.

`revise_constraint` appends an explicitly selected pinned constraint revision
with its user-source message ID. The source must belong to the same session.
The expected previous revision is checked in the writer transaction; a source
older than the current source is rejected. An inactive revision withdraws a
pin without deleting history. `constraints` returns only the latest active
revision of each key, and fails rather than omit active pins when its budget
cannot include them. Older revisions remain inspectable through `constraint`.
This preserves corrections independently of summaries. It does not infer that
an arbitrary paraphrase faithfully represents its cited message.

All writes use immediate transactions and report contention without retrying.
Connections are single-owner-thread objects; separate connections support
independent readers and serialized writers. Foreign keys, WAL and full
synchronous commits are enabled. The SQLite VM instruction budget bounds
individual API operations, not filesystem latency or whole-process runtime.

Input text is bounded at 64 KiB, IDs/keys/timestamp labels at 256 bytes and
search phrases at 4096 bytes. Pages allow at most 4096 messages and 4 MiB of
content bytes. Byte overflow rejects the request without returning clipped
messages. SQLite cache defaults to 4096 KiB. Result accounting excludes
allocator overhead, IDs, timestamps and SQLite internal buffers; these limits
are not a demonstrated 3 GB application memory bound. Persistent history grows
on disk; disk-quota/retention management remains future work.

This increment implements the persistence and scoped phrase-retrieval
boundary. `context.hpp` now adds snapshot-consistent retrieval and
source-attributed token assembly; see `CONTEXT.md`. Cross-session project
retrieval, semantic ranking, summary generation, active-task state and model
load/unload integration remain pending. Original messages are private local
context, not public knowledge contributions or instructions from retrieved
documents.

Focused check:

```sh
make build/conversation-test
./build/conversation-test
```

SQLite references:
[FTS5 phrase syntax](https://sqlite.org/fts5.html#fts5_strings) and
[progress handlers](https://sqlite.org/c3ref/progress_handler.html).
