#include "problem_goals.hpp"
#include "problem_wire.hpp"

#include <algorithm>
#include <stdexcept>

namespace axiom {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct Writer {
    Bytes bytes;
    void byte(std::uint8_t value) {
        require(bytes.size() < max_wire_bytes - 12, "Ordered goal wire bound exceeded");
        bytes.push_back(value);
    }
    void integer(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) byte(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    void blob(std::span<const std::uint8_t> value) {
        require(value.size() <= max_wire_bytes - 16
            && bytes.size() <= max_wire_bytes - 16 - value.size(), "Ordered goal blob bound exceeded");
        integer(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    Bytes finish(std::uint8_t kind) {
        Bytes envelope{'A', 'X', 'G', 'L', 1, kind, 0, 0};
        const auto size = static_cast<std::uint32_t>(bytes.size());
        for (unsigned i = 0; i < 4; ++i)
            envelope.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
        envelope.insert(envelope.end(), bytes.begin(), bytes.end());
        return envelope;
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
    std::uint8_t byte() {
        require(position < bytes.size(), "Truncated ordered goal wire");
        return bytes[position++];
    }
    std::uint32_t integer() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(byte()) << (8 * i);
        return value;
    }
    std::uint32_t count(std::size_t maximum) {
        const auto value = integer();
        require(value <= maximum, "Ordered goal count bound exceeded");
        return value;
    }
    std::span<const std::uint8_t> blob() {
        const auto size = count(max_wire_bytes);
        require(size <= bytes.size() - position, "Truncated ordered goal blob");
        const auto value = bytes.subspan(position, size);
        position += size;
        return value;
    }
    void end() const { require(position == bytes.size(), "Trailing ordered goal bytes"); }
};
Reader open(std::span<const std::uint8_t> bytes, std::uint8_t kind) {
    require(bytes.size() >= 12 && bytes.size() <= max_wire_bytes, "Invalid ordered goal wire size");
    const Bytes header{'A', 'X', 'G', 'L', 1, kind, 0, 0};
    require(std::equal(header.begin(), header.end(), bytes.begin()), "Unknown ordered goal envelope");
    Reader envelope{bytes, 8};
    require(envelope.integer() == bytes.size() - 12, "Ordered goal payload length mismatch");
    return {bytes.subspan(12)};
}
void write_goal(Writer& writer, const ProblemGoal& goal) {
    if (const auto* quantity = std::get_if<ProblemQuantityGoal>(&goal)) {
        writer.byte(1);
        writer.integer(quantity->quantity);
        writer.integer(quantity->unit);
    } else {
        writer.byte(2);
        const auto& atom = std::get<ProblemAtomGoal>(goal).atom;
        require(atom.terms.size() <= 16, "Ordered atom arity bound exceeded");
        writer.integer(atom.predicate);
        writer.byte(atom.negative ? 1 : 0);
        writer.integer(static_cast<std::uint32_t>(atom.terms.size()));
        for (const auto& term : atom.terms) {
            require(term.kind == TermKind::Constant, "Ordered goal must be ground");
            writer.integer(term.index);
        }
    }
}
ProblemGoal read_goal(Reader& reader) {
    const auto kind = reader.byte();
    if (kind == 1) return ProblemQuantityGoal{reader.integer(), reader.integer()};
    require(kind == 2, "Unknown ordered goal kind");
    Atom atom;
    atom.predicate = reader.integer();
    const auto sign = reader.byte();
    require(sign <= 1, "Invalid ordered goal polarity");
    atom.negative = sign != 0;
    const auto count = reader.count(16);
    for (std::uint32_t i = 0; i < count; ++i)
        atom.terms.push_back({TermKind::Constant, reader.integer()});
    return ProblemAtomGoal{std::move(atom)};
}
}

Bytes encode_problem_goals_request(const ProblemGoalsRequest& request) {
    require(!request.goals.empty() && request.goals.size() <= max_problem_goals,
        "Invalid ordered goal count");
    Writer writer;
    writer.blob(encode_problem_request(request.first));
    writer.integer(static_cast<std::uint32_t>(request.goals.size()));
    for (const auto& slot : request.goals) {
        write_goal(writer, slot.goal);
        require(!slot.supports.empty() && slot.supports.size() <= 16, "Invalid goal support count");
        writer.integer(static_cast<std::uint32_t>(slot.supports.size()));
        for (const auto& span : slot.supports) {
            writer.integer(span.view); writer.integer(span.offset);
            writer.integer(span.length); writer.integer(span.locator);
        }
    }
    return writer.finish(1);
}

ProblemGoalsRequest decode_problem_goals_request(std::span<const std::uint8_t> bytes) {
    auto reader = open(bytes, 1);
    ProblemGoalsRequest request;
    request.first = decode_problem_request(reader.blob());
    const auto count = reader.count(max_problem_goals);
    require(count > 0, "Empty ordered goal request");
    for (std::uint32_t i = 0; i < count; ++i) {
        SupportedProblemGoal slot{read_goal(reader), {}};
        const auto supports = reader.count(16);
        require(supports > 0, "Missing ordered goal supports");
        for (std::uint32_t j = 0; j < supports; ++j)
            slot.supports.push_back({reader.integer(), reader.integer(), reader.integer(), reader.integer()});
        request.goals.push_back(std::move(slot));
    }
    reader.end();
    return request;
}

Bytes encode_problem_goals_result(const ProblemGoalsResult& result) {
    require(result.request_digest.size() == 64 && !result.slots.empty()
        && result.slots.size() <= max_problem_goals, "Invalid ordered goal result");
    Writer writer;
    writer.blob({reinterpret_cast<const std::uint8_t*>(result.request_digest.data()), 64});
    writer.integer(static_cast<std::uint32_t>(result.slots.size()));
    for (const auto& slot : result.slots) {
        writer.integer(slot.ordinal);
        writer.blob(encode_problem_result(slot.result));
    }
    return writer.finish(2);
}

ProblemGoalsResult decode_problem_goals_result(std::span<const std::uint8_t> bytes) {
    auto reader = open(bytes, 2);
    const auto digest = reader.blob();
    require(digest.size() == 64, "Invalid ordered result digest");
    ProblemGoalsResult result{std::string(digest.begin(), digest.end()), {}};
    const auto count = reader.count(max_problem_goals);
    require(count > 0, "Empty ordered result");
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto ordinal = reader.integer();
        result.slots.push_back({ordinal, decode_problem_result(reader.blob())});
    }
    reader.end();
    return result;
}
} // namespace axiom
