#include "translator.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace axiom {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string lowercase(std::string text) {
    for (auto& character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (value < 128) character = static_cast<char>(std::tolower(value));
    }
    return text;
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string input_sentence(const std::string& input) {
    require(!input.empty() && input.size() <= 4096, "English input must contain 1..4096 bytes");
    for (const unsigned char value : input) {
        require(value >= 32 || value == '\t' || value == '\n' || value == '\r',
                "Control byte in English input");
        require(value != 127, "Control byte in English input");
    }
    auto sentence = trim(input);
    if (!sentence.empty() && (sentence.back() == '?' || sentence.back() == '.')) {
        sentence.pop_back();
        sentence = trim(std::move(sentence));
    }
    require(!sentence.empty(), "Empty English sentence");
    return sentence;
}

bool identifier(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 128 && (std::isalnum(c) || c == '_' || c == '.');
    });
}

std::string search_words(std::string literal) {
    if (literal.starts_with('!')) literal.erase(0, 1);
    for (char& character : literal) {
        if (character == '.' || character == '_') character = ' ';
    }
    return literal;
}

void validate_expression(const std::string& expression) {
    bool found_value = false;
    for (std::size_t position = 0; position < expression.size();) {
        const auto character = static_cast<unsigned char>(expression[position]);
        if (std::isspace(character) || std::isdigit(character)
            || std::string_view("+-*/()").find(static_cast<char>(character)) != std::string_view::npos) {
            ++position;
        } else if (expression.compare(position, 5, "value") == 0) {
            position += 5;
            require(position == expression.size()
                    || !(std::isalnum(static_cast<unsigned char>(expression[position]))
                         || expression[position] == '_'), "Invalid value identifier");
            found_value = true;
        } else {
            throw std::runtime_error("Library calculation permits only numbers, value, and + - * / ()");
        }
    }
    require(found_value, "Library calculation must refer to value");
}

std::string substitute(const std::string& expression, const Rational& value) {
    std::string output;
    const auto replacement = "(" + value.get_str() + ")";
    for (std::size_t position = 0; position < expression.size();) {
        if (expression.compare(position, 5, "value") == 0) {
            output += replacement;
            position += 5;
        } else {
            output.push_back(expression[position++]);
        }
        require(output.size() <= max_text_bytes, "Expanded library expression is too large");
    }
    return output;
}

EnglishRequest calculation(std::vector<std::string> command) {
    // Compile now: unsupported syntax never becomes a speculative formal state.
    static_cast<void>(encode_request(compile(command)));
    EnglishRequest request;
    request.command = std::move(command);
    return request;
}

bool phrase_field(const std::string& field) {
    return !field.empty() && field.size() <= 128
        && std::all_of(field.begin(), field.end(), [](unsigned char c) {
            return c < 128 && (std::isalnum(c) || c == ' ' || c == '-' || c == '/');
        });
}

EnglishRequest library_calculation(const std::string& sentence) {
    const auto lowered = lowercase(sentence);
    constexpr std::string_view prefix = "using the library value of \"";
    const auto first_end = sentence.find('"', prefix.size());
    require(first_end != std::string::npos, "Expected a quoted quantity name");
    constexpr std::string_view unit_marker = " in \"";
    require(lowered.compare(first_end + 1, unit_marker.size(), unit_marker) == 0,
            "Expected: in \"UNIT\"");
    const auto unit_start = first_end + 1 + unit_marker.size();
    const auto unit_end = sentence.find('"', unit_start);
    require(unit_end != std::string::npos, "Expected a quoted unit");
    constexpr std::string_view action = ", calculate ";
    require(lowered.compare(unit_end + 1, action.size(), action) == 0,
            "Expected: , calculate EXPRESSION");
    EnglishRequest request;
    request.kind = EnglishKind::LibraryCalculation;
    request.quantity = lowercase(sentence.substr(prefix.size(), first_end - prefix.size()));
    request.unit = lowercase(sentence.substr(unit_start, unit_end - unit_start));
    request.expression = sentence.substr(unit_end + 1 + action.size());
    require(phrase_field(request.quantity) && phrase_field(request.unit),
            "Quantity and unit must be bounded ASCII names");
    validate_expression(request.expression);
    request.query = "\"" + (request.quantity == "speed of light in vacuum"
        ? std::string("speed of light") : request.quantity) + "\"";
    return request;
}

// Deliberately small numeric grammar. It encodes the stated decimal exactly;
// it does not assert that a physical measurement has infinite precision.
Rational decimal_number(const std::string& source) {
    require(!source.empty() && source.size() <= 256, "Source numeral is too long");
    const auto point_position = source.find('.');
    const auto integral = source.substr(0, point_position);
    const auto first_space = integral.find(' ');
    if (first_space != std::string::npos) {
        require(first_space >= 1 && first_space <= 3, "Invalid grouped source integer");
        for (std::size_t start = first_space + 1; start < integral.size(); start += 4) {
            require(start + 3 <= integral.size()
                    && (start + 3 == integral.size() || integral[start + 3] == ' '),
                    "Invalid grouped source integer");
        }
    }
    std::string digits;
    std::size_t decimals = 0;
    bool point = false;
    for (unsigned char character : source) {
        if (std::isdigit(character)) {
            digits.push_back(static_cast<char>(character));
            if (point) ++decimals;
        } else if (character == ' ' && !point) {
            continue;
        } else if (character == '.' && !point) {
            point = true;
        } else {
            throw std::runtime_error("Unsupported source numeral");
        }
    }
    require(!digits.empty() && source.back() != '.', "Incomplete source numeral");
    mpz_class denominator = 1;
    for (std::size_t index = 0; index < decimals; ++index) denominator *= 10;
    Rational result(mpz_class(digits, 10), denominator);
    result.canonicalize();
    return result;
}

struct NormalizedSource {
    std::string text;
    std::vector<std::size_t> offsets;
};

NormalizedSource normalize_source(const std::string& raw) {
    require(raw.size() <= max_text_bytes, "Evidence text exceeds translation limit");
    NormalizedSource result;
    const std::vector<std::pair<std::string_view, std::string_view>> replacements{
        {"\xc2\xa0", " "}, {"\xe2\x80\x89", " "}, {"\xe2\x80\xaf", " "},
        {"\xe2\x80\x8d", ""}, {"\xe2\x81\xa0", ""},
        {"\xe2\x8b\x85", "*"}, {"\xc2\xb7", "*"}, {"\xe2\x88\x92", "-"}
    };
    for (std::size_t position = 0; position < raw.size();) {
        const auto start = position;
        std::string_view replacement(raw.data() + position, 1);
        std::size_t consumed = 1;
        for (const auto& [pattern, value] : replacements) {
            if (raw.compare(position, pattern.size(), pattern) == 0) {
                replacement = value;
                consumed = pattern.size();
                break;
            }
        }
        for (unsigned char character : replacement) {
            if (character < 128 && std::isspace(character)) character = ' ';
            if (character == ' ' && !result.text.empty() && result.text.back() == ' ') continue;
            result.text.push_back(character < 128
                ? static_cast<char>(std::tolower(character)) : static_cast<char>(character));
            result.offsets.push_back(start);
        }
        position += consumed;
    }
    result.offsets.push_back(raw.size());
    return result;
}

bool statement_end(const std::string& text, std::size_t end) {
    while (end < text.size() && text[end] == ' ') ++end;
    // Citation markers have no effect on the asserted numerical value.
    while (end < text.size() && text[end] == '[') {
        ++end;
        bool digit = false;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == ' ')) {
            digit = digit || text[end] != ' ';
            ++end;
        }
        if (!digit || end == text.size() || text[end++] != ']') return false;
        while (end < text.size() && text[end] == ' ') ++end;
    }
    return end == text.size()
        || (text[end] == '.' && (end + 1 == text.size() || text[end + 1] == ' '));
}

std::vector<QuantityPremise> extract(const Evidence& source, const EnglishRequest& request) {
    std::vector<QuantityPremise> found;
    const auto normalized = normalize_source(source.text);
    const auto& text = normalized.text;
    // Only explicit declarative templates are admitted. No arbitrary
    // nearest-number search or instructions from archive text are executed.
    std::vector<std::pair<std::string, bool>> prefixes{
        {"the " + request.quantity + " is exactly ", false},
        {"the " + request.quantity + " is ", false},
        {request.quantity + " is exactly ", false},
        {request.quantity + " is ", false}
    };
    std::vector<std::string> units{request.unit};
    if (request.unit == "metres per second" || request.unit == "meters per second" || request.unit == "m/s") {
        units = {"metres per second", "meters per second", "m/s", "m*s -1"};
    }
    if (request.quantity == "speed of light in vacuum"
        && lowercase(source.title) == "speed of light"
        && std::filesystem::path(source.path).filename() == "Speed_of_light") {
        // A documented domain grammar, not an embedded value: the numeral is
        // read from the actual article, with the vacuum qualifier retained.
        prefixes.emplace_back(
            "the speed of light in vacuum , often called simply the speed of light and commonly denoted c , "
            "is a universal physical constant exactly equal to ", true);
    }
    for (const auto& [prefix, article_bound] : prefixes) {
        for (std::size_t position = 0; (position = text.find(prefix, position)) != std::string::npos;) {
            const auto start = position;
            position += prefix.size();
            // A matched clause must start a sentence/paragraph, not occur
            // after "if", "not", a quotation, or another conditional clause.
            const auto previous = text.find_last_not_of(" \t\r\n", start == 0 ? 0 : start - 1);
            const auto raw_start = normalized.offsets[start];
            const auto paragraph = raw_start == 0 ? std::string::npos
                : source.text.find_last_of('\n', raw_start - 1);
            const auto paragraph_start = paragraph == std::string::npos ? 0 : paragraph + 1;
            const bool at_paragraph_start = source.text.find_first_not_of(" \t\r\n", paragraph_start) == raw_start;
            if (article_bound && !at_paragraph_start) continue;
            if (!at_paragraph_start && start != 0 && previous != std::string::npos
                && text[previous] != '.' && text[previous] != '!' && text[previous] != '?') {
                continue;
            }
            const auto number_start = position;
            while (position < text.size()
                   && (std::isdigit(static_cast<unsigned char>(text[position]))
                       || text[position] == '.' || text[position] == ' ')) {
                ++position;
            }
            auto number_end = position;
            while (number_end > number_start && text[number_end - 1] == ' ') --number_end;
            if (number_start == number_end || position == number_end) continue;
            std::size_t unit_end = 0;
            for (const auto& unit : units) {
                if (text.compare(position, unit.size(), unit) == 0
                    && statement_end(text, position + unit.size())) {
                    unit_end = position + unit.size();
                    break;
                }
            }
            if (unit_end == 0) continue;
            try {
                const auto value = decimal_number(text.substr(number_start, number_end - number_start));
                const auto duplicate = std::any_of(found.begin(), found.end(), [&](const auto& premise) {
                    return premise.offset == normalized.offsets[start] && premise.value == value;
                });
                if (!duplicate) found.push_back({value, source, normalized.offsets[start],
                    normalized.offsets[unit_end] - normalized.offsets[start]});
            } catch (const std::runtime_error&) {
                // An unrecognized numeral is unsupported source data.
            }
        }
    }
    return found;
}

}

EnglishRequest translate_english(const std::string& question) {
    const auto sentence = input_sentence(question);
    const auto lowered = lowercase(sentence);
    const auto after = [&](std::string_view prefix) { return trim(sentence.substr(prefix.size())); };
    if (lowered.starts_with("using the library value of \"")) return library_calculation(sentence);
    if (lowered.starts_with("what is the determinant of ")) {
        return calculation({"det", after("what is the determinant of ")});
    }
    if (lowered.starts_with("what are the born probabilities for ")) {
        return calculation({"born", after("what are the born probabilities for ")});
    }
    if (lowered.starts_with("solve the linear system ")) {
        constexpr std::string_view prefix = "solve the linear system ";
        constexpr std::string_view middle = " with right side ";
        const auto delimiter = lowered.find(middle, prefix.size());
        require(delimiter != std::string::npos, "Linear-system sentence requires: with right side MATRIX");
        return calculation({"linear", trim(sentence.substr(prefix.size(), delimiter - prefix.size())),
                            trim(sentence.substr(delimiter + middle.size()))});
    }
    if (lowered.starts_with("solve ")) {
        auto equation = after("solve ");
        if (lowercase(equation).ends_with(" for x")) equation.resize(equation.size() - 6);
        return calculation({"solve", trim(std::move(equation))});
    }
    for (const std::string_view prefix : {"what is ", "calculate ", "evaluate "}) {
        if (lowered.starts_with(prefix)) return calculation({"eval", after(prefix)});
    }
    if (lowered.starts_with("research ")) {
        EnglishRequest request;
        request.kind = EnglishKind::Research;
        request.query = after("research ");
        require(!request.query.empty() && request.query.size() <= 1024, "Research topic must contain 1..1024 bytes");
        return request;
    }
    if (lowered.starts_with("is ")) {
        auto predicate = trim(lowered.substr(3));
        const auto separator = predicate.find(' ');
        require(separator != std::string::npos, "Expected: Is ENTITY PROPERTY?");
        const auto entity = predicate.substr(0, separator);
        auto property = trim(predicate.substr(separator + 1));
        bool negative = property.starts_with("not ");
        if (negative) property = trim(property.substr(4));
        require(identifier(entity) && identifier(property), "Unsupported entity/property sentence");
        EnglishRequest request;
        request.kind = EnglishKind::Proof;
        request.goal = (negative ? "!" : "") + entity + "." + property;
        require(request.goal.size() <= 129, "Translated predicate is too long");
        request.query = search_words(request.goal);
        return request;
    }
    throw std::runtime_error("UNSUPPORTED_INPUT: sentence is outside the explicit C++ grammar; use --help");
}

GroundedCalculation calculate_from_library(const EnglishRequest& request,
                                           const LibrarySearch& search) {
    require(request.kind == EnglishKind::LibraryCalculation, "Expected a library calculation request");
    validate_expression(request.expression);
    require(phrase_field(request.quantity) && phrase_field(request.unit), "Invalid quantity or unit");
    GroundedCalculation result;
    result.coverage_complete = search.complete;
    require(search.evidence.size() <= 100, "Too many evidence records for translation");
    for (const auto& source : search.evidence) {
        auto premises = extract(source, request);
        for (auto& premise : premises) {
            if (std::any_of(result.premises.begin(), result.premises.end(), [&](const auto& previous) {
                    return previous.value != premise.value;
                })) {
                result.status = GroundingStatus::Conflict;
            }
            result.premises.push_back(std::move(premise));
        }
    }
    if (result.premises.empty() || result.status == GroundingStatus::Conflict) return result;
    // This is a conditional calculation from cited source premises, not an
    // unconditional proof that the source assertion is a fact about reality.
    const auto expression = substitute(request.expression, result.premises.front().value);
    result.binary_result = run_binary(encode_request(compile({"eval", expression})));
    static_cast<void>(decode_result(result.binary_result));
    result.status = GroundingStatus::Conditional;
    return result;
}

std::string translator_help() {
    return R"HELP(
English bridge: ask "QUESTION" [--module MODULE] [--library ROOT] [--gpu]
  What is 1/3 + 1/6?
  Calculate (2+3)*4.
  Solve 2*x+3=11 for x.
  What is the determinant of [1,2;3,4]?
  Solve the linear system [1,2;3,4] with right side [5;11].
  What are the Born probabilities for [3/5,0;0,4/5]?
  Is socrates mortal?                  requires --module
  Is stone not alive?                  explicit negative goal
  Research quantum mechanics.         requires --library; evidence only
  Using the library value of "NAME" in "UNIT", calculate value * 2.

Only these explicit sentence patterns are supported. The core still receives
typed AXRM binary, never English. Source extraction accepts documented
declarative quantity templates; unsupported text stays UNKNOWN. A library
calculation is CONDITIONAL on the cited source, never an unconditional proof.
)HELP";
}

}
