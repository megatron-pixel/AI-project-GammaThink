#include "problem.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace axiom {
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
std::string trim(std::string_view value) {
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    while (!value.empty() && space(value.back())) value.remove_suffix(1);
    return std::string(value);
}
bool identifier(const std::string& value) {
    if (value.empty() || value.size() > 128) return false;
    const auto letter = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!letter(static_cast<unsigned char>(value.front()))) return false;
    return std::all_of(value.begin(), value.end(), [&](unsigned char c) {
        return letter(c) || (c >= '0' && c <= '9') || c == '.';
    });
}
bool utf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first == 0) return false;
        if (first < 128) continue;
        unsigned count = 0;
        std::uint32_t value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31; minimum = 128; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15; minimum = 2048; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 65536; }
        else return false;
        if (count > text.size() - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            if ((byte & 0xc0) != 0x80) return false;
            value = (value << 6) | (byte & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
bool boundary(const std::string& text, std::size_t offset) {
    return offset == text.size() || (static_cast<unsigned char>(text[offset]) & 0xc0) != 0x80;
}
bool basis(PremiseBasis value) {
    return value == PremiseBasis::Formal || value == PremiseBasis::Question || value == PremiseBasis::Source;
}
bool dimensionless(const ProblemDimension& value) {
    return std::all_of(value.begin(), value.end(), [](auto power) { return power == 0; });
}
ProblemDimension combined(ProblemDimension left, const ProblemDimension& right, bool divide) {
    for (std::size_t i = 0; i < left.size(); ++i) {
        left[i] += divide ? -right[i] : right[i];
        require(left[i] >= -64 && left[i] <= 64, "Problem dimension bound exceeded");
    }
    return left;
}
std::string digits(std::string_view text, bool fractional) {
    const auto stripped = trim(text);
    std::vector<std::string> groups;
    std::size_t cursor = 0;
    while (cursor < stripped.size()) {
        const auto start = cursor;
        while (cursor < stripped.size() && stripped[cursor] >= '0' && stripped[cursor] <= '9') ++cursor;
        require(cursor > start, "Invalid numeral digits");
        groups.push_back(stripped.substr(start, cursor - start));
        if (cursor == stripped.size()) break;
        require(space(stripped[cursor]), "Invalid numeral character");
        while (cursor < stripped.size() && space(stripped[cursor])) ++cursor;
    }
    require(!groups.empty(), "Empty numeral digits");
    if (groups.size() > 1) {
        require(fractional ? groups.front().size() == 3 : groups.front().size() <= 3,
                "Invalid digit grouping");
        for (std::size_t i = 1; i < groups.size(); ++i) {
            require(groups[i].size() == 3 || (fractional && i + 1 == groups.size()
                && groups[i].size() <= 3), "Invalid digit grouping");
        }
    }
    std::string result;
    for (const auto& group : groups) result += group;
    require(result.size() <= 1233, "Numeral digit bound exceeded");
    return result;
}
std::string numeric_glyphs(std::string_view raw) {
    require(raw.size() <= 4096, "Numeral span bound exceeded");
    const std::string original(raw);
    require(utf8(original), "Invalid numeral UTF-8");
    const std::array<std::pair<std::string_view, char>, 12> superscripts{{
        {"\xc2\xb9", '1'}, {"\xc2\xb2", '2'}, {"\xc2\xb3", '3'},
        {"\xe2\x81\xb0", '0'}, {"\xe2\x81\xb4", '4'}, {"\xe2\x81\xb5", '5'},
        {"\xe2\x81\xb6", '6'}, {"\xe2\x81\xb7", '7'}, {"\xe2\x81\xb8", '8'},
        {"\xe2\x81\xb9", '9'}, {"\xe2\x81\xba", '+'}, {"\xe2\x81\xbb", '-'}
    }};
    std::string result;
    bool exponent_run = false;
    for (std::size_t i = 0; i < raw.size();) {
        const auto tail = raw.substr(i);
        bool matched = false;
        for (const auto& [glyph, replacement] : superscripts) {
            if (tail.starts_with(glyph)) {
                if (!exponent_run) result += '^';
                result += replacement;
                exponent_run = true;
                i += glyph.size();
                matched = true;
                break;
            }
        }
        if (matched) continue;
        exponent_run = false;
        if (tail.starts_with("\xc3\x97")) { result += '*'; i += 2; }
        else if (tail.starts_with("\xe2\x88\x92")) { result += '-'; i += 3; }
        else if (tail.starts_with("\xc2\xa0")) { result += ' '; i += 2; }
        else if (tail.starts_with("\xe2\x80\x89") || tail.starts_with("\xe2\x80\xaf")) {
            result += ' '; i += 3;
        } else {
            require(static_cast<unsigned char>(raw[i]) < 128, "Unsupported numeral glyph");
            result += raw[i++];
        }
    }
    return trim(result);
}
}

Rational checked_problem_rational(Rational value) {
    require(value.get_den() != 0, "Zero rational denominator");
    value.canonicalize();
    require(value.get_den() > 0 && mpz_sizeinbase(value.get_num_mpz_t(), 2) <= 4096
        && mpz_sizeinbase(value.get_den_mpz_t(), 2) <= 4096, "Problem rational bound exceeded");
    return value;
}

Rational parse_evidence_number(std::string_view numeral) {
    std::string value = numeric_glyphs(numeral);
    require(!value.empty(), "Empty numeral");
    std::string mantissa = value, exponent;
    const auto e = value.find_first_of("eE");
    const auto times = value.find_first_of("*x");
    require(e == std::string::npos || times == std::string::npos, "Multiple scientific notation markers");
    if (e != std::string::npos) {
        mantissa = trim(std::string_view(value).substr(0, e));
        exponent = trim(std::string_view(value).substr(e + 1));
        require(!exponent.empty(), "Missing scientific exponent");
    } else if (times != std::string::npos) {
        mantissa = trim(std::string_view(value).substr(0, times));
        auto tail = trim(std::string_view(value).substr(times + 1));
        require(tail.starts_with("10") && tail.size() > 2
            && (space(tail[2]) || tail[2] == '^'), "Invalid scientific notation base");
        tail = trim(std::string_view(tail).substr(2));
        if (tail.starts_with('^')) tail = trim(std::string_view(tail).substr(1));
        require(!tail.empty(), "Missing scientific exponent");
        exponent = tail;
    }
    bool negative = false;
    if (!mantissa.empty() && (mantissa.front() == '+' || mantissa.front() == '-')) {
        negative = mantissa.front() == '-';
        mantissa = trim(std::string_view(mantissa).substr(1));
    }
    const auto dot = mantissa.find('.');
    require(dot == std::string::npos || mantissa.find('.', dot + 1) == std::string::npos,
            "Multiple decimal points");
    std::string integer_part, fraction_part;
    if (dot == std::string::npos) integer_part = digits(mantissa, false);
    else {
        const auto left = trim(std::string_view(mantissa).substr(0, dot));
        const auto right = trim(std::string_view(mantissa).substr(dot + 1));
        require(!left.empty() || !right.empty(), "Empty decimal");
        integer_part = left.empty() ? "0" : digits(left, false);
        if (!right.empty()) fraction_part = digits(right, true);
    }
    require(integer_part.size() + fraction_part.size() <= 1233, "Numeral digit bound exceeded");
    mpz_class numerator(integer_part + fraction_part, 10), denominator = 1;
    mpz_ui_pow_ui(denominator.get_mpz_t(), 10, fraction_part.size());
    if (negative) numerator = -numerator;
    if (!exponent.empty()) {
        bool exponent_negative = false;
        if (exponent.front() == '+' || exponent.front() == '-') {
            exponent_negative = exponent.front() == '-';
            exponent.erase(0, 1);
        }
        require(!exponent.empty() && exponent.size() <= 5
            && std::all_of(exponent.begin(), exponent.end(), [](char c) { return c >= '0' && c <= '9'; }),
            "Invalid scientific exponent");
        const auto power = std::stoul(exponent);
        require(power <= 1233, "Scientific exponent bound exceeded");
        mpz_class factor;
        mpz_ui_pow_ui(factor.get_mpz_t(), 10, power);
        if (exponent_negative) denominator *= factor;
        else numerator *= factor;
    }
    return checked_problem_rational(Rational(numerator, denominator));
}

std::string evidence_digest(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1
        && length == 32, "Cannot hash evidence view");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned i = 0; i < length; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 15];
    }
    return result;
}

EvidenceView make_evidence_view(std::string id, PremiseBasis premise_basis, std::string text,
                               std::string archive_id, std::string entry_path,
                               std::string extractor_version) {
    require(!text.empty() && text.size() <= 65536 && utf8(text), "Invalid evidence view text");
    EvidenceView result;
    result.id = std::move(id);
    result.basis = premise_basis;
    result.archive_id = std::move(archive_id);
    result.entry_path = std::move(entry_path);
    result.extractor_version = std::move(extractor_version);
    result.digest = evidence_digest(text);
    result.text = std::move(text);
    return result;
}

namespace {
AdmittedProblem admit_problem_impl(const ProblemRequest& request, bool with_goal) {
    const auto& context = request.context;
    const auto& proposal = request.proposal;
    require(request.limits.work > 0 && request.limits.work <= 4000000
        && request.limits.steps > 0 && request.limits.steps <= 256, "Invalid problem limits");
    require(context.views.size() <= 16 && context.units.size() <= 64
        && proposal.entities.size() <= 64 && proposal.quantities.size() <= 64
        && proposal.numbers.size() <= 128 && proposal.bindings.size() <= 128
        && proposal.nodes.size() <= 256 && proposal.equations.size() <= 64,
        "Problem admission count bound exceeded");
    AdmittedProblem admitted;
    std::size_t all_text = 0, source_bytes = 0;
    const auto text = [&](const std::string& value, std::size_t maximum, bool empty = false) {
        require((empty || !value.empty()) && value.size() <= maximum && utf8(value),
                "Invalid problem text");
        all_text += value.size();
        require(all_text <= max_wire_bytes / 2, "Problem aggregate text bound exceeded");
    };
    std::set<std::string> view_ids;
    for (const auto& view : context.views) {
        text(view.id, 128);
        text(view.archive_id, 1024, view.basis != PremiseBasis::Source);
        text(view.entry_path, 1024, view.basis != PremiseBasis::Source);
        text(view.extractor_version, 128);
        text(view.text, 65536);
        require(basis(view.basis) && view_ids.insert(view.id).second, "Invalid or duplicate evidence view");
        require(view.digest == evidence_digest(view.text), "Evidence view digest mismatch");
        source_bytes += view.text.size();
        require(source_bytes <= 131072 && view.locators.size() <= 256, "Evidence byte or locator bound exceeded");
        for (std::size_t i = 0; i < view.locators.size(); ++i) {
            const auto& locator = view.locators[i];
            require(locator.offset <= view.text.size() && locator.length > 0
                && locator.length <= view.text.size() - locator.offset
                && boundary(view.text, locator.offset)
                && boundary(view.text, locator.offset + locator.length), "Invalid evidence locator range");
            text(locator.table, 128, true); text(locator.row, 128, true); text(locator.column, 128, true);
            require(locator.headers.size() <= 16, "Locator header bound exceeded");
            if (locator.role == LocatorRole::Paragraph) {
                require(locator.table.empty() && locator.row.empty() && locator.column.empty()
                    && locator.headers.empty(), "Paragraph has table coordinates");
            } else {
                require((locator.role == LocatorRole::TableCell || locator.role == LocatorRole::TableHeader)
                    && !locator.table.empty()
                    && (!locator.row.empty() || !locator.column.empty()), "Invalid table locator");
                if (locator.role == LocatorRole::TableCell) {
                    require(!locator.row.empty() && !locator.column.empty(), "Missing table cell coordinate");
                }
                for (const auto header : locator.headers) {
                    require(header < i && view.locators[header].role == LocatorRole::TableHeader
                        && view.locators[header].table == locator.table, "Invalid table header reference");
                }
            }
        }
    }
    admitted.admission_work = 1 + source_bytes / 64;
    const auto span_text = [&](const EvidenceSpan& span) -> std::string_view {
        require(span.view < context.views.size(), "Invalid evidence view reference");
        const auto& view = context.views[span.view];
        require(span.offset <= view.text.size() && span.length > 0
            && span.length <= view.text.size() - span.offset
            && boundary(view.text, span.offset) && boundary(view.text, span.offset + span.length),
            "Invalid evidence span");
        if (view.locators.empty()) require(span.locator == no_step, "Unknown evidence locator");
        else {
            require(span.locator < view.locators.size(), "Missing evidence locator");
            const auto& locator = view.locators[span.locator];
            require(span.offset >= locator.offset
                && static_cast<std::uint64_t>(span.offset) + span.length
                   <= static_cast<std::uint64_t>(locator.offset) + locator.length,
                "Span crosses its structural locator");
        }
        return std::string_view(view.text).substr(span.offset, span.length);
    };
    const auto origin = [&](const ProblemOrigin& value) {
        require(basis(value.basis) && value.supports.size() <= 16, "Invalid premise origin");
        text(value.label, 1024);
        require(value.basis == PremiseBasis::Formal || !value.supports.empty(), "Uncited interpretation");
        bool question = false, source = false;
        for (const auto& support : value.supports) {
            (void)span_text(support);
            question |= context.views[support.view].basis == PremiseBasis::Question;
            source |= context.views[support.view].basis == PremiseBasis::Source;
        }
        require(value.basis != PremiseBasis::Formal || (!question && !source), "Formal origin cites interpreted data");
        require(value.basis != PremiseBasis::Question || (question && !source), "Invalid question origin");
        require(value.basis != PremiseBasis::Source || source, "Invalid source origin");
    };
    const auto scope = [&](const ProblemScope& value) {
        require(value.qualifiers.size() <= 16, "Scope qualifier bound exceeded");
        std::string previous;
        for (const auto& [key, item] : value.qualifiers) {
            require(identifier(key) && identifier(item) && (previous.empty() || previous < key),
                    "Invalid or noncanonical scope qualifier");
            text(key, 128); text(item, 128);
            previous = key;
        }
    };
    text(context.unit_registry_id, 128, context.units.empty());
    text(context.unit_registry_version, 128, context.units.empty());
    std::set<std::string> unit_ids, spellings;
    for (const auto& unit : context.units) {
        require(identifier(unit.id) && unit_ids.insert(unit.id).second, "Invalid or duplicate unit ID");
        text(unit.id, 128); text(unit.symbol, 128);
        const auto normalized = checked_problem_rational(unit.scale);
        require(normalized > 0 && normalized.get_num() == unit.scale.get_num()
            && normalized.get_den() == unit.scale.get_den(), "Invalid or noncanonical unit scale");
        for (const auto power : unit.dimension) require(power >= -64 && power <= 64, "Invalid unit dimension");
        require(!unit.spellings.empty() && unit.spellings.size() <= 16, "Invalid unit spellings");
        bool symbol_present = false;
        for (const auto& spelling : unit.spellings) {
            text(spelling, 128);
            require(spellings.insert(spelling).second, "Ambiguous unit spelling");
            symbol_present |= spelling == unit.symbol;
        }
        require(symbol_present, "Unit symbol missing from spellings");
    }
    std::set<std::string> entities;
    for (const auto& entity : proposal.entities) {
        require(identifier(entity) && entities.insert(entity).second, "Invalid or duplicate entity");
        text(entity, 128);
    }
    std::set<std::tuple<std::string, std::uint32_t, bool,
                        std::vector<std::pair<std::string, std::string>>>> slots;
    for (const auto& quantity : proposal.quantities) {
        require(identifier(quantity.property) && quantity.entity < proposal.entities.size()
            && quantity.unit < context.units.size(), "Invalid quantity declaration");
        text(quantity.property, 128);
        scope(quantity.scope);
        require(slots.emplace(quantity.property, quantity.entity, quantity.scope.resolved,
                              quantity.scope.qualifiers).second, "Duplicate semantic quantity slot");
    }
    for (const auto& number : proposal.numbers) {
        require(number.unit < context.units.size(), "Unknown numeral unit");
        require(number.precision == QuantityPrecision::Exact || number.precision == QuantityPrecision::Approximate
            || number.precision == QuantityPrecision::Bound || number.precision == QuantityPrecision::Unspecified,
            "Invalid quantity precision");
        const auto lexical = span_text(number.numeral);
        const auto& numeral_view = context.views[number.numeral.view];
        const auto& numeral_text = numeral_view.text;
        const auto lower = number.numeral.locator == no_step ? 0U
            : numeral_view.locators[number.numeral.locator].offset;
        const auto upper = number.numeral.locator == no_step ? numeral_text.size()
            : static_cast<std::size_t>(numeral_view.locators[number.numeral.locator].offset)
              + numeral_view.locators[number.numeral.locator].length;
        const auto start = static_cast<std::size_t>(number.numeral.offset);
        const auto end = start + number.numeral.length;
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        auto sign_end = start;
        while (sign_end > lower && space(numeral_text[sign_end - 1])) --sign_end;
        require(!(sign_end > lower && (numeral_text[sign_end - 1] == '-'
                    || numeral_text[sign_end - 1] == '+'))
            && !(sign_end >= lower + 3
                 && std::string_view(numeral_text).substr(sign_end - 3,3) == "\xe2\x88\x92"),
            "Numeral span omits its sign");
        require(!(start > lower && digit(numeral_text[start - 1]))
            && !(end < upper && digit(numeral_text[end])), "Numeral span cuts through digits");
        require(!(start >= lower + 2 && numeral_text[start - 1] == '.'
                  && digit(numeral_text[start - 2]))
            && !(end + 1 < upper && numeral_text[end] == '.' && digit(numeral_text[end + 1])),
            "Numeral span cuts through a decimal");
        const auto exponent = [](char c) { return c == 'e' || c == 'E'; };
        if (end < upper && exponent(numeral_text[end])) {
            auto following = end + 1;
            if (following < upper && (numeral_text[following] == '+' || numeral_text[following] == '-')) ++following;
            require(following == upper || !digit(numeral_text[following]),
                    "Numeral span omits its exponent");
        }
        require(!(start >= lower + 2 && exponent(numeral_text[start - 1])
                  && (digit(numeral_text[start - 2]) || numeral_text[start - 2] == '.'))
            && !(start >= lower + 3 && (numeral_text[start - 1] == '+' || numeral_text[start - 1] == '-')
                  && exponent(numeral_text[start - 2]) && digit(numeral_text[start - 3])),
            "Numeral span selects an exponent fragment");
        const auto& unit = context.units[number.unit];
        if (number.unit_span) {
            require(number.unit_span->view == number.numeral.view, "Unit comes from another source view");
            const auto selected = span_text(*number.unit_span);
            require(std::find(unit.spellings.begin(), unit.spellings.end(), selected) != unit.spellings.end(),
                    "Cited unit does not match registry");
        } else require(dimensionless(unit.dimension) && unit.scale == 1, "Missing dimensional unit span");
        if (number.qualifier) {
            require(number.qualifier->view == number.numeral.view, "Qualifier comes from another source view");
            (void)span_text(*number.qualifier);
        }
        const auto parsed = parse_evidence_number(lexical);
        admitted.lexical_numbers.push_back(parsed);
        admitted.canonical_numbers.push_back(checked_problem_rational(parsed * unit.scale));
        admitted.admission_work += lexical.size() + 1;
    }
    for (const auto& binding : proposal.bindings) {
        require(binding.quantity < proposal.quantities.size() && binding.number < proposal.numbers.size(),
                "Invalid quantity binding");
        const auto& quantity = proposal.quantities[binding.quantity];
        require(context.units[quantity.unit].dimension
            == context.units[proposal.numbers[binding.number].unit].dimension, "Binding dimension mismatch");
        origin(binding.origin);
    }
    for (std::size_t i = 0; i < proposal.nodes.size(); ++i) {
        const auto dimension = std::visit([&](const auto& node) -> ProblemDimension {
            using Node = std::decay_t<decltype(node)>;
            if constexpr (std::is_same_v<Node, ProblemQuantityNode>) {
                require(node.quantity < proposal.quantities.size(), "Invalid quantity node");
                return context.units[proposal.quantities[node.quantity].unit].dimension;
            } else if constexpr (std::is_same_v<Node, ProblemNumberNode>) {
                require(node.number < proposal.numbers.size(), "Invalid numeral node");
                return context.units[proposal.numbers[node.number].unit].dimension;
            } else if constexpr (std::is_same_v<Node, ProblemNegateNode>) {
                require(node.operand < i, "Non-acyclic unary node");
                return admitted.node_dimensions[node.operand];
            } else {
                require(node.left < i && node.right < i, "Non-acyclic binary node");
                const auto& left = admitted.node_dimensions[node.left];
                const auto& right = admitted.node_dimensions[node.right];
                if (node.operation == Code::Add || node.operation == Code::Subtract) {
                    require(left == right, "Additive dimension mismatch");
                    return left;
                }
                require(node.operation == Code::Multiply || node.operation == Code::Divide,
                        "Unsupported problem expression opcode");
                return combined(left, right, node.operation == Code::Divide);
            }
        }, proposal.nodes[i]);
        admitted.node_dimensions.push_back(dimension);
        ++admitted.admission_work;
    }
    std::vector<Atom> guard_goals;
    for (const auto& equation : proposal.equations) {
        require(equation.left < proposal.nodes.size() && equation.right < proposal.nodes.size(),
                "Invalid equation root");
        require(admitted.node_dimensions[equation.left] == admitted.node_dimensions[equation.right],
                "Equation dimension mismatch");
        scope(equation.scope);
        origin(equation.origin);
        std::vector<std::uint32_t> pending{equation.left, equation.right};
        std::vector<bool> seen(proposal.nodes.size(), false);
        while (!pending.empty()) {
            const auto index = pending.back(); pending.pop_back();
            if (seen[index]) continue;
            seen[index] = true;
            ++admitted.admission_work;
            std::visit([&](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, ProblemQuantityNode>) {
                    require(proposal.quantities[node.quantity].scope == equation.scope,
                            "Equation crosses quantity scopes");
                } else if constexpr (std::is_same_v<Node, ProblemNegateNode>) pending.push_back(node.operand);
                else if constexpr (std::is_same_v<Node, ProblemBinaryNode>) {
                    pending.push_back(node.left); pending.push_back(node.right);
                }
            }, proposal.nodes[index]);
        }
        if (equation.guard) guard_goals.push_back(*equation.guard);
    }
    if (with_goal) {
        if (const auto goal = std::get_if<ProblemQuantityGoal>(&proposal.goal)) {
            require(goal->quantity < proposal.quantities.size() && goal->unit < context.units.size(),
                    "Invalid quantity goal");
            require(context.units[proposal.quantities[goal->quantity].unit].dimension
                == context.units[goal->unit].dimension, "Goal unit dimension mismatch");
        } else guard_goals.push_back(std::get<ProblemAtomGoal>(proposal.goal).atom);
    }
    const bool has_logic = !proposal.logic.name.empty() || !proposal.logic.constants.empty()
        || !proposal.logic.predicates.empty() || !proposal.logic.rules.empty();
    if (has_logic) {
        require(proposal.logic.constants == proposal.entities
            && proposal.logic_origins.size() == proposal.logic.rules.size(), "Logic identity/origin mismatch");
        text(proposal.logic.name, 128); text(proposal.logic.version, 128);
        for (const auto& rule : proposal.logic.rules) text(rule.provenance, 1024, true);
        for (const auto& value : proposal.logic_origins) origin(value);
        require(!proposal.logic.predicates.empty(), "Missing logic predicates");
        for (const auto& atom : guard_goals) {
            require(atom.predicate < proposal.logic.predicates.size()
                && atom.terms.size() == proposal.logic.predicates[atom.predicate].arity, "Invalid ground guard");
            for (const auto& term : atom.terms) {
                require(term.kind == TermKind::Constant && term.index < proposal.entities.size(),
                        "Non-ground guard");
            }
        }
        Atom validation_goal;
        if (!guard_goals.empty()) validation_goal = guard_goals.front();
        else {
            validation_goal.predicate = 0;
            validation_goal.terms.assign(proposal.logic.predicates.front().arity, {TermKind::Constant, 0});
        }
        const auto grounding = ground_relational({proposal.logic, validation_goal, {}, false});
        admitted.admission_work += grounding.instances.size() + grounding.request.module.symbols.size();
    } else require(guard_goals.empty() && proposal.logic_origins.empty(), "Guard without a theory");
    return admitted;
}
}

AdmittedProblem admit_problem(const ProblemRequest& request) {
    return admit_problem_impl(request, true);
}

AdmittedProblem admit_problem_theory(const ProblemRequest& request) {
    return admit_problem_impl(request, false);
}

} // namespace axiom
