# Finite relational Horn unit

`include/relational.hpp` and `src/relational.cpp` extend the existing CPU ground
proof engine without changing AXRM v1. The CLI dispatches AXRM v1 to the existing
engine and AXRM v2 to the finite relational engine.

```sh
./build/axiom prove-rel modules/relations.axioms 'ancestor(ana,cy)'
./build/axiom compile-rel modules/relations.axioms 'ancestor(ana,cy)' |
  ./build/axiom run | ./build/axiom decode
```

Both paths return PROVED for this example. The reverse goal
`ancestor(cy,ana)` returns UNKNOWN. This formal interface does not provide
learned English-to-relational translation. Relational execution is CPU-only;
existing AXRM v1 GPU proof dispatch remains supported.

## Source syntax

```text
axioms 2 ancestry 1.0
constant ana
constant bo
constant cy
predicate parent 2
predicate ancestor 2
fact parent(ana,bo) | declared A
fact parent(bo,cy) | declared B
rule parent(?x,?y) -> ancestor(?x,?y) | definition
rule parent(?x,?z) & ancestor(?z,?y) -> ancestor(?x,?y) | recursion
```

Declarations precede all clauses. Constants and predicates are indexed by their
declaration order, not sorted by name. Each namespace has unique identifiers
matching `[A-Za-z_][A-Za-z0-9_.]*`, at most 128 bytes. Module names use the same
grammar; versions are nonempty tokens of at most 128 bytes. Blank lines and
whole-line `#` comments are allowed. Provenance after the one `|` is mandatory,
opaque text, not an instruction. Metadata excludes control bytes.

An atom is optional `!`, predicate name, and parenthesized comma-separated terms.
Zero-arity predicates use `p()`. Terms are declared constants or `?IDENTIFIER`
variables. Whitespace around punctuation is allowed. Variables are local to a
single source rule and numbered densely by first occurrence, scanning ordered
premises before the head. Every variable must occur in a premise. Facts and
goals must be ground. All constants and predicates must be declared before use.
Rule bodies are nonempty and `&`-separated, with exactly one `->` and one head.
Repeated terms and repeated premises retain their identity and order.

This is a single finite constant domain: term tags distinguish constants from
variables, not user-defined sorts. There are no function constructors, equality
solver, existential heads, implicit constants, or negation-as-failure. Explicit
negative premises and heads work exactly like signed v1 literals. Results retain
v1 `Proved`, `Disproved`, `Unknown`, and `Contradiction` semantics relative to the
signed goal. Unrelated contradictions do not explode. Search budgets can yield
`Unknown` with `limited=true`; this is not a claim of nonderivability.

## Public C++20 API

All names are in `namespace axiom`. `Bytes`, `Limits`, `Request`, and `Result`
come from `core.hpp`. Inputs are immutable except where passed by value.

```cpp
enum class TermKind : std::uint8_t { Constant = 1, Variable = 2 };
struct Term { TermKind kind = TermKind::Constant; std::uint32_t index = 0; };
struct Predicate { std::string name; std::uint32_t arity = 0; };
struct Atom {
    std::uint32_t predicate = 0;
    bool negative = false;
    std::vector<Term> terms;
};
struct RelationalRule {
    std::uint32_t variables = 0;
    Atom head;
    std::vector<Atom> premises;
    std::string provenance;
};
struct RelationalModule {
    std::string name, version;
    std::vector<std::string> constants;
    std::vector<Predicate> predicates;
    std::vector<RelationalRule> rules;
};
struct RelationalRequest {
    RelationalModule module;
    Atom goal;
    Limits limits;
    bool gpu = false;
};
struct GroundInstance {
    std::uint32_t source_rule = 0;
    std::vector<std::uint32_t> substitution;
};
struct Grounding {
    Request request;
    std::vector<GroundInstance> instances;
};
struct RelationalResult {
    RelationalRequest request;
    std::vector<GroundInstance> instances;
    Result proof;
};

RelationalModule parse_relational_module(const std::string& source);
RelationalModule load_relational_module(const std::filesystem::path& path);
Atom parse_relational_goal(const RelationalModule& module, const std::string& goal);
RelationalRequest compile_relational(RelationalModule module, const std::string& goal,
                                     Limits limits = {}, bool gpu = false);
Grounding ground_relational(const RelationalRequest& request);
Bytes encode_relational_request(const RelationalRequest& request);
RelationalRequest decode_relational_request(std::span<const std::uint8_t> bytes);
Bytes encode_relational_result(const RelationalResult& result);
RelationalResult decode_relational_result(std::span<const std::uint8_t> bytes);
RelationalResult execute_relational(const RelationalRequest& request);
bool verify_relational_result(const RelationalRequest& request,
                              const RelationalResult& result);
std::string render_relational(const RelationalResult& result);
Bytes run_relational_binary(std::span<const std::uint8_t> request);
```

`Term`, `Predicate`, `Atom`, `RelationalRule`, `RelationalModule`, and
`GroundInstance` also provide defaulted equality. A `Bytes` vector converts to the
span parameters directly. Invalid input throws `std::runtime_error`; verification
returns `false` for invalid input/certificates. File loading is explicit and
bounded. The binary runner performs no filesystem access.

`compile_relational` parses a ground goal and performs admission. Typed callers
may construct a request directly; all executable and codec boundaries validate
it. `ground_relational` exposes the complete CPU v1 request and certificate.
`execute_relational` calls existing `axiom::execute` on that request and checks the
result. `gpu=true` is explicitly rejected; v1 GPU code is untouched.

`decode_relational_result` validates the embedded request and its complete
certificate. Call `verify_relational_result(original_request, decoded_result)`
to bind a returned result to the caller's original theory, goal, and limits.
Self-consistency alone does not authenticate which request a remote party used.
Rendering includes the atom dictionary, source-rule substitutions, and the
existing v1 proof/history summary.

## Canonical grounding and verification

For each predicate in declaration order, enumerate all tuples of domain
constants in lexicographic index order, last coordinate varying fastest.
Nullary predicates have one tuple. These unsigned atoms receive consecutive
indices, and collision-free ground symbols `r0`, `r1`, ... . A signed literal is
`2*index + negative`. No user identifier is concatenated into an internal name.

For each source rule in source order, enumerate **all** substitutions in
lexicographic constant-index order for variables `0..variables-1`, last variable
varying fastest. A variable-free rule contributes exactly one empty substitution,
including facts. Instantiate even rules whose premises cannot be proved.
Heads, ordered premises, and provenance are preserved; rules are not deduplicated.
The result contains one `GroundInstance` per generated rule.

The verifier does not call the producer grounder, tuple decoder, or literal
mapper. It builds an independent predicate/tuple table by prefix expansion,
checks every symbol, and traverses all rule substitutions with a separate
odometer. It checks each certificate position, substitution vector, rule head,
ordered premises, provenance, and goal against that table. Missing, duplicate,
extra, reordered, or out-of-domain instances fail, even when no positive proof
uses them. Only after complete coverage is established does the existing v1
codec validate proof dependencies, roots, statuses, and independent monotone
closure for hidden opposite conclusions.

Limits are admission bounds, not an OS resource sandbox:

- Constants: 1..64; predicates: 1..64; arity: 0..4.
- Variables per rule: 0..8, dense and all occurring in a premise.
- Unsigned ground atoms: at most 1024; signed literals: at most 2048.
- Source and generated rules: each at most 512; premises: at most 16.
- Source/goal text: at most 65536 bytes; provenance: 1..1024 bytes.
- Search events: 1..4096; depth: 1..128; defaults inherited from `Limits`.
- Every record, including envelope: at most 1 MiB.

Checked additions, multiplications, and powers enforce limits before enumeration
or search. Admission conservatively budgets a v2 result with the full echoed
request, every instance, the complete ground module, up to two proof steps per
unsigned atom with sixteen dependencies each, and the full requested history.
The bound may reject a theory whose particular search result would be smaller;
it never truncates a theory to fit. Lower event limits reduce that part of the
budget. Typed requests have the same structural/wire bounds; the source-text
limit applies when parsing/loading textual input.

## AXRM v2 wire layout

All integers are unsigned little-endian. Arrays have a u32 element count.
Strings have u32 byte length followed by bytes. A blob has u32 byte length
followed by exactly that many bytes. No padding or native object layouts occur.

The unchanged 12-byte envelope layout is:

| Offset | Value |
| --- | --- |
| 0..3 | ASCII `AXRM` |
| 4..5 | u16 version `2` |
| 6 | u8 record type: request `1`, result `2` |
| 7 | reserved u8, must be `0` |
| 8..11 | u32 payload byte length |

Each payload begins with u8 operation `7` (relational proof).

Request remainder, in order:

1. Module name string, module version string.
2. Constant-string array.
3. Predicate array: each name string, u32 arity.
4. Source-rule array: each u32 variable count, head atom, premise-atom array,
   provenance string.
5. Ground goal atom.
6. u32 event limit, u32 depth limit, u8 GPU flag (only `0` is supported).

Atom encoding: u32 predicate index, u8 negative flag (`0` or `1`), term array.
Each term is u8 kind (`1` constant, `2` variable), u32 index into the constant
domain or rule-local variables. All indices, arities, tags, and safety conditions
are validated. Neither variable names nor source text are encoded.

Result remainder, in order:

1. Blob containing a **complete AXRM v2 request record**, including its envelope.
2. GroundInstance array: each u32 source-rule index, array of u32 substitution
   constant indices.
3. Blob containing a **complete AXRM v1 result record**, including its envelope.

There are no duplicated status or limit-result fields: these belong only to the
nested v1 `Result`. That record must be operation `6` (`Prove`) and CPU, not math,
not a request, and never nested v2. The echoed request includes input budgets;
the nested `limited` flag describes search termination.

Every level enforces exact payload lengths and end-of-record. Truncation,
trailing data (even with a patched envelope length), invalid tags/types/versions,
invalid booleans, and invalid nested records fail. Legacy v1 decoders continue
to reject v2. `binary_dispatch.cpp` selects the version before calling the
appropriate decoder.

## Standalone validation

From the `ai-design` directory:

```sh
make build/relational-test build/binary-dispatch-test
./build/relational-test
./build/binary-dispatch-test
```

Tests cover signed outcomes, ancestry, repeated variables, joins, local scope,
cycles/backtracking/cache, search bounds, canonical order, complete coverage,
request binding, adversarial v2/nested records, every truncated prefix, v1
compatibility, and deterministic bytes. A separate source-level finite closure
oracle exhausts 256 signed fact sets and checks all twelve signed unary/binary
goals per set. It never calls the grounder or uses its numeric literal mapping.
