#include "relational.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
using namespace axiom;
std::size_t checks = 0;
void check(bool value) {
    ++checks;
    if (!value) {
        throw std::runtime_error("failed assertion " + std::to_string(checks));
    }
}
template<class F> void rejects(F&& f) {
    ++checks;
    try {
        f();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("expected rejection " + std::to_string(checks));
}
RelationalRequest request(const std::string& clauses, const std::string& goal = "p(a)") {
    return compile_relational(parse_relational_module(
        "axioms 2 test 1.0\nconstant a\nconstant b\n"
        "predicate p 1\npredicate q 1\npredicate edge 2\n" + clauses), goal);
}
void u32(Bytes& b, std::uint32_t v) {
    for (unsigned k = 0; k < 32; k += 8) {
        b.push_back(static_cast<std::uint8_t>(v >> k));
    }
}
void patch32(Bytes& b, std::size_t pos, std::uint32_t v) {
    for (unsigned k = 0; k < 4; ++k) {
        b.at(pos + k) = static_cast<std::uint8_t>(v >> (8 * k));
    }
}
void blob(Bytes& b, const Bytes& other) {
    u32(b, static_cast<std::uint32_t>(other.size()));
    b.insert(b.end(), other.begin(), other.end());
}
Bytes wrap(Bytes payload, std::uint8_t type = 2) {
    Bytes b{'A', 'X', 'R', 'M', 2, 0, type, 0};
    u32(b, static_cast<std::uint32_t>(payload.size()));
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
// Deliberately no producer validation; adversarial tests must reach the decoder.
Bytes raw_result(const RelationalResult& r, const Bytes& nested) {
    Bytes p{7};
    blob(p, encode_relational_request(r.request));
    u32(p, static_cast<std::uint32_t>(r.instances.size()));
    for (const auto& i : r.instances) {
        u32(p, i.source_rule);
        u32(p, static_cast<std::uint32_t>(i.substitution.size()));
        for (auto c : i.substitution) {
            u32(p, c);
        }
    }
    blob(p, nested);
    return wrap(std::move(p));
}
void roundtrip(const RelationalRequest& q, Status status) {
    const auto bytes = encode_relational_request(q);
    check(encode_relational_request(decode_relational_request(bytes)) == bytes);
    const auto r = execute_relational(q);
    check(r.proof.status == status);
    check(verify_relational_result(q, r));
    const auto encoded = encode_relational_result(r);
    check(encode_relational_result(decode_relational_result(encoded)) == encoded);
    check(run_relational_binary(bytes) == encoded);
    check(run_relational_binary(bytes) == run_relational_binary(bytes));
    check(!render_relational(r).empty());
}
void reasoning() {
    const auto module = load_relational_module("modules/relations.axioms");
    const auto q = compile_relational(module, "ancestor(ana,cy)");
    roundtrip(q, Status::Proved);
    const auto g = ground_relational(q);
    check(g.request.module.symbols.size() == 18);
    check(g.instances.size() == 38);
    check(g.instances[2] == GroundInstance{2, {0, 0}});
    check(g.instances[10] == GroundInstance{2, {2, 2}});
    check(g.instances[11] == GroundInstance{3, {0, 0, 0}});
    check(g.instances.back() == GroundInstance{3, {2, 2, 2}});
    check(g.request.module.rules[2].premises == std::vector<std::uint32_t>{0});
    check(g.request.module.rules[2].head == 18);
    check(g.request.goal == 22);
    roundtrip(compile_relational(module, "ancestor(cy,ana)"), Status::Unknown);
    roundtrip(request("fact !p(a) | explicit\n"), Status::Disproved);
    roundtrip(request("fact !p(a) | explicit\n", "!p(a)"), Status::Proved);
    roundtrip(request("fact p(a) | positive\n", "!p(a)"), Status::Disproved);
    roundtrip(request("fact !p(a) | negative\nfact p(a) | positive\n"), Status::Contradiction);
    roundtrip(request("fact !q(a) | negative\nfact q(a) | positive\n"), Status::Unknown);
    roundtrip(request("fact !q(a) | negative\nfact q(a) | positive\nfact p(a) | seed\n"), Status::Proved);
    roundtrip(request("fact !q(a) | negative\nrule !q(?x) -> p(?x) | explicit body\n"), Status::Proved);
    roundtrip(request("fact edge(a,b) | not diagonal\n"
                      "rule edge(?x,?x) -> p(?x) | repeated\n"), Status::Unknown);
    roundtrip(request("fact edge(a,a) | diagonal\n"
                      "rule edge(?x,?x) -> p(?x) | repeated\n"), Status::Proved);
    roundtrip(request("fact edge(a,b) | pair\nfact q(b) | right\n"
                      "rule edge(?x,?y) & q(?y) -> p(?x) | join\n"), Status::Proved);
    roundtrip(request("fact edge(a,b) | pair\nfact q(a) | wrong side\n"
                      "rule edge(?x,?y) & q(?y) -> p(?x) | join\n"), Status::Unknown);
    auto local = request("fact edge(a,b) | pair\n"
                         "rule edge(?x,?y) -> q(?y) | first scope\n"
                         "rule q(?x) -> p(?x) | second scope\n", "p(b)");
    check(local.module.rules[1].variables == 2 && local.module.rules[2].variables == 1);
    roundtrip(local, Status::Proved);
    auto cycle = request("rule q(?x) -> p(?x) | cycle\n"
                         "rule p(?x) -> q(?x) | cycle\n"
                         "rule edge(?x,?x) -> p(?x) | failed branch\n"
                         "fact p(a) | alternative\n"
                         "rule p(?x) & q(?x) -> edge(?x,?x) | cache join\n", "edge(a,a)");
    roundtrip(cycle, Status::Proved);
    const auto r = execute_relational(cycle);
    for (auto kind : {EventKind::Cycle, EventKind::Backtrack, EventKind::Cached}) {
        check(std::any_of(r.proof.history.begin(), r.proof.history.end(),
                          [&](const auto& e) { return e.kind == kind; }));
    }
    roundtrip(request("rule q(?x) -> p(?x) | loop\nrule p(?x) -> q(?x) | loop\n"),
              Status::Unknown);
    cycle.limits.events = 1;
    const auto limited = execute_relational(cycle);
    check(limited.proof.status == Status::Unknown && limited.proof.limited);
    check(limited.proof.history.size() == 1);
    check(verify_relational_result(cycle, limited));
    auto depth = request("fact q(a) | base\nrule q(?x) -> p(?x) | one\n"
                         "rule p(?x) -> edge(?x,?x) | two\n", "edge(a,a)");
    depth.limits.depth = 1;
    check(execute_relational(depth).proof.limited);
    roundtrip(compile_relational(parse_relational_module(
        "axioms 2 nullary 1\nconstant a\npredicate yes 0\nfact yes() | seed\n"), "yes()"),
        Status::Proved);
    const auto mixed = compile_relational(parse_relational_module(
        "axioms 2 mixed 1\nconstant a\nconstant b\npredicate ready 0\n"
        "predicate item 1\npredicate pair 2\npredicate done 0\n"
        "fact ready() | ready\nfact item(b) | item\nfact pair(a,b) | pair\n"
        "rule ready() & item(?x) & pair(a,?x) -> !done() | join\n"), "!done()");
    const auto mixed_ground = ground_relational(mixed);
    check(mixed_ground.request.module.symbols.size() == 8);
    check(mixed_ground.request.goal == 15);
    check(mixed_ground.request.module.rules[1].head == 4);
    check(mixed_ground.request.module.rules[2].head == 8);
    check(mixed_ground.request.module.rules[3].premises ==
          std::vector<std::uint32_t>({0, 2, 6}));
    check(mixed_ground.request.module.rules[4].premises ==
          std::vector<std::uint32_t>({0, 4, 8}));
    roundtrip(mixed, Status::Proved);
}
void tampering() {
    const auto q = request("fact q(a) | seed\n"
                           "rule q(?x) -> p(?x) | yes\n"
                           "rule q(?x) -> !p(?x) | opposite\n");
    const auto valid = execute_relational(q);
    check(valid.proof.status == Status::Contradiction);
    auto bad = valid;
    bad.instances.pop_back();
    check(!verify_relational_result(q, bad));
    bad = valid;
    std::swap(bad.instances[1], bad.instances[2]);
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.instances[1].source_rule = 0;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.instances[1].substitution[0] = 2;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.instances[1].substitution.clear();
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.instances.push_back(valid.instances.back());
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.module.symbols[0] = "forged";
    check(!verify_relational_result(q, bad));
    bad = valid;
    std::swap(bad.proof.module.symbols[0], bad.proof.module.symbols[1]);
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.goal ^= 1;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.module.rules[1].head += 2;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.module.rules[1].provenance = "different";
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.request.module.rules[0].provenance = "changed echo";
    check(!verify_relational_result(q, bad));
    auto other = q;
    other.goal.negative = true;
    check(!verify_relational_result(other, valid));
    other = q;
    other.limits.events = 4000;
    check(!verify_relational_result(other, valid));
    bad = valid;
    bad.proof.gpu = true;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.operation = Operation::Evaluate;
    check(!verify_relational_result(q, bad));
    bad = valid;
    bad.proof.proof.back().dependencies = {999};
    check(!verify_relational_result(q, bad));
    auto join = request("fact q(a) | q\nfact edge(a,b) | edge\n"
                        "rule q(?x) & edge(?x,?y) -> p(?y) | ordered\n", "p(b)");
    auto jr = execute_relational(join);
    std::swap(jr.proof.module.rules[2].premises[0], jr.proof.module.rules[2].premises[1]);
    check(!verify_relational_result(join, jr));

    // Remove the opposite-producing rule AND its instance. The v1 result is
    // individually sound, so only complete relational coverage catches this.
    auto reduced = ground_relational(q);
    reduced.request.module.rules.erase(reduced.request.module.rules.begin() + 3,
                                        reduced.request.module.rules.end());
    bad = valid;
    bad.instances.resize(3);
    bad.proof = execute(reduced.request);
    check(bad.proof.status == Status::Proved);
    check(decode_result(encode_result(bad.proof)).status == Status::Proved);
    check(!verify_relational_result(q, bad));
    rejects([&] { decode_relational_result(raw_result(bad, encode_result(bad.proof))); });
    rejects([&] { encode_relational_result(bad); });

    // Omit an unused substitution, including its otherwise harmless ground rule.
    reduced = ground_relational(q);
    reduced.request.module.rules.erase(reduced.request.module.rules.begin() + 4);
    bad = valid;
    bad.instances.erase(bad.instances.begin() + 4);
    bad.proof = execute(reduced.request);
    check(!verify_relational_result(q, bad));
    // Leave complete grounding but hide the opposite proof/status.
    bad = valid;
    bad.proof.status = Status::Proved;
    bad.proof.negative_root = no_step;
    check(!verify_relational_result(q, bad));
}

// Source-level oracle: instantiate atoms into strings, repeatedly enumerate
// assignments, and apply source clauses directly. It shares neither canonical
// literal IDs nor ground rules with the implementation.
std::set<std::string> closure(const RelationalModule& m) {
    std::set<std::string> known;
    const auto key = [&](const Atom& a, const std::vector<std::uint32_t>& assignment) {
        std::string s = (a.negative ? "!" : "") + m.predicates[a.predicate].name + "(";
        for (const auto& t : a.terms) {
            s += m.constants[t.kind == TermKind::Constant ? t.index : assignment.at(t.index)] + ",";
        }
        return s + ")";
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& r : m.rules) {
            std::vector<std::uint32_t> assignment;
            std::function<void()> visit = [&] {
                if (assignment.size() != r.variables) {
                    for (std::uint32_t c = 0; c < m.constants.size(); ++c) {
                        assignment.push_back(c);
                        visit();
                        assignment.pop_back();
                    }
                } else if (std::all_of(r.premises.begin(), r.premises.end(),
                              [&](const auto& p) { return known.contains(key(p, assignment)); })) {
                    changed = known.insert(key(r.head, assignment)).second || changed;
                }
            };
            visit();
        }
    }
    return known;
}
void oracle() {
    // Exhaust all 256 signed unary fact sets on a two-element domain, with
    // recursive signed Horn rules, repeated variables, and joins.
    for (unsigned mask = 0; mask < 256; ++mask) {
        std::string facts;
        for (unsigned bit = 0; bit < 8; ++bit) {
            if ((mask & (1U << bit)) != 0) {
                facts += "fact " + std::string(bit & 1 ? "!" : "") +
                    (bit / 4 ? "q" : "p") + "(" + (bit / 2 % 2 ? "b" : "a") + ") | seed\n";
            }
        }
        auto q = request(facts + "rule p(?x) -> q(?x) | forward\n"
                          "rule q(?x) -> p(?x) | cycle\n"
                          "rule !q(?x) -> !p(?x) | signed\n"
                          "rule p(?x) & q(?y) -> edge(?x,?y) | join\n"
                          "rule edge(?x,?x) & !p(?x) -> !q(?x) | repeated\n");
        const auto expected = closure(q.module);
        for (const auto* pred : {"p", "q", "edge"}) {
            for (const auto* c : {"a", "b"}) {
                for (const bool neg : {false, true}) {
                    const std::string args = std::string(c) + (std::string(pred) == "edge" ? ",a" : "");
                    const auto goal = std::string(neg ? "!" : "") + pred + "(" + args + ")";
                    q.goal = parse_relational_goal(q.module, goal);
                    auto key = std::string(pred) + "(" + c + "," +
                        (std::string(pred) == "edge" ? "a," : "") + ")";
                    const bool pos = expected.contains((neg ? "!" : "") + key);
                    const bool opposite = expected.contains((neg ? "" : "!") + key);
                    const auto status = pos ? (opposite ? Status::Contradiction : Status::Proved) :
                        (opposite ? Status::Disproved : Status::Unknown);
                    const auto r = execute_relational(q);
                    check(!r.proof.limited && r.proof.status == status);
                    check(verify_relational_result(q, r));
                }
            }
        }
    }
}

void bounds_and_syntax() {
    for (const auto& source : {
        "", "axioms 1 wrong 1\n", "axioms 2 bad 1 extra\n",
        "axioms 2 bad 1\npredicate p 1\n",
        "axioms 2 bad 1\nconstant a\n",
        "axioms 2 bad 1\nconstant a\nconstant a\npredicate p 1\n",
        "axioms 2 bad 1\nconstant a\npredicate p 1\npredicate p 2\n",
        "axioms 2 bad 1\nconstant a\npredicate p 5\n"}) {
        rejects([&] { parse_relational_module(source); });
    }
    for (const auto& clauses : {
        "fact p(?x) | variable\n",
        "fact p(c) | unknown constant\n",
        "fact unknown(a) | undeclared\n",
        "fact p(a,b) | arity\n",
        "fact p(f(a)) | function\n",
        "fact p(a) | \n",
        "fact p(a)\n",
        "fact !!p(a) | sign\n",
        "rule p(?x) -> q(?y) | existential\n",
        "rule -> p(a) | empty body\n",
        "rule p(?x) -> q(?x) -> p(?x) | heads\n",
        "rule not p(?x) -> q(?x) | naf\n",
        "rule p(?x) & ?x=?y -> q(?y) | equality\n",
        "fact p(a) | one | two\n",
        "fact p(a) | fact\nconstant c\n"}) {
        rejects([&] { request(clauses); });
    }
    auto base = request("");
    for (const auto& goal : {"p(?x)", "p(c)", "p(f(a))", "p(a) trailing", "!!p(a)", "p()", ""}) {
        rejects([&] { parse_relational_goal(base.module, goal); });
    }
    rejects([&] { parse_relational_module(std::string(max_text_bytes + 1, ' ')); });
    rejects([&] { parse_relational_goal(base.module, std::string(max_text_bytes + 1, ' ')); });
    rejects([] { load_relational_module("modules/does-not-exist.axioms"); });
    auto bad = base;
    bad.gpu = true;
    rejects([&] { execute_relational(bad); });
    rejects([&] { encode_relational_request(bad); });
    rejects([&] { compile_relational(base.module, "p(a)", {}, true); });
    for (auto n : {0U, 4097U, UINT32_MAX}) {
        bad = base;
        bad.limits.events = n;
        rejects([&] { ground_relational(bad); });
    }
    for (auto n : {0U, 129U, UINT32_MAX}) {
        bad = base;
        bad.limits.depth = n;
        rejects([&] { ground_relational(bad); });
    }
    bad = base;
    bad.module.constants.clear();
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.constants.clear();
    for (unsigned i = 0; i < 64; ++i) {
        bad.module.constants.push_back("c" + std::to_string(i));
    }
    check(ground_relational(compile_relational(
        RelationalModule{"many", "1", bad.module.constants, {{"p", 1}}, {}}, "p(c63)"))
        .request.module.symbols.size() == 64);
    bad.module.constants.push_back("overflow");
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.predicates.assign(64, {"p", 0});
    for (unsigned i = 0; i < 64; ++i) {
        bad.module.predicates[i].name += std::to_string(i);
    }
    bad.goal.terms.clear();
    check(ground_relational(bad).request.module.symbols.size() == 64);
    bad.module.predicates.push_back({"overflow", 0});
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.predicates[0].arity = 5;
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.constants = {"a", "b", "c", "d"};
    bad.module.predicates = {{"p", 4}, {"q", 4}, {"r", 4}, {"s", 4}};
    bad.goal.terms.assign(4, {});
    check(ground_relational(bad).request.module.symbols.size() == 1024);
    bad.module.predicates.push_back({"t", 0});
    rejects([&] { ground_relational(bad); });
    bad = base;
    RelationalRule rule;
    rule.variables = 8;
    rule.head = Atom{0, false, {{TermKind::Variable, 0}}};
    for (std::uint32_t i = 0; i < 8; ++i) {
        rule.premises.push_back({0, false, {{TermKind::Variable, i}}});
    }
    rule.provenance = "eight variables";
    bad.module.rules = {rule, rule};
    check(ground_relational(bad).instances.size() == 512);
    bad.module.rules.push_back({0, base.goal, {}, "one too many"});
    rejects([&] { ground_relational(bad); });
    bad.module.rules = {rule};
    bad.module.constants.push_back("c");
    rejects([&] { ground_relational(bad); }); // 3^8 cannot overflow or truncate
    bad.module.constants = {"a"};
    check(ground_relational(bad).instances.size() == 1);
    bad.module.rules[0].variables = 9;
    rejects([&] { ground_relational(bad); });
    bad = request("rule p(?x) -> q(?x) | scope\n");
    bad.module.rules[0].variables = 2;
    rejects([&] { ground_relational(bad); }); // gap/body absence
    bad.module.rules[0].variables = UINT32_MAX;
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.goal.terms[0].kind = static_cast<TermKind>(99);
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.goal.predicate = UINT32_MAX;
    rejects([&] { ground_relational(bad); });
    bad = request("fact p(a) | x\n");
    bad.module.rules[0].premises.assign(16, base.goal);
    check(ground_relational(bad).instances.size() == 1);
    bad.module.rules[0].premises.push_back(base.goal);
    rejects([&] { ground_relational(bad); });
    bad = request("fact p(a) | x\n");
    bad.module.rules[0].provenance.assign(1024, 'x');
    check(ground_relational(bad).instances.size() == 1);
    bad.module.rules[0].provenance.push_back('x');
    rejects([&] { ground_relational(bad); });
    bad = request("fact p(a) | x\n");
    const auto repeated_rule = bad.module.rules.front();
    bad.module.rules.assign(512, repeated_rule);
    check(ground_relational(bad).instances.size() == 512);
    bad.module.rules.push_back(bad.module.rules.front());
    rejects([&] { ground_relational(bad); });
    bad.module.rules.pop_back();
    for (auto& r : bad.module.rules) {
        r.provenance.assign(1024, 'x');
    }
    rejects([&] { execute_relational(bad); }); // combined echo+nested budget
    bad = request("fact p(a) | x\n");
    bad.module.rules[0].provenance = std::string("x\0y", 3);
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.constants[0].assign(129, 'a');
    rejects([&] { ground_relational(bad); });
    bad = base;
    bad.module.version.assign(129, 'a');
    rejects([&] { ground_relational(bad); });
}

struct Cursor {
    const Bytes& b;
    std::size_t pos = 13;
    std::uint32_t get() {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(b.at(pos++)) << (8 * i);
        }
        return v;
    }
    void text() {
        const auto n = get();
        pos += n;
    }
};
void wire() {
    const auto q = request("fact q(a) | seed\nrule q(?x) -> p(?x) | lift\n");
    const auto r = execute_relational(q);
    const auto qb = encode_relational_request(q);
    const auto rb = encode_relational_result(r);
    for (std::size_t n = 0; n < qb.size(); ++n) {
        rejects([&] { decode_relational_request(std::span(qb).first(n)); });
    }
    for (std::size_t n = 0; n < rb.size(); ++n) {
        rejects([&] { decode_relational_result(std::span(rb).first(n)); });
    }
    for (const auto* record : {&qb, &rb}) {
        for (std::size_t pos : {0U, 4U, 5U, 6U, 7U, 8U, 12U}) {
            auto b = *record;
            b[pos] = 255;
            rejects([&] {
                if (record == &qb) {
                    decode_relational_request(b);
                } else {
                    decode_relational_result(b);
                }
            });
        }
        auto trailing = *record;
        trailing.push_back(0);
        rejects([&] {
            if (record == &qb) {
                decode_relational_request(trailing);
            } else {
                decode_relational_result(trailing);
            }
        });
        patch32(trailing, 8, static_cast<std::uint32_t>(trailing.size() - 12));
        rejects([&] {
            if (record == &qb) {
                decode_relational_request(trailing);
            } else {
                decode_relational_result(trailing);
            }
        });
    }
    Cursor c{qb};
    c.text(); c.text();
    const auto constants_pos = c.pos;
    const auto constants = c.get();
    for (std::uint32_t i = 0; i < constants; ++i) {
        c.text();
    }
    const auto pred_pos = c.pos;
    const auto predicates = c.get();
    const auto pred_name = c.pos;
    for (std::uint32_t i = 0; i < predicates; ++i) {
        c.text(); c.get();
    }
    const auto rule_count_pos = c.pos;
    c.get();
    const auto variables_pos = c.pos;
    c.get();
    const auto atom_pos = c.pos;
    for (auto [pos, value] : std::vector<std::pair<std::size_t, std::uint32_t>>{
        {constants_pos, 65}, {pred_pos, 65}, {rule_count_pos, 513},
        {variables_pos, 9}, {atom_pos, 999}, {atom_pos + 5, 5}, {atom_pos + 10, 999},
        {pred_name, UINT32_MAX}}) {
        auto b = qb;
        patch32(b, pos, value);
        rejects([&] { decode_relational_request(b); });
    }
    for (const auto pos : {atom_pos + 4, atom_pos + 9, qb.size() - 1}) {
        auto b = qb;
        b[pos] = 3;
        rejects([&] { decode_relational_request(b); });
    }
    auto b = qb;
    b.at(b.size() - 1) = 1;
    rejects([&] { decode_relational_request(b); });
    rejects([&] { decode_relational_result(qb); });
    rejects([&] { decode_relational_request(rb); });
    rejects([&] { decode_request(qb); });
    rejects([&] { decode_result(rb); });
    auto nested = encode_result(r.proof);
    nested[4] = 2;
    rejects([&] { decode_relational_result(raw_result(r, nested)); });
    rejects([&] { decode_relational_result(raw_result(r, rb)); });
    rejects([&] { decode_relational_result(raw_result(r, qb)); });
    auto math = encode_result(execute(compile({"eval", "1+1"})));
    rejects([&] { decode_relational_result(raw_result(r, math)); });
    auto ground = ground_relational(q);
    rejects([&] { decode_relational_result(raw_result(r, encode_request(ground.request))); });
    nested = encode_result(r.proof);
    nested[12] = 7;
    rejects([&] { decode_relational_result(raw_result(r, nested)); });
    nested = encode_result(r.proof);
    nested.push_back(0);
    rejects([&] { decode_relational_result(raw_result(r, nested)); });
    check(decode_request(encode_request(ground.request)).operation == Operation::Prove);
    check(decode_result(run_binary(encode_request(ground.request))).status == Status::Proved);
    check(decode_result(run_binary(encode_request(compile({"eval", "2+3"})))).values[0] == 5);
    // Existing GPU callback still works for a v1 ground proof.
    auto gpu = compile_proof(parse_module("axioms 1 gpu 1\nfact x | seed\n"), "x");
    gpu.gpu = true;
    const auto matcher = [](const std::vector<std::uint64_t>& masks, std::uint64_t known) {
        std::vector<std::uint8_t> out;
        for (auto m : masks) {
            out.push_back((m & known) == m);
        }
        return out;
    };
    check(decode_result(run_binary(encode_request(gpu), matcher)).status == Status::Proved);
    rejects([&] { decode_relational_request(encode_request(ground.request)); });
    rejects([&] { decode_relational_result(encode_result(r.proof)); });
}
} // namespace
int main() {
    try {
        reasoning();
        tampering();
        bounds_and_syntax();
        wire();
        oracle();
        std::cout << "relational tests: " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "relational tests: " << e.what() << '\n';
        return 1;
    }
}
