# Scoped source compilation

`ZimSourceArchive::read` returns complete raw document Evidence. It does not
convert source statements into trusted premises. Oversized documents fail
instead of being truncated.

The interpreter supplies the normal shared archive DSL and the ObjectId of
the source it translated. `compile_source` checks serialization and source
binding; it also preserves clarification and unsupported outcomes.
`validate_source_candidate` checks the existing native theory representation
against host-owned units and the complete source view. Neither operation
certifies that the English translation is faithful.

## Immutable object dependencies

`materialize_source` produces:

- An Assumption containing the existing serialized `ProblemContext`: exact
  unit definitions, registry identity/version, source bytes and locators.
  Its format records whether selectors use offset-v1 or quote-v1.
- A Claim retaining the exact original archive DSL. It depends on the raw
  source and on the context object.
- One scoped Claim for each quantity binding or equation, and one scoped
  Rule for each finite-Horn rule. Each depends on the complete module.

All generated objects inherit source visibility and provenance. None is a
Fact or receives an authoritative assessment automatically. Unit definitions,
registry versions and selector modes participate in context identity; changing
them changes the module and all member identities. Journal dependency
invalidation reaches these downstream objects without rewriting their bytes.

## Member locators

The format `gammathink.source-member.v1` contains a UTF-8 JSON Pointer:

```text
/theory/bindings/0
/theory/equations/0
/theory/rules/0
```

Indices are canonical unsigned decimal without leading zeroes. These are
metadata locators into the existing shared parsed-JSON profile, not another
meaning schema. Source citations, signs, scopes, constant/variable references,
ordered jointly required premises and alternative rules stay in that profile.

`source_member_json` checks module/context identities, source coverage, selector
mode, native representation, member kind and index before returning the
selected tuple. That tuple is not standalone: its indices still refer to the
complete module and host context. A successful read does not authorize treating
the source interpretation as an empirically established fact.

Materialization has object-count and aggregate encoded-byte limits. The
existing native evidence-view limit remains 65536 bytes. Larger raw documents
must not be silently truncated to obtain admission.

Generated-ZIM integration uses a supervisor-supplied translation fixture,
not measured neural interpretation. Production interpreter wiring, diverse
real-library fidelity, whole-application resource use and frontier comparison
remain separate acceptance gates.
