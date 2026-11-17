#include "relational.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace axiom {
namespace {
void need(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::size_t add(std::size_t a, std::size_t b) {
    need(a <= max_wire_bytes && b <= max_wire_bytes - a, "relational size limit exceeded");
    return a + b;
}
std::size_t mul(std::size_t a, std::size_t b) {
    need(a == 0 || b <= max_wire_bytes / a, "relational size limit exceeded");
    return a * b;
}
std::size_t power(std::size_t base, std::uint32_t exponent) {
    std::size_t value = 1;
    for (std::uint32_t i = 0; i < exponent; ++i) {
        value = mul(value, base);
    }
    return value;
}
bool identifier(const std::string& text) {
    const auto alpha = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (text.empty() || text.size() > 128 || !alpha(text.front())) {
        return false;
    }
    return std::all_of(text.begin(), text.end(), [&](unsigned char c) {
        return alpha(c) || (c >= '0' && c <= '9') || c == '.';
    });
}
bool text_ok(const std::string& s, std::size_t bound) {
    return !s.empty() && s.size() <= bound &&
        std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 32 && c != 127; });
}
std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" :
        s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto end = s.find(delimiter, start);
        out.push_back(trim(s.substr(start, end - start)));
        if (end == std::string::npos) {
            return out;
        }
        start = end + 1;
    }
}
void validate_atom(const RelationalModule& m, const Atom& a, std::uint32_t variables,
                   std::vector<bool>* used = nullptr) {
    need(a.predicate < m.predicates.size(), "invalid predicate index");
    need(a.terms.size() == m.predicates[a.predicate].arity, "predicate arity mismatch");
    for (const auto& t : a.terms) {
        if (t.kind == TermKind::Constant) {
            need(t.index < m.constants.size(), "invalid constant index");
        } else {
            need(t.kind == TermKind::Variable && t.index < variables, "invalid variable");
            if (used) {
                (*used)[t.index] = true;
            }
        }
    }
}
// Shape validation is shared; the verifier below does not share enumeration or
// literal mapping with the producer.
void validate_module(const RelationalModule& m) {
    need(identifier(m.name) && text_ok(m.version, 128), "invalid relational metadata");
    need(!m.constants.empty() && m.constants.size() <= 64, "constant count must be 1..64");
    need(!m.predicates.empty() && m.predicates.size() <= 64, "predicate count must be 1..64");
    need(m.rules.size() <= 512, "source rule limit exceeded");
    std::set<std::string> constants, predicates;
    for (const auto& c : m.constants) {
        need(identifier(c) && constants.insert(c).second, "invalid or duplicate constant");
    }
    for (const auto& p : m.predicates) {
        need(identifier(p.name) && predicates.insert(p.name).second && p.arity <= 4,
             "invalid or duplicate predicate");
    }
    for (const auto& r : m.rules) {
        need(r.variables <= 8 && r.premises.size() <= 16, "rule bounds exceeded");
        need(text_ok(r.provenance, 1024), "invalid provenance");
        std::vector<bool> used(r.variables, false);
        for (const auto& a : r.premises) {
            validate_atom(m, a, r.variables, &used);
        }
        need(std::all_of(used.begin(), used.end(), [](bool v) { return v; }),
             "variables must be dense and occur in body");
        validate_atom(m, r.head, r.variables);
    }
}
std::size_t atom_size(const Atom& a) {
    return add(9, mul(5, a.terms.size()));
}
std::size_t request_body_size(const RelationalRequest& q) {
    const auto& m = q.module;
    std::size_t size = 1; // opcode
    size = add(size, add(4, m.name.size()));
    size = add(size, add(4, m.version.size()));
    size = add(size, 12); // constant, predicate and rule counts
    for (const auto& c : m.constants) {
        size = add(size, add(4, c.size()));
    }
    for (const auto& p : m.predicates) {
        size = add(size, add(8, p.name.size()));
    }
    for (const auto& r : m.rules) {
        size = add(size, add(12, r.provenance.size()));
        size = add(size, atom_size(r.head));
        for (const auto& a : r.premises) {
            size = add(size, atom_size(a));
        }
    }
    return add(size, add(atom_size(q.goal), 9));
}
struct Admission {
    std::size_t atoms = 0;
    std::size_t rules = 0;
};
Admission admit(const RelationalRequest& q) {
    validate_module(q.module);
    validate_atom(q.module, q.goal, 0);
    need(!q.gpu, "relational GPU execution is unsupported");
    need(q.limits.events > 0 && q.limits.events <= 4096 &&
         q.limits.depth > 0 && q.limits.depth <= 128, "invalid relational search limits");
    const auto& m = q.module;
    Admission out;
    for (const auto& p : m.predicates) {
        out.atoms = add(out.atoms, power(m.constants.size(), p.arity));
    }
    need(out.atoms <= 1024, "ground atom limit exceeded");
    // Exact ground-module wire size: internal names r<ordinal> have <=5 bytes.
    // This conservative bound also budgets every possible proof literal with
    // sixteen dependencies, and the full requested event budget, before search.
    std::size_t ground_module = add(add(20, m.name.size()), m.version.size());
    ground_module = add(ground_module, mul(out.atoms, 9));
    std::size_t instances = 4;
    for (const auto& r : m.rules) {
        const auto count = power(m.constants.size(), r.variables);
        out.rules = add(out.rules, count);
        need(out.rules <= 512, "generated rule limit exceeded");
        instances = add(instances, mul(count, add(8, mul(4, r.variables))));
        ground_module = add(ground_module,
            mul(count, add(add(12, mul(4, r.premises.size())), r.provenance.size())));
    }
    const auto nested = add(add(40, ground_module),
        add(mul(mul(out.atoms, 2), 76), mul(q.limits.events, 13)));
    const auto total = add(13, add(add(4, add(12, request_body_size(q))),
                                   add(instances, add(4, nested))));
    need(total <= max_wire_bytes, "worst-case relational result exceeds 1 MiB");
    return out;
}
Atom parse_atom(const RelationalModule& m, std::string source,
                std::map<std::string, std::uint32_t>& variables, bool allow_variables) {
    source = trim(source);
    Atom a;
    if (!source.empty() && source.front() == '!') {
        a.negative = true;
        source = trim(source.substr(1));
    }
    const auto open = source.find('(');
    need(open != std::string::npos && !source.empty() && source.back() == ')' &&
         source.find('(', open + 1) == std::string::npos &&
         source.find(')') == source.size() - 1, "expected function-free atom");
    const auto name = trim(source.substr(0, open));
    const auto p = std::find_if(m.predicates.begin(), m.predicates.end(),
                              [&](const auto& pred) { return pred.name == name; });
    need(p != m.predicates.end(), "undeclared predicate");
    a.predicate = static_cast<std::uint32_t>(p - m.predicates.begin());
    const auto arguments = trim(source.substr(open + 1, source.size() - open - 2));
    if (!arguments.empty()) {
        for (auto term : split(arguments, ',')) {
            if (!term.empty() && term.front() == '?') {
                term = term.substr(1);
                need(allow_variables && identifier(term), "invalid or nonground variable");
                auto [it, inserted] = variables.emplace(term, static_cast<std::uint32_t>(variables.size()));
                static_cast<void>(inserted);
                need(variables.size() <= 8, "variable count exceeded");
                a.terms.push_back({TermKind::Variable, it->second});
            } else {
                const auto c = std::find(m.constants.begin(), m.constants.end(), term);
                need(c != m.constants.end(), "undeclared constant");
                a.terms.push_back({TermKind::Constant, static_cast<std::uint32_t>(c - m.constants.begin())});
            }
        }
    }
    need(a.terms.size() == p->arity, "predicate arity mismatch");
    return a;
}
std::uint32_t literal(const RelationalModule& m, const Atom& a,
                      const std::vector<std::uint32_t>& substitution,
                      const std::vector<std::size_t>& offsets) {
    std::size_t tuple = 0;
    for (const auto& t : a.terms) {
        tuple = tuple * m.constants.size() +
            (t.kind == TermKind::Constant ? t.index : substitution[t.index]);
    }
    return static_cast<std::uint32_t>((offsets[a.predicate] + tuple) * 2 + (a.negative ? 1 : 0));
}
std::vector<std::uint32_t> tuple_at(std::size_t ordinal, std::uint32_t width, std::size_t c) {
    std::vector<std::uint32_t> tuple(width);
    for (auto i = width; i > 0; --i) {
        tuple[i - 1] = static_cast<std::uint32_t>(ordinal % c);
        ordinal /= c;
    }
    return tuple;
}

struct Writer {
    Bytes bytes;
    void u8(std::uint8_t v) {
        need(bytes.size() < max_wire_bytes, "relational record too large");
        bytes.push_back(v);
    }
    void u32(std::size_t v) {
        need(v <= UINT32_MAX, "integer overflow");
        for (unsigned i = 0; i < 32; i += 8) {
            u8(static_cast<std::uint8_t>(v >> i));
        }
    }
    void blob(std::span<const std::uint8_t> b) {
        u32(b.size());
        for (auto v : b) {
            u8(v);
        }
    }
    void text(const std::string& s) {
        u32(s.size());
        for (unsigned char c : s) {
            u8(c);
        }
    }
    void atom(const Atom& a) {
        u32(a.predicate);
        u8(a.negative ? 1 : 0);
        u32(a.terms.size());
        for (const auto& t : a.terms) {
            u8(static_cast<std::uint8_t>(t.kind));
            u32(t.index);
        }
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t pos = 0;
    std::uint8_t u8() {
        need(pos < bytes.size(), "truncated relational record");
        return bytes[pos++];
    }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 32; i += 8) {
            v |= static_cast<std::uint32_t>(u8()) << i;
        }
        return v;
    }
    std::uint32_t count(std::uint32_t maximum) {
        const auto v = u32();
        need(v <= maximum, "relational count exceeded");
        return v;
    }
    std::span<const std::uint8_t> blob() {
        const auto n = count(max_wire_bytes);
        need(n <= bytes.size() - pos, "truncated relational blob");
        const auto b = bytes.subspan(pos, n);
        pos += n;
        return b;
    }
    std::string text() {
        const auto b = blob();
        need(b.size() <= 1024, "relational text too long");
        return std::string(b.begin(), b.end());
    }
    Atom atom() {
        Atom a;
        a.predicate = u32();
        const auto flag = u8();
        need(flag <= 1, "invalid atom sign");
        a.negative = flag != 0;
        const auto n = count(4);
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto kind = static_cast<TermKind>(u8());
            need(kind == TermKind::Constant || kind == TermKind::Variable, "invalid term tag");
            a.terms.push_back({kind, u32()});
        }
        return a;
    }
    void end() const { need(pos == bytes.size(), "trailing relational bytes"); }
};
Bytes envelope(std::uint8_t type, const Writer& payload) {
    need(payload.bytes.size() <= max_wire_bytes - 12, "relational payload too large");
    Writer w;
    for (const auto b : Bytes{'A', 'X', 'R', 'M', 2, 0, type, 0}) {
        w.u8(b);
    }
    w.u32(payload.bytes.size());
    for (auto b : payload.bytes) {
        w.u8(b);
    }
    return w.bytes;
}
Reader payload(std::span<const std::uint8_t> bytes, std::uint8_t type) {
    need(bytes.size() <= max_wire_bytes, "relational record too large");
    Reader r{bytes};
    for (auto expected : Bytes{'A', 'X', 'R', 'M', 2, 0, type, 0}) {
        need(r.u8() == expected, "invalid relational envelope");
    }
    const auto n = r.u32();
    need(n == bytes.size() - r.pos, "relational payload length mismatch");
    Reader out{bytes.subspan(r.pos)};
    need(out.u8() == 7, "invalid relational opcode");
    return out;
}
void write_request(Writer& w, const RelationalRequest& q) {
    w.u8(7);
    const auto& m = q.module;
    w.text(m.name);
    w.text(m.version);
    w.u32(m.constants.size());
    for (const auto& c : m.constants) {
        w.text(c);
    }
    w.u32(m.predicates.size());
    for (const auto& p : m.predicates) {
        w.text(p.name);
        w.u32(p.arity);
    }
    w.u32(m.rules.size());
    for (const auto& rule : m.rules) {
        w.u32(rule.variables);
        w.atom(rule.head);
        w.u32(rule.premises.size());
        for (const auto& a : rule.premises) {
            w.atom(a);
        }
        w.text(rule.provenance);
    }
    w.atom(q.goal);
    w.u32(q.limits.events);
    w.u32(q.limits.depth);
    w.u8(q.gpu ? 1 : 0);
}
} // namespace

RelationalModule parse_relational_module(const std::string& source) {
    need(source.size() <= max_text_bytes, "relational source too large");
    RelationalModule m;
    std::istringstream input(source);
    std::string line;
    bool header = false;
    bool clauses = false;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (!header) {
            std::istringstream h(line);
            std::string magic, version, extra;
            need(static_cast<bool>(h >> magic >> version >> m.name >> m.version) &&
                 !(h >> extra) && magic == "axioms" && version == "2", "invalid relational header");
            header = true;
            continue;
        }
        if (line.starts_with("constant ")) {
            need(!clauses, "declarations must precede clauses");
            m.constants.push_back(trim(line.substr(9)));
            need(m.constants.size() <= 64, "too many constants");
        } else if (line.starts_with("predicate ")) {
            need(!clauses, "declarations must precede clauses");
            std::istringstream p(line.substr(10));
            Predicate pred;
            std::string arity, extra;
            need(static_cast<bool>(p >> pred.name >> arity) && !(p >> extra) &&
                 arity.size() == 1 && arity[0] >= '0' && arity[0] <= '4', "invalid predicate declaration");
            pred.arity = static_cast<std::uint32_t>(arity[0] - '0');
            m.predicates.push_back(std::move(pred));
            need(m.predicates.size() <= 64, "too many predicates");
        } else {
            clauses = true;
            const auto bar = line.find('|');
            need(bar != std::string::npos && line.find('|', bar + 1) == std::string::npos,
                 "expected one provenance separator");
            const auto statement = trim(line.substr(0, bar));
            RelationalRule r;
            r.provenance = trim(line.substr(bar + 1));
            std::map<std::string, std::uint32_t> variables;
            if (statement.starts_with("fact ")) {
                r.head = parse_atom(m, statement.substr(5), variables, false);
            } else {
                need(statement.starts_with("rule "), "expected fact or rule");
                const auto arrow = statement.find("->");
                need(arrow != std::string::npos &&
                     statement.find("->", arrow + 2) == std::string::npos, "expected one rule arrow");
                for (const auto& body : split(statement.substr(5, arrow - 5), '&')) {
                    r.premises.push_back(parse_atom(m, body, variables, true));
                }
                r.head = parse_atom(m, statement.substr(arrow + 2), variables, true);
                r.variables = static_cast<std::uint32_t>(variables.size());
            }
            m.rules.push_back(std::move(r));
            need(m.rules.size() <= 512, "too many source rules");
        }
    }
    need(header, "missing relational header");
    validate_module(m);
    // Admission is goal-independent except for its small, fixed maximum size.
    RelationalRequest probe;
    probe.module = m;
    probe.goal.terms.assign(m.predicates.front().arity, Term{});
    static_cast<void>(admit(probe));
    return m;
}
RelationalModule load_relational_module(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    need(static_cast<bool>(input), "cannot open relational module");
    std::string source;
    char c = 0;
    while (input.get(c)) {
        need(source.size() < max_text_bytes, "relational source too large");
        source.push_back(c);
    }
    need(input.eof(), "relational module read failed");
    return parse_relational_module(source);
}
Atom parse_relational_goal(const RelationalModule& module, const std::string& goal) {
    validate_module(module);
    need(goal.size() <= max_text_bytes, "relational goal too large");
    std::map<std::string, std::uint32_t> variables;
    return parse_atom(module, goal, variables, false);
}
RelationalRequest compile_relational(RelationalModule module, const std::string& goal,
                                     Limits limits, bool gpu) {
    RelationalRequest q;
    q.goal = parse_relational_goal(module, goal);
    q.module = std::move(module);
    q.limits = limits;
    q.gpu = gpu;
    static_cast<void>(admit(q));
    return q;
}
Grounding ground_relational(const RelationalRequest& q) {
    const auto counts = admit(q);
    Grounding g;
    auto& out = g.request;
    out.operation = Operation::Prove;
    out.module.name = q.module.name;
    out.module.version = q.module.version;
    out.limits = q.limits;
    for (std::size_t i = 0; i < counts.atoms; ++i) {
        out.module.symbols.push_back("r" + std::to_string(i));
    }
    std::vector<std::size_t> offsets;
    std::size_t base = 0;
    for (const auto& predicate : q.module.predicates) {
        offsets.push_back(base);
        base += power(q.module.constants.size(), predicate.arity);
    }
    out.goal = literal(q.module, q.goal, {}, offsets);
    for (std::uint32_t i = 0; i < q.module.rules.size(); ++i) {
        const auto& source = q.module.rules[i];
        const auto count = power(q.module.constants.size(), source.variables);
        for (std::size_t j = 0; j < count; ++j) {
            auto substitution = tuple_at(j, source.variables, q.module.constants.size());
            Rule r;
            r.head = literal(q.module, source.head, substitution, offsets);
            for (const auto& a : source.premises) {
                r.premises.push_back(literal(q.module, a, substitution, offsets));
            }
            r.provenance = source.provenance;
            out.module.rules.push_back(std::move(r));
            g.instances.push_back({i, std::move(substitution)});
        }
    }
    return g;
}

bool verify_relational_result(const RelationalRequest& q, const RelationalResult& result) {
    try {
        static_cast<void>(admit(q));
        need(q.module == result.request.module && q.goal == result.request.goal &&
             q.limits.events == result.request.limits.events &&
             q.limits.depth == result.request.limits.depth && q.gpu == result.request.gpu,
             "relational request echo mismatch");
        const auto& proof = result.proof;
        const auto& gm = proof.module;
        need(proof.operation == Operation::Prove && !proof.gpu &&
             proof.values.empty() && proof.basis.empty(), "expected CPU v1 proof result");
        need(gm.name == q.module.name && gm.version == q.module.version, "ground metadata mismatch");
        // Independent enumeration: construct the complete atom table as vectors,
        // not the producer's mixed-radix literal computation.
        std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> atoms;
        for (std::uint32_t p = 0; p < q.module.predicates.size(); ++p) {
            std::vector<std::vector<std::uint32_t>> tuples(1);
            for (std::uint32_t k = 0; k < q.module.predicates[p].arity; ++k) {
                std::vector<std::vector<std::uint32_t>> next;
                for (const auto& prefix : tuples) {
                    for (std::uint32_t c = 0; c < q.module.constants.size(); ++c) {
                        auto t = prefix;
                        t.push_back(c);
                        next.push_back(std::move(t));
                    }
                }
                tuples = std::move(next);
            }
            for (auto& t : tuples) {
                atoms.emplace_back(p, std::move(t));
            }
        }
        need(gm.symbols.size() == atoms.size(), "incomplete atom table");
        for (std::size_t i = 0; i < atoms.size(); ++i) {
            need(gm.symbols[i] == "r" + std::to_string(i), "noncanonical ground atom name");
        }
        const auto check_literal = [&](const Atom& a, const std::vector<std::uint32_t>& sub,
                                       std::uint32_t actual) {
            need(actual / 2 < atoms.size() && (actual % 2 != 0) == a.negative,
                 "ground literal sign or index mismatch");
            const auto& entry = atoms[actual / 2];
            need(entry.first == a.predicate && entry.second.size() == a.terms.size(),
                 "ground predicate mismatch");
            for (std::size_t k = 0; k < a.terms.size(); ++k) {
                const auto& t = a.terms[k];
                const auto c = t.kind == TermKind::Constant ? t.index : sub.at(t.index);
                need(entry.second[k] == c, "ground argument mismatch");
            }
        };
        check_literal(q.goal, {}, proof.goal);
        need(result.instances.size() == gm.rules.size(), "instance/rule count mismatch");
        std::size_t cursor = 0;
        for (std::uint32_t source = 0; source < q.module.rules.size(); ++source) {
            const auto& r = q.module.rules[source];
            std::vector<std::uint32_t> expected(r.variables, 0);
            bool more = true;
            while (more) {
                need(cursor < result.instances.size(), "missing grounding instance");
                const auto& inst = result.instances[cursor];
                need(inst.source_rule == source && inst.substitution == expected,
                     "incomplete or unordered grounding substitutions");
                const auto& actual = gm.rules[cursor];
                need(actual.provenance == r.provenance &&
                     actual.premises.size() == r.premises.size(), "ground rule shape mismatch");
                check_literal(r.head, expected, actual.head);
                for (std::size_t j = 0; j < r.premises.size(); ++j) {
                    check_literal(r.premises[j], expected, actual.premises[j]);
                }
                ++cursor;
                more = false;
                for (std::size_t j = expected.size(); j > 0; --j) {
                    if (++expected[j - 1] < q.module.constants.size()) {
                        more = true;
                        break;
                    }
                    expected[j - 1] = 0;
                }
            }
        }
        need(cursor == result.instances.size(), "extra grounding instance");
        need(proof.history.size() <= q.limits.events, "history exceeds request budget");
        for (const auto& e : proof.history) {
            need(e.depth <= q.limits.depth + 1, "history exceeds request depth");
        }
        // Both v1 codec directions validate replay, roots, status and independent
        // closure against the now-certified COMPLETE ground module.
        static_cast<void>(decode_result(encode_result(proof)));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
Bytes encode_relational_request(const RelationalRequest& q) {
    static_cast<void>(admit(q));
    Writer w;
    write_request(w, q);
    return envelope(1, w);
}
RelationalRequest decode_relational_request(std::span<const std::uint8_t> bytes) {
    auto r = payload(bytes, 1);
    RelationalRequest q;
    auto& m = q.module;
    m.name = r.text();
    m.version = r.text();
    const auto constants = r.count(64);
    for (std::uint32_t i = 0; i < constants; ++i) {
        m.constants.push_back(r.text());
    }
    const auto predicates = r.count(64);
    for (std::uint32_t i = 0; i < predicates; ++i) {
        auto name = r.text();
        m.predicates.push_back({std::move(name), r.u32()});
    }
    const auto rules = r.count(512);
    for (std::uint32_t i = 0; i < rules; ++i) {
        RelationalRule rule;
        rule.variables = r.count(8);
        rule.head = r.atom();
        const auto premises = r.count(16);
        for (std::uint32_t j = 0; j < premises; ++j) {
            rule.premises.push_back(r.atom());
        }
        rule.provenance = r.text();
        m.rules.push_back(std::move(rule));
    }
    q.goal = r.atom();
    q.limits.events = r.u32();
    q.limits.depth = r.u32();
    const auto gpu = r.u8();
    need(gpu <= 1, "invalid relational GPU flag");
    q.gpu = gpu != 0;
    r.end();
    static_cast<void>(admit(q));
    return q;
}
Bytes encode_relational_result(const RelationalResult& result) {
    need(verify_relational_result(result.request, result), "invalid relational certificate");
    Writer w;
    w.u8(7);
    w.blob(encode_relational_request(result.request));
    w.u32(result.instances.size());
    for (const auto& inst : result.instances) {
        w.u32(inst.source_rule);
        w.u32(inst.substitution.size());
        for (auto c : inst.substitution) {
            w.u32(c);
        }
    }
    w.blob(encode_result(result.proof));
    return envelope(2, w);
}
RelationalResult decode_relational_result(std::span<const std::uint8_t> bytes) {
    auto r = payload(bytes, 2);
    RelationalResult result;
    result.request = decode_relational_request(r.blob());
    const auto n = r.count(512);
    for (std::uint32_t i = 0; i < n; ++i) {
        GroundInstance inst;
        inst.source_rule = r.u32();
        const auto k = r.count(8);
        for (std::uint32_t j = 0; j < k; ++j) {
            inst.substitution.push_back(r.u32());
        }
        result.instances.push_back(std::move(inst));
    }
    result.proof = decode_result(r.blob());
    need(result.proof.operation == Operation::Prove, "nested record must be v1 Prove");
    r.end();
    need(verify_relational_result(result.request, result), "invalid relational grounding certificate");
    return result;
}
RelationalResult execute_relational(const RelationalRequest& request) {
    auto grounding = ground_relational(request);
    RelationalResult result{request, std::move(grounding.instances), execute(grounding.request)};
    need(verify_relational_result(request, result), "relational execution verification failed");
    return result;
}
std::string render_relational(const RelationalResult& result) {
    need(verify_relational_result(result.request, result), "invalid relational result for rendering");
    std::ostringstream out;
    const auto& m = result.request.module;
    const auto atom_name = [&](const Atom& a) {
        std::string s = (a.negative ? "!" : "") + m.predicates[a.predicate].name + "(";
        for (std::size_t i = 0; i < a.terms.size(); ++i) {
            s += (i ? "," : "") + m.constants[a.terms[i].index];
        }
        return s + ")";
    };
    out << "relational goal: " << atom_name(result.request.goal) << '\n';
    out << "complete grounding: " << result.instances.size() << " instances\n";
    // Show the canonical dictionary in relational notation as well as the v1
    // replay so every proof literal can be interpreted without source parsing.
    std::size_t ordinal = 0;
    for (std::uint32_t p = 0; p < m.predicates.size(); ++p) {
        const auto count = power(m.constants.size(), m.predicates[p].arity);
        for (std::size_t i = 0; i < count; ++i) {
            Atom a{p, false, {}};
            for (auto c : tuple_at(i, m.predicates[p].arity, m.constants.size())) {
                a.terms.push_back({TermKind::Constant, c});
            }
            out << "r" << ordinal++ << " = " << atom_name(a) << '\n';
        }
    }
    for (std::size_t i = 0; i < result.instances.size(); ++i) {
        const auto& inst = result.instances[i];
        out << "ground rule " << i << ": source " << inst.source_rule << " substitution [";
        for (std::size_t j = 0; j < inst.substitution.size(); ++j) {
            out << (j ? "," : "") << m.constants[inst.substitution[j]];
        }
        out << "]\n";
    }
    out << render(result.proof);
    return out.str();
}
Bytes run_relational_binary(std::span<const std::uint8_t> request) {
    return encode_relational_result(execute_relational(decode_relational_request(request)));
}
} // namespace axiom
