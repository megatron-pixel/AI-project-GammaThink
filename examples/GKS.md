# GKS storage format, version 1

This implements compressed, independently addressed blocks, local journal
snapshot compilation and read-only identity lookup. The existing GKOB
knowledge object schema and formal payloads are unchanged. Kiwix ZIM files
remain source archives; the writable SQLite journal remains separate.

## Envelope

All integers are unsigned little-endian. Stored block identity is SHA256 over
the complete envelope and compressed bytes. `BlockId` is a distinct C++ type
from `ObjectId`; neither is an IPFS CID.

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `GKSB` |
| 4 | 2 | GKS format version, 1 |
| 6 | 1 | Kind: objects 1, index 2, manifest 3 |
| 7 | 1 | Codec: 1, Zstandard |
| 8 | 4 | Decompressed body length |
| 12 | 4 | Stored frame length |
| 16 | variable | Exactly one known-size, dictionary-free Zstandard frame |

Decoders verify the expected block hash, both extents, frame count and
declared content size before allocating the decompression output. Bodies are
at most 8 MiB; stored frame bytes at most 8 MiB + 64 KiB. Caller limits may
reduce format limits. Concatenated/skippable frames, unknown sizes, trailing
data, unknown envelope versions and unknown codecs are rejected.

The writer uses compression level 3. Block hashes bind stored bytes, so a
different compressor version or settings may change a block identity without
changing any knowledge object identity. These are integrity checks, not
authentication, logical verification or empirical truth.

## Bodies

Variable byte strings start with a 32-bit byte length. Text is UTF-8 without
NUL and at most 65,536 bytes. Collections start with a 32-bit count and
contain at most 4,096 records. Object and block hashes are 32 bytes.

**Object chunks** contain a positive record count. Each record stores the
object hash, 32-bit GKOB length, canonical GKOB bytes, and a one-byte optional
assessment flag. Object hashes must increase strictly; GKOB length is capped
by the existing 4 MiB object limit. Decoding checks each object hash.

An assessment stores a 64-bit positive version; one-byte knowledge state,
logical validity and evidence assessment; checker and reason text; a one-byte
optional certificate flag and object hash when present; then a counted list
of up to 16,385 checked dependency object hashes, matching the existing
journal assessment contract rather than the chunk-record limit. These are
preserved observations.
Decoding never imports them as authoritative local checker records or
changes an object's identity.

**Index chunks** store a nonempty key-format name of at most 128 bytes,
32-bit tree level and positive entry/child count. A position is a length-led
binary key followed by an object hash. Keys are at most 196,640 bytes;
positions compare lexicographically by key and then object hash.

At level zero, entries contain a position, object-chunk hash and 32-bit
zero-based object ordinal below 4,096. Entries increase strictly. At positive
levels, children contain inclusive first/last positions and a child-block
hash. Ranges cannot overlap; tree levels are at most 16. This permits small
index nodes and object chunks to be fetched separately. Fetching children,
validating cross-block range/level promises and interpreting a declared key
format belong to the snapshot reader, not this block decoder.

**Manifest blocks** store name and revision text of at most 128 bytes, 16-bit
existing object schema version, 64-bit declared object count, index roots and
parent manifest hashes. A root contains one-byte index kind, key-format name,
32-bit level and index-block hash. Index kinds are object identities 1,
object kind 2, concept 3, source 4, text 5, relationship 6 and prerequisite 7.
Roots increase strictly by kind. Nonempty snapshots require an object index;
empty snapshots contain no roots. Up to 16 parent hashes increase strictly.
Declared counts and cross-block references are not certified by decoding
one manifest. The compiler/reader must validate its index profile and closure.

## Verification

`compile_snapshot` captures objects and their latest assessment observations
inside `Journal::read_snapshot`. Its SQLite read view is pinned before export;
another WAL connection can write without changing that view. The export does
not include the complete assessment history and is not a journal backup.
Source, derivation, scope and ordered formal payloads are not rewritten.

The destination must be a new directory. It is private (0700); blocks are
exclusive-created, hashed, fsynced and finalized owner-read-only (0400) as
`<64 lowercase hex digits>.gksb`. The root identity is returned only after
all chunks and the manifest are written and the directory is fsynced.
Compilation never overwrites an existing destination or publishes data.
Failure may leave unreferenced blocks or a partial temporary workspace; no
root is returned and no registry entry is installed. Cleanup is explicit.

The compiler uses a disk-backed SQLite sorting workspace with a configured
cache target, page quota and VM instruction meter. It builds object chunks, then
index leaves and branches incrementally. Alternating frontier tables avoid
mutating a table being scanned. A 256 KiB chunk size is a soft target; one
large object and at least two branch children may exceed it, up to the 8 MiB
body limit. Object count, input bytes, stored output bytes and attempted
postings have separate limits. Duplicate posting attempts count against the
work budget. Workspace and stored-output quotas are separate; none measures
process RSS or establishes the 3 GB application target.

Seven index profiles are compiled when populated: `object-id-v1` (empty key,
object identity position), `object-kind-v1` (one-byte kind),
`concept-utf8-v1` (exact UTF-8), `source-nul-version-v1` (archive, NUL, version),
`unicode61-position-v1` (normalized term, NUL, 32-bit little-endian token
offset), `target-relationship-v1` (target hash and one-byte relationship) and
`prerequisite-id-v1` (target hash). Both journal and snapshot use the same
indexed text construction and SQLite unicode61 tokenizer. Ordered derivation
fields feed relationship/prerequisite indexes without changing object bytes.

`GksSnapshot(directory, expected_root)` loads a supplied root identity and
`get(object_id)` reads only the object-index path and containing object
chunk. It checks child levels, ranges, key profile and object ordinals.
Missing required blocks are `Unavailable`; corrupt bytes are errors. An
identity absent from the index is an availability miss, not logical falsity.
Local mount and unified indexed-query interfaces are documented in
`REGISTRY.md` and `DRIVE.md`. IPFS, signatures and publication remain separate
tasks.

`make architecture-test` includes `build/gks-test` and
`build/gks-snapshot-test`. Synthetic checks cover
object/assessment preservation, leaf and branch round trips, identity
separation, every stored-byte truncation, hash-matching malformed envelopes,
incorrect embedded hashes, concatenated frames, invalid ordering and
caller-reduced byte/count/key/depth budgets. This does not measure archive
scaling, RSS, real-library interpretation or end-to-end remote retrieval.
