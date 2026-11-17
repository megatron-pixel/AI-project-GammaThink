# Canonical CoK object format, version 1

This is knowledge metadata and a container for existing payload contracts,
not a replacement English-to-DSL or parsed-meaning schema. Formal payloads
retain the existing admitted representation. Decoding metadata does not
admit a formal request or establish source fidelity.

Object identity is SHA-256 of canonical GKOB bytes. Its external form is
exactly 64 lowercase hexadecimal digits. It is not an IPFS CID. Encoding is
fixed-width little-endian; all collections preserve their supplied order,
including repeated source assertions and jointly necessary premises.
Text is length-prefixed UTF-8 with no NUL. Arbitrary payload bytes are opaque.

The record contains, in order:

- Four bytes `GKOB`, uint16 schema version 1, uint8 object kind, uint8 visibility.
- Label and payload-contract strings: uint32 byte length followed by bytes.
- Payload: uint32 byte length followed by existing-contract bytes.
- Concepts: uint32 count followed by strings.
- Sources: uint32 count; each has archive identity, archive version, entry
  path and extractor-version strings; 32 source-digest bytes; uint64 offset
  and length. Extents must not overflow, but their interpretation is checked
  against the actual source separately.
- Links: uint32 count; each has a uint8 relationship and 32 object-ID bytes.
- A uint8 derivation marker, exactly 0 or 1. Derivation objects require 1,
  all other object kinds require 0.
- When present: 32-byte conclusion and rule IDs, followed by ordered
  premise, assumption and evidence ID lists, each uint32-counted.

Object-kind tags 1..12 are Claim, Fact, Rule, Assumption, Hypothesis,
Derivation, Evidence, Goal, Action, Experiment, TestResult, Contradiction.
Relationship tags 1..8 are Requires, Implies, Supports, Contradicts,
DependsOn, Produces, Tests, AlternativeTo. Visibility 1 is Private, 2 Public.
Public visibility does not authorize publishing a contribution.

Maximum encoded size is 4 MiB including metadata. Each text field is at most
65536 bytes and each collection at most 4096 entries. Readers reject
unknown tags/versions, trailing bytes, malformed Unicode and oversized or
truncated fields. Requests are never shortened to fit.

All referenced objects are retrieval references. Required dependencies are
Requires/DependsOn links plus a derivation's rule, premises, assumptions
and evidence. Its conclusion is a reference, not a premise of its own proof.
Supports and AlternativeTo links must not be flattened into jointly
necessary premises.

Knowledge state and versioned logical/evidence assessments live separately
from immutable object content. A valid formal certificate does not establish
empirical truth; missing archives do not establish falsehood.
