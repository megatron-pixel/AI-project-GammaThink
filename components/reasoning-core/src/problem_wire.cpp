#include "problem_wire.hpp"

#include <bit>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace axiom {
namespace {
void need(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class A, class T> void transfer(A& archive, T& value);

struct Writer {
    static constexpr bool reading = false;
    Bytes bytes;
    void byte(std::uint8_t v) {
        need(bytes.size() < max_wire_bytes, "Problem wire byte limit exceeded");
        bytes.push_back(v);
    }
    template<class T> void integer(const T& v) {
        for (unsigned i = 0; i < sizeof(T); ++i)
            byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    void boolean(bool v) { byte(v ? 1 : 0); }
    void count(std::size_t n, std::size_t maximum) {
        need(n <= maximum, "Problem wire count limit exceeded");
        integer(static_cast<std::uint32_t>(n));
    }
    void blob(std::span<const std::uint8_t> b) {
        need(bytes.size() <= max_wire_bytes - 4
            && b.size() <= max_wire_bytes - 4 - bytes.size(), "Problem wire blob limit exceeded");
        count(b.size(), max_wire_bytes);
        bytes.insert(bytes.end(), b.begin(), b.end());
    }
    void text(const std::string& s, std::size_t maximum = 1024) {
        need(s.size() <= maximum, "Problem wire text limit exceeded");
        blob({reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
    }
    void rational(const Rational& q) {
        need(q.get_den() > 0, "Invalid problem rational denominator");
        need(mpz_sizeinbase(q.get_num_mpz_t(), 2) <= 4096
            && mpz_sizeinbase(q.get_den_mpz_t(), 2) <= 4096, "Problem rational bit limit exceeded");
        const auto canonical = checked_problem_rational(q);
        need(canonical.get_num() == q.get_num() && canonical.get_den() == q.get_den(),
             "Noncanonical problem rational");
        byte(q < 0 ? 1 : 0);
        for (const auto& z : {q.get_num(), q.get_den()}) {
            const mpz_class absolute = z < 0 ? -z : z;
            std::uint8_t buffer[512]{};
            std::size_t n = 0;
            mpz_export(buffer, &n, -1, 1, 0, 0, absolute.get_mpz_t());
            blob({buffer, n});
        }
    }
    template<class T> void enumeration(const T& v, unsigned low, unsigned high) {
        const auto tag = static_cast<unsigned>(v);
        need(tag >= low && tag <= high, "Unknown problem wire enum");
        byte(static_cast<std::uint8_t>(tag));
    }
    template<class V> void list(const V& values, std::size_t maximum) {
        count(values.size(), maximum);
        for (const auto& value : values) transfer(*this, value);
    }
    template<class... T> void operator()(const T&... v) { (transfer(*this, v), ...); }
};

struct Reader {
    static constexpr bool reading = true;
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
    std::uint8_t byte() {
        need(position < bytes.size(), "Truncated problem wire record");
        return bytes[position++];
    }
    template<class T> void integer(T& v) {
        v = 0;
        for (unsigned i = 0; i < sizeof(T); ++i)
            v |= static_cast<T>(byte()) << (8 * i);
    }
    void boolean(bool& v) {
        const auto b = byte();
        need(b <= 1, "Invalid problem wire bool");
        v = b != 0;
    }
    std::uint32_t count(std::size_t maximum) {
        std::uint32_t n = 0;
        integer(n);
        need(n <= maximum, "Problem wire count limit exceeded");
        return n;
    }
    std::span<const std::uint8_t> blob(std::size_t maximum = max_wire_bytes) {
        const auto n = count(maximum);
        need(n <= bytes.size() - position, "Truncated problem wire blob");
        const auto b = bytes.subspan(position, n);
        position += n;
        return b;
    }
    void text(std::string& s, std::size_t maximum = 1024) {
        const auto b = blob(maximum);
        s.assign(b.begin(), b.end());
    }
    mpz_class magnitude() {
        const auto b = blob(512);
        need(b.empty() || b.back() != 0, "Noncanonical problem integer");
        mpz_class z;
        mpz_import(z.get_mpz_t(), b.size(), -1, 1, 0, 0, b.data());
        return z;
    }
    void rational(Rational& q) {
        bool negative = false;
        boolean(negative);
        auto numerator = magnitude();
        const auto denominator = magnitude();
        need(denominator > 0 && !(negative && numerator == 0), "Invalid problem rational");
        if (negative) numerator = -numerator;
        q = Rational(numerator, denominator);
        q.canonicalize();
        need(q.get_num() == numerator && q.get_den() == denominator, "Noncanonical problem rational");
    }
    template<class T> void enumeration(T& v, unsigned low, unsigned high) {
        const auto tag = byte();
        need(tag >= low && tag <= high, "Unknown problem wire enum");
        v = static_cast<T>(tag);
    }
    template<class V> void list(V& values, std::size_t maximum) {
        const auto n = count(maximum);
        // Every element in this schema consumes at least one byte. Do not
        // allocate a declared vector until its minimum representation fits.
        need(n <= bytes.size() - position, "Truncated problem wire list");
        for (std::uint32_t i = 0; i < n; ++i) {
            typename V::value_type value{};
            transfer(*this, value);
            values.push_back(std::move(value));
        }
    }
    template<class... T> void operator()(T&... v) { (transfer(*this, v), ...); }
    void end() const { need(position == bytes.size(), "Trailing problem wire bytes"); }
};

template<std::size_t I = 0, class V> void select_variant(V& v, std::uint8_t tag) {
    if constexpr (I < std::variant_size_v<V>) {
        if (tag == I + 1) v.template emplace<I>();
        else select_variant<I + 1>(v, tag);
    } else {
        throw std::runtime_error("Unknown problem variant tag");
    }
}

// One field traversal keeps the encoder and decoder schemas identical.
template<class A, class V> void transfer(A& a, V& v) {
    using T = std::remove_const_t<V>;
    if constexpr (std::is_same_v<T, bool>) a.boolean(v);
    else if constexpr (std::is_same_v<T, std::uint32_t> || std::is_same_v<T, std::uint64_t>)
        a.integer(v);
    else if constexpr (std::is_same_v<T, std::int32_t>) {
        if constexpr (A::reading) {
            std::uint32_t bits = 0;
            a.integer(bits);
            v = std::bit_cast<std::int32_t>(bits);
        } else a.integer(std::bit_cast<std::uint32_t>(v));
    } else if constexpr (std::is_same_v<T, std::string>) a.text(v);
    else if constexpr (std::is_same_v<T, Rational>) a.rational(v);
    else if constexpr (std::is_same_v<T, PremiseBasis> || std::is_same_v<T, LocatorRole>)
        a.enumeration(v, 1, 3);
    else if constexpr (std::is_same_v<T, QuantityPrecision> || std::is_same_v<T, ProblemPremiseKind>)
        a.enumeration(v, 1, 4);
    else if constexpr (std::is_same_v<T, ProblemStatus>) a.enumeration(v, 1, 5);
    else if constexpr (std::is_same_v<T, ProblemIssue>) a.enumeration(v, 0, 6);
    else if constexpr (std::is_same_v<T, TermKind>) a.enumeration(v, 1, 2);
    else if constexpr (std::is_same_v<T, Code>) a.enumeration(v, 3, 6);
    else if constexpr (requires { v.has_value(); }) {
        bool present = v.has_value();
        a(present);
        if constexpr (A::reading) { if (present) v.emplace(); else v.reset(); }
        if (present) a(*v);
    } else if constexpr (requires { v.index(); }) {
        if constexpr (A::reading) select_variant(v, a.byte());
        else a.byte(static_cast<std::uint8_t>(v.index() + 1));
        std::visit([&](auto& alternative) { a(alternative); }, v);
    } else if constexpr (std::is_same_v<T, EvidenceSpan>) a(v.view, v.offset, v.length, v.locator);
    else if constexpr (std::is_same_v<T, EvidenceLocator>) {
        a(v.role, v.offset, v.length, v.table, v.row, v.column);
        a.list(v.headers, 16);
    } else if constexpr (std::is_same_v<T, EvidenceView>) {
        a.text(v.id, 128); a(v.basis, v.archive_id, v.entry_path);
        a.text(v.extractor_version, 128); a.text(v.digest, 64); a.text(v.text, 65536);
        a.list(v.locators, 256);
    } else if constexpr (std::is_same_v<T, ProblemOrigin>) {
        a(v.basis, v.label); a.list(v.supports, 16);
    } else if constexpr (std::is_same_v<T, ProblemScope>) {
        a(v.resolved); a.list(v.qualifiers, 16);
    } else if constexpr (std::is_same_v<T, std::pair<std::string, std::string>>) {
        a.text(v.first, 128); a.text(v.second, 128);
    } else if constexpr (std::is_same_v<T, ProblemUnit>) {
        a.text(v.id, 128); a.text(v.symbol, 128);
        for (auto& exponent : v.dimension) a(exponent);
        a(v.scale); a.list(v.spellings, 16);
    } else if constexpr (std::is_same_v<T, ProblemNumber>)
        a(v.numeral, v.unit_span, v.unit, v.precision, v.qualifier);
    else if constexpr (std::is_same_v<T, ProblemQuantity>)
        a(v.property, v.entity, v.scope, v.unit);
    else if constexpr (std::is_same_v<T, ProblemBinding>) a(v.quantity, v.number, v.origin);
    else if constexpr (std::is_same_v<T, ProblemQuantityNode>) a(v.quantity);
    else if constexpr (std::is_same_v<T, ProblemNumberNode>) a(v.number);
    else if constexpr (std::is_same_v<T, ProblemNegateNode>) a(v.operand);
    else if constexpr (std::is_same_v<T, ProblemBinaryNode>) a(v.operation, v.left, v.right);
    else if constexpr (std::is_same_v<T, ProblemEquation>) a(v.left, v.right, v.scope, v.guard, v.origin);
    else if constexpr (std::is_same_v<T, ProblemQuantityGoal>) a(v.quantity, v.unit);
    else if constexpr (std::is_same_v<T, ProblemAtomGoal>) a(v.atom);
    else if constexpr (std::is_same_v<T, Term>) a(v.kind, v.index);
    else if constexpr (std::is_same_v<T, Atom>) {
        a(v.predicate, v.negative); a.list(v.terms, 4);
    } else if constexpr (std::is_same_v<T, Predicate>) { a.text(v.name, 128); a(v.arity); }
    else if constexpr (std::is_same_v<T, RelationalRule>) {
        a(v.variables, v.head); a.list(v.premises, 16); a(v.provenance);
    } else if constexpr (std::is_same_v<T, RelationalModule>) {
        a.text(v.name, 128); a.text(v.version, 128);
        a.list(v.constants, 64); a.list(v.predicates, 64); a.list(v.rules, 512);
    } else if constexpr (std::is_same_v<T, ProblemContext>) {
        a.text(v.unit_registry_id, 128); a.text(v.unit_registry_version, 128);
        a.list(v.units, 64); a.list(v.views, 16);
    } else if constexpr (std::is_same_v<T, ProblemProposal>) {
        a.list(v.entities, 64); a.list(v.quantities, 64); a.list(v.numbers, 128);
        a.list(v.bindings, 128); a.list(v.nodes, 256); a.list(v.equations, 64);
        a(v.logic); a.list(v.logic_origins, 512); a(v.goal);
    } else if constexpr (std::is_same_v<T, ProblemLimits>) a(v.work, v.steps);
    else if constexpr (std::is_same_v<T, ProblemRequest>) a(v.context, v.proposal, v.limits);
    else if constexpr (std::is_same_v<T, ProblemBindingStep>) a(v.binding, v.lexical_value, v.unit_scale);
    else if constexpr (std::is_same_v<T, ProblemEquationStep>) {
        a(v.equation); a.list(v.dependencies, 256); a(v.guard);
    } else if constexpr (std::is_same_v<T, ProblemStep>) a(v.quantity, v.canonical_value, v.justification);
    else if constexpr (std::is_same_v<T, ProblemGuardProof>) {
        a(v.goal);
        if constexpr (A::reading) v.certificate = decode_relational_result(a.blob());
        else a.blob(encode_relational_result(v.certificate));
    } else if constexpr (std::is_same_v<T, ProblemValueConflict>) a(v.first, v.second);
    else if constexpr (std::is_same_v<T, ProblemEquationConflict>) {
        a(v.equation); a.list(v.dependencies, 256); a(v.guard);
    } else if constexpr (std::is_same_v<T, ProblemGuardConflict>) a(v.guard);
    else if constexpr (std::is_same_v<T, ProblemPendingEquation>) a(v.equation, v.issue);
    else if constexpr (std::is_same_v<T, ProblemPremiseRef>) a(v.kind, v.index);
    else if constexpr (std::is_same_v<T, ProblemResult>) {
        a(v.request, v.status, v.issue);
        a.list(v.steps, 256); a.list(v.guards, 65);
        a(v.conflict, v.goal_step, v.logical_goal_guard, v.answer, v.answer_unit);
        a.list(v.pending_equations, 64); a.list(v.used, 832);
        a(v.formal_assumptions, v.question_assumptions, v.source_interpretations, v.work);
    } else static_assert(!sizeof(T), "Missing problem wire field traversal");
}

Bytes envelope(std::uint8_t kind, const Writer& body) {
    need(body.bytes.size() <= max_wire_bytes - 12, "Problem wire envelope limit exceeded");
    Writer w;
    for (auto b : Bytes{'A','X','R','M',3,0,kind,0}) w.byte(b);
    w.integer(static_cast<std::uint32_t>(body.bytes.size()));
    w.bytes.insert(w.bytes.end(), body.bytes.begin(), body.bytes.end());
    return w.bytes;
}
Reader payload(std::span<const std::uint8_t> bytes, std::uint8_t kind) {
    need(bytes.size() <= max_wire_bytes, "Problem wire envelope limit exceeded");
    Reader r{bytes};
    for (auto b : Bytes{'A','X','R','M',3,0,kind,0})
        need(r.byte() == b, "Invalid AXRM v3 envelope");
    const auto n = r.count(max_wire_bytes - 12);
    need(n == bytes.size() - r.position, "Problem wire payload length mismatch");
    return Reader{bytes.subspan(r.position)};
}
} // namespace

Bytes encode_problem_request(const ProblemRequest& request) {
    static_cast<void>(admit_problem(request));
    Writer w;
    w(request);
    return envelope(1, w);
}
ProblemRequest decode_problem_request(std::span<const std::uint8_t> bytes) {
    auto r = payload(bytes, 1);
    ProblemRequest request;
    r(request);
    r.end();
    static_cast<void>(admit_problem(request));
    return request;
}
Bytes encode_problem_result(const ProblemResult& result) {
    // Traverse bounded fields before invoking the independent checker.
    Writer w;
    w(result);
    need(verify_problem_result(result.request, result), "Invalid problem certificate");
    return envelope(2, w);
}
ProblemResult decode_problem_result(std::span<const std::uint8_t> bytes) {
    auto r = payload(bytes, 2);
    ProblemResult result;
    r(result);
    r.end();
    need(verify_problem_result(result.request, result), "Invalid problem certificate");
    return result;
}
std::string render_problem(const ProblemResult& result) {
    need(verify_problem_result(result.request, result), "Invalid problem certificate for rendering");
    std::ostringstream out;
    out << "Conditional consequence of admitted premises; source truth and source/question "
           "interpretations are not verified.\n";
    out << "Assumptions used: formal=" << result.formal_assumptions
        << " question=" << result.question_assumptions << " source=" << result.source_interpretations << '\n';
    out << "status: ";
    switch (result.status) {
    case ProblemStatus::Checked: out << "checked"; break;
    case ProblemStatus::Unresolved: out << "unresolved"; break;
    case ProblemStatus::Conflict: out << "conflict"; break;
    case ProblemStatus::Unsupported: out << "unsupported"; break;
    case ProblemStatus::Limited: out << "limited"; break;
    }
    if (result.logical_goal_guard) {
        const auto status = result.guards.at(*result.logical_goal_guard).certificate.proof.status;
        if (status == Status::Disproved) out << " (logical goal disproved)";
        else if (status == Status::Proved) out << " (logical goal proved)";
        else if (status == Status::Contradiction) out << " (logical conflict)";
        else out << " (logical goal unresolved)";
    }
    out << '\n';
    if (result.answer) {
        const auto& unit = result.request.context.units.at(result.answer_unit);
        out << "exact answer: " << result.answer->get_str() << ' ' << unit.symbol
            << " [unit " << unit.id << ", scale " << unit.scale.get_str() << ", SI dimensions";
        for (const auto exponent : unit.dimension) out << ' ' << exponent;
        out << "]\n";
    }
    out << "issue: " << static_cast<unsigned>(result.issue)
        << "; pending equations: " << result.pending_equations.size()
        << "; work: " << result.work << '\n';
    return out.str();
}
Bytes run_problem_binary(std::span<const std::uint8_t> request) {
    const auto decoded = decode_problem_request(request);
    const auto result = execute_problem(decoded);
    need(verify_problem_result(decoded, result), "Problem execution verification failed");
    return encode_problem_result(result);
}
} // namespace axiom
