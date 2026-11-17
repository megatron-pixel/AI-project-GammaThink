# Executable contract, version 1

This implementation is a bounded symbolic engine, not a pretrained wrapper.
There is no trained neural model or learned weight file. Production compiler,
inference, binary translation, and CLI are C++20. GMP supplies exact rationals.
Module replacement changes declared assumptions, never executable core semantics.

## Language and operations

The CLI accepts `eval EXPR` (`evaluate` is an alias), `solve LEFT=RIGHT`,
`det MATRIX`, `linear MATRIX RHS`, `born AMPLITUDES`, and `prove MODULE GOAL`.
Each expression or matrix is one shell argument. Scalar expressions contain
decimal integers, parentheses, unary `+`/`-`, and binary `+`, `-`, `*`, `/`.
Multiplication/division bind more tightly than addition/subtraction; binary
operators associate left. `x` is permitted only in affine equations.
No implicit multiplication, exponentiation, decimal fractions, radicals,
arbitrary English, quantifiers, nonlinear products, or variable denominators.
Represent rational fractions using `/`. Unsupported syntax is rejected.

Matrices are `[a,b;c,d]`, with scalar expressions as cells. All nonempty
rectangular dimensions through 16 are supported. Determinants require square
matrices. Linear solves accept an `m` by `n` matrix and `m` by 1 right side.
An infinite family returns a particular solution and one vector per free
parameter; an inconsistent system returns no solution. These are distinct.
The particular solution and nullspace basis are checked by exact substitution.
Determinants use exact pivoted elimination. These are arithmetic algorithms,
not symbolic derivations for arbitrary mathematics.

`born "[3/5,0;0,4/5]"` accepts finite rational-complex pure-state amplitudes
as `(real, imaginary)` rows. It requires exact norm one and returns
`[9/25,16/25]` in the computational basis. It does not normalize invalid input
silently. This operation assumes the finite-state Born rule; it does not
implement arbitrary observables, state evolution, density matrices, irrational
amplitudes, or infinite-dimensional quantum mechanics.

## Modules and proofs

Module source starts with `axioms 1 NAME VERSION`. Remaining nonblank lines:

```text
fact atom | source or declared assumption
rule premise & !other_premise -> conclusion | source or declared assumption
```

Whole-line `#` comments are allowed. Provenance is mandatory and has no execution
authority. `NAME` and atom names match `[A-Za-z_][A-Za-z0-9_.]*`; names are at most
128 bytes. Version is a nonempty token of at most 128 bytes. Modules are explicit
ground/propositional Horn clauses, not first-order rules with hidden variables.
The compiler interns names. Literal ID `2*n` is symbol `n`; `2*n+1` is its
explicit negation. The executor handles numeric literal IDs, not text semantics.
The supplied modules are declared demonstrations, not library-extracted facts.

CPU search is deterministic backward depth-first search in module rule order.
It branches, backtracks on failure, detects active-path cycles, and memoizes
only closed proofs. A failed goal in a cyclic branch cannot poison another
branch. Successfully derived facts from failed branches may remain because
their dependency certificates contain no speculative assumptions. Both goal
polarities are searched. Every retained derivation is replayed against its rule,
with exact premise order and strictly earlier dependency indices.

The search's full bounded event history is serialized, not a rolling hash.
Independent forward closure checks that a success status does not hide a
derivable opposite. This validation does not add search-produced proof steps.
`PROVED` means only the goal is derived; `DISPROVED` requires an explicit
opposite derivation; `CONTRADICTION` requires both; otherwise `UNKNOWN`.
Incomplete searches report `UNKNOWN` with `limited=true`, except that two
already checked opposite proofs suffice for `CONTRADICTION`.
Contradictions are goal-local, paraconsistent, and never imply every atom.
All proof claims are conditional on this exact module, which is embedded in
both request and result. Nothing authenticates a module author's assumptions.

`prove MODULE GOAL --gpu` uses actual CUDA subset tests to propose eligible
rules during monotone forward closure. It requires at most 64 signed literal
IDs (32 symbols, including any newly introduced goal), and a nonempty rule set.
CPU validation rejects wrong positive or negative eligibility answers; the
same independent proof checker remains mandatory. There is no CPU fallback
for a requested GPU backend. GPU mode is recorded in the request/result.
CPU and GPU may have different certificates and budget consumption while
agreeing on complete logical outcomes. This is not a GPU speedup claim.

`--library ROOT` on the human `prove` command performs retrieval only after an
`UNKNOWN` result. Citations, excerpts, coverage, and errors are printed in a
separate evidence report. The proof and its status are never changed.
No extracted prose is promoted to an axiom. Binary `run` never performs
retrieval, filesystem module loading, or shell execution.
For this retrieval bridge only, `.` and `_` in a literal become keyword spaces
and a leading `!` is removed; for example `quantum.mechanics` searches
`quantum mechanics`. This mapping does not change the formal goal or its status.

## Wire encoding

Every multibyte integer is unsigned little-endian. No C++ object layouts,
host-endian words, pointer values, or struct padding appear on the wire.

Header, exactly 12 bytes:

| Offset | Field |
| --- | --- |
| 0..3 | ASCII `AXRM` |
| 4..5 | u16 schema version `1` |
| 6 | u8 record type: `1` request, `2` result |
| 7 | u8 reserved, must be zero |
| 8..11 | u32 payload byte count |

The payload must consume the whole record: truncation and trailing bytes fail.
There is no checksum or cryptographic authentication; validation establishes
format/type/proof consistency, not that a record came from a trusted producer.
The human CLI encodes its request, calls `run_binary`, decodes the result, then
renders only the decoded fields. Binary commands emit no banners.

Primitive encodings:

- A string is u32 byte length followed by bytes. No NUL/control bytes.
  Metadata is opaque to inference; symbol names are ASCII identifiers.
- An array is u32 element count followed by its elements.
- A rational is u8 sign (`0` nonnegative, `1` negative), numerator magnitude,
  denominator magnitude. Each magnitude is u32 byte length followed by
  little-endian bytes. Zero numerator has zero bytes; nonzero magnitudes have
  no high zero byte. Denominator is positive; fraction is reduced; negative
  zero is rejected. There is no decimal text inside numeric fields.
- A matrix is u32 rows, u32 columns, then row-major rationals. For the optional
  nullspace basis only, zero rows ends the matrix without a column field.
- A module is name string, version string, symbol-string array, rule array.
  Each rule is u32 head literal, array of u32 premises, provenance string.

Request payload begins with u8 operation:

| Value | Operation | Remaining payload |
| --- | --- | --- |
| 1 | Evaluate | Instruction array |
| 2 | Solve | Instruction array representing left minus right |
| 3 | Determinant | Matrix |
| 4 | Linear | Coefficient matrix, right-side matrix |
| 5 | Born | Amplitude matrix |
| 6 | Prove | Module, u32 goal, u32 event limit, u32 depth limit, u8 GPU flag |

Instructions: u8 opcode; only opcode 1 also has a rational operand.
Opcodes are `1 constant, 2 x, 3 add, 4 subtract, 5 multiply, 6 divide, 7 negate`.
They form a validated postfix expression with a final stack size of one.

Result payload begins with four u8 fields: operation, status, limited flag,
GPU flag. Flags are 0 or 1. Status codes: `1 OK, 2 INFINITE_SOLUTIONS,
3 INCONSISTENT, 4 PROVED, 5 DISPROVED, 6 UNKNOWN, 7 CONTRADICTION`.

Math results then contain a rational values array and optional basis matrix.
Scalar results have one value; unique solve results have one value; unique
linear results have a solution vector. Infinite linear results additionally
contain one nullspace vector per basis row. Born results contain probabilities.
Infinite scalar equations and inconsistent systems have empty values/basis.

Proof results then contain module, u32 goal, u32 positive root, u32 negative
root, proof-step array, and search-event array. `0xffffffff` means absent root.
Each proof step contains u32 literal, u32 rule index, array of u32 dependencies.
Each event contains u8 kind, u32 literal, u32 rule (or `0xffffffff`), u32 depth.
Event kinds: `1 Visit, 2 TryRule, 3 Cached, 4 Cycle, 5 Backtrack, 6 Derived,
7 Bound`. On an exhausted event budget, the limited flag records termination
even when no additional event fits. Math result decoding validates structure
and invariants, not provenance/authentication of a remote calculation.

## Resource and operational contract

- Record: 1 MiB including header. Text/module input: 64 KiB.
- Matrix: each dimension 1..16. Rational numerator/denominator: 4096 bits.
  Arithmetic intermediates are checked after every scalar operation.
- Expression: 512 postfix instructions, nesting 128.
- Module: 1024 symbols, 512 rules, 16 premises per rule, provenance 1024 bytes.
- Proof: at most 2048 steps. Events: 1..4096, depth: 1..128; defaults maximal.
  Limits are serialized and can be lowered by typed callers.
- These are deterministic input/state bounds, not an OS memory or time sandbox.
  Library decompression memory and GPU driver resources belong to their backends.
- Status results exit 0, including unknown/contradiction/inconsistent.
  Input, codec, GPU, and arithmetic failures exit 2 and go to stderr.
  Incomplete/error-bearing standalone search exits 1.

Core tests link only `src/core.cpp tests/core_test.cpp -lgmpxx -lgmp`.
The optional `RuleMatcher` callback has signature
`vector<uint8_t>(const vector<uint64_t>& required_masks, uint64_t known_facts)`.
The CLI supplies the real parent-owned `gpu_rule_matches`; unit tests can
exercise the boundary with a deterministic oracle without claiming GPU use.
Parent-owned `search_library`, `check_library`, and `test_gpu_rules` remain
unchanged. `compile prove MODULE GOAL --gpu | run | decode` preserves GPU
selection. Retrieval options belong to human output, not the binary request.

## Explicit C++ English and evidence bridge

`ask QUESTION` recognizes the documented English templates in `README.md` and
`--help`. It produces the same AXRM v1 requests; the core and wire schema have
not acquired English semantics or learned weights.

Pure mathematics uses the existing typed operations. `Is ENTITY PROPERTY?`
and `Is ENTITY not PROPERTY?` produce ground positive/negative predicates
using a separately supplied `--module`. `compile ask QUESTION` supports these
formal translations, including `--module` and proof-only `--gpu`; it emits
binary only. Retrieval cannot occur inside binary compilation or execution.

`Research TOPIC.` performs indexed library lookup, without claiming a proof.
The explicit quantity-calculation template searches the selected library,
extracts only supported declarative numerical statements, and substitutes the
stated rational into an exact expression before AXRM execution.

The bridge's `grounding` field is distinct from the core arithmetic `status`:
`CONDITIONAL` means a checked calculation under cited source premises,
`UNKNOWN` means no supported premise, and `CONFLICTING_SOURCE_VALUES` means
the admitted source statements disagree. It is never an unconditional proof
of physical reality. These grounding/provenance fields are bridge output,
not new authenticated AXRM fields. The embedded core result certifies only
the arithmetic request it actually executed.

Supported extraction records include original excerpt byte offset and length.
Normalization has a reversible position map for admitted spans, including SI
spacing, multiplication signs, and Unicode minus. HTML paragraph boundaries
are retained. Source truth, full archive integrity, unrestricted language,
scientific-notation interpretation, and automatic discovery of new theories
remain outside this grammar.
