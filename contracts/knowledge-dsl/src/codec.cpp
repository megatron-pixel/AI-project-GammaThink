#include "archive_dsl/codec.hpp"

#include <json-c/json.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <map>
#include <memory>
#include <type_traits>
#include <vector>

namespace gammathink::archive_dsl {
namespace {
constexpr std::array<std::string_view, 10> roles = {
    "ENTITY", "QUANTITY", "NUMBER", "BINDING", "NODE",
    "EQUATION", "PREDICATE", "RULE", "QUANTIFIED_EQUATION", "GOAL"};
constexpr std::array<std::string_view, 9> fields = {
    "entities", "quantities", "numbers", "bindings", "nodes",
    "equations", "predicates", "rules", "quantified_equations"};

void require(bool condition, const char* message) {
    if (!condition) throw Error(message);
}
void bounded(std::string_view text) {
    require(text.size() <= max_bytes, "Byte limit exceeded");
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i++]);
        if (lead < 0x80) continue;
        const unsigned count = lead >= 0xc2 && lead <= 0xdf ? 1
            : lead >= 0xe0 && lead <= 0xef ? 2 : lead >= 0xf0 && lead <= 0xf4 ? 3 : 0;
        require(count != 0 && i + count <= text.size(), "Invalid UTF-8 scalar");
        unsigned code = lead & ((1U << (6 - count)) - 1);
        for (unsigned j = 0; j < count; ++j) {
            const auto byte = static_cast<unsigned char>(text[i++]);
            require(byte >= 0x80 && byte <= 0xbf, "Invalid UTF-8 continuation");
            code = code * 64 + (byte & 0x3f);
        }
        require(code >= (count == 1 ? 0x80U : count == 2 ? 0x800U : 0x10000U)
            && code <= 0x10ffff && (code < 0xd800 || code > 0xdfff),
            "Invalid UTF-8 scalar");
    }
}
struct Value {
    char kind = 'n';
    std::string raw;
    std::string text;
    std::vector<Value> items;
    std::map<std::string, Value> members;
};

// A small structural scanner retains exact JSON number tokens and rejects
// duplicate decoded keys before json-c could silently replace them.
struct Parser {
    std::string_view source;
    std::size_t pos = 0;
    void space() {
        while (pos < source.size() && std::string_view(" \t\r\n").find(source[pos])
               != std::string_view::npos) ++pos;
    }
    char peek() const { return pos < source.size() ? source[pos] : '\0'; }
    unsigned hex4() {
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            require(pos < source.size(), "Truncated Unicode escape");
            const char c = source[pos++];
            const auto index = std::string_view("0123456789abcdef").find(
                c >= 'A' && c <= 'F' ? static_cast<char>(c + 32) : c);
            require(index != std::string_view::npos, "Invalid Unicode escape");
            value = value * 16 + static_cast<unsigned>(index);
        }
        return value;
    }
    Value string() {
        const auto start = pos++;
        bool closed = false;
        while (pos < source.size()) {
            const auto c = static_cast<unsigned char>(source[pos++]);
            if (c == '"') { closed = true; break; }
            require(c >= 32, "Control character in string");
            if (c != '\\') continue;
            require(pos < source.size(), "Truncated string escape");
            const char escape = source[pos++];
            require(std::string_view("\"\\/bfnrtu").find(escape)
                    != std::string_view::npos, "Invalid string escape");
            if (escape != 'u') continue;
            const auto code = hex4();
            require(code < 0xdc00 || code > 0xdfff, "Unpaired low surrogate");
            if (code >= 0xd800 && code <= 0xdbff) {
                require(source.substr(pos, 2) == "\\u", "Unpaired high surrogate");
                pos += 2;
                const auto low = hex4();
                require(low >= 0xdc00 && low <= 0xdfff, "Unpaired high surrogate");
            }
        }
        require(closed, "Unterminated string");
        Value result;
        result.kind = 's';
        result.raw = std::string(source.substr(start, pos - start));
        const std::unique_ptr<json_tokener, decltype(&json_tokener_free)>
            tok(json_tokener_new(), json_tokener_free);
        require(tok != nullptr, "Cannot allocate JSON scanner");
        json_tokener_set_flags(tok.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        const std::unique_ptr<json_object, decltype(&json_object_put)> parsed(
            json_tokener_parse_ex(tok.get(), result.raw.c_str(),
                                 static_cast<int>(result.raw.size())), json_object_put);
        require(json_tokener_get_error(tok.get()) == json_tokener_success
                    && parsed != nullptr, "Invalid UTF-8 JSON string");
        result.text.assign(json_object_get_string(parsed.get()),
                           static_cast<std::size_t>(json_object_get_string_len(parsed.get())));
        return result;
    }
    Value value(std::size_t depth = 0) {
        require(depth <= max_depth, "JSON depth limit exceeded");
        space();
        if (peek() == '"') return string();
        Value result;
        if (peek() == '[' || peek() == '{') {
            const char open = source[pos++];
            const char close = open == '[' ? ']' : '}';
            result.kind = open == '[' ? 'a' : 'o';
            result.raw += open;
            space();
            if (peek() != close) {
                while (true) {
                    if (open == '{') {
                        require(peek() == '"', "Expected object key");
                        auto key = string();
                        space();
                        require(peek() == ':', "Expected colon");
                        ++pos;
                        auto child = value(depth + 1);
                        require(!result.members.contains(key.text), "Duplicate JSON key");
                        result.raw += key.raw + ":" + child.raw;
                        result.members.emplace(std::move(key.text), std::move(child));
                    } else {
                        auto child = value(depth + 1);
                        result.raw += child.raw;
                        result.items.push_back(std::move(child));
                    }
                    space();
                    if (peek() != ',') break;
                    result.raw += ',';
                    ++pos;
                    space();
                }
            }
            require(peek() == close, "Expected closing delimiter");
            ++pos;
            result.raw += close;
            return result;
        }
        const auto start = pos;
        while (pos < source.size()
               && std::string_view(" \t\r\n,]}").find(source[pos]) == std::string_view::npos)
            ++pos;
        result.raw = std::string(source.substr(start, pos - start));
        if (result.raw == "null" || result.raw == "true" || result.raw == "false")
            return result;
        // JSON's number grammar (not strtod's permissive extensions).
        std::size_t i = 0;
        const auto digit = [&]() { return i < result.raw.size()
            && result.raw[i] >= '0' && result.raw[i] <= '9'; };
        if (i < result.raw.size() && result.raw[i] == '-') ++i;
        require(digit(), "Invalid JSON value");
        if (result.raw[i] == '0') ++i;
        else while (digit()) ++i;
        if (i < result.raw.size() && result.raw[i] == '.') {
            ++i;
            require(digit(), "Invalid JSON fraction");
            while (digit()) ++i;
        }
        if (i < result.raw.size() && (result.raw[i] == 'e' || result.raw[i] == 'E')) {
            ++i;
            if (i < result.raw.size() && (result.raw[i] == '+' || result.raw[i] == '-')) ++i;
            require(digit(), "Invalid JSON exponent");
            while (digit()) ++i;
        }
        require(i == result.raw.size(), "Invalid JSON number");
        static const std::unique_ptr<std::remove_pointer_t<locale_t>, decltype(&freelocale)>
            numeric_locale(newlocale(LC_NUMERIC_MASK, "C", nullptr), freelocale);
        require(numeric_locale != nullptr, "Cannot allocate numeric locale");
        require(std::isfinite(strtod_l(result.raw.c_str(), nullptr, numeric_locale.get())),
                "Nonfinite JSON number");
        return result;
    }
};
Value parse(std::string_view text) {
    bounded(text);
    Parser parser{text};
    auto value = parser.value();
    parser.space();
    require(parser.pos == text.size(), "Trailing JSON content");
    return value;
}
std::string_view trim(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    line.remove_prefix(first);
    line.remove_suffix(line.size() - line.find_last_not_of(" \t") - 1);
    return line;
}
std::string assemble(const std::array<std::vector<std::string>, 10>& records, bool quantified) {
    std::string result = "{\"theory\":{";
    for (std::size_t i = 0; i < fields.size() - (quantified ? 0 : 1); ++i) {
        if (i) result += ',';
        result += "\"" + std::string(fields[i]) + "\":[";
        for (std::size_t j = 0; j < records[i].size(); ++j) {
            if (j) result += ',';
            result += records[i][j];
        }
        result += ']';
    }
    result += "},\"goals\":[";
    for (std::size_t j = 0; j < records.back().size(); ++j) {
        if (j) result += ',';
        result += records.back()[j];
    }
    return result + "]}";
}
void payload(std::size_t role, const Value& value) {
    require(value.kind == (role == 0 ? 's' : role == roles.size() - 1 ? 'o' : 'a'),
            "Wrong role payload type");
}

std::string encode_impl(std::string_view input_json, bool quantified) {
    const auto root = parse(input_json);
    require(root.kind == 'o', "Expected profile object");
    std::string result = quantified ? "GAMMATHINK_DSL 2\n" : "GAMMATHINK_DSL 1\n";
    if (root.members.contains("kind")) {
        require(root.members.size() == 2 && root.members.contains("reason"),
                "Invalid decline fields");
        const auto& kind = root.members.at("kind");
        const auto& reason = root.members.at("reason");
        require(kind.kind == 's' && (kind.text == "clarify" || kind.text == "unsupported")
            && reason.kind == 's' && !reason.text.empty(), "Invalid decline");
        result += (kind.text == "clarify" ? "CLARIFY " : "UNSUPPORTED ") + reason.raw + "\n";
    } else {
        require(root.members.size() == 2 && root.members.contains("theory")
            && root.members.contains("goals"), "Expected exactly theory and goals");
        const auto& theory = root.members.at("theory");
        require(theory.kind == 'o' && theory.members.size() == fields.size() - (quantified ? 0 : 1),
                "Invalid theory fields");
        std::size_t count = 0;
        for (std::size_t i = 0; i < roles.size(); ++i) {
            if (!quantified && i == fields.size() - 1) continue;
            const auto goal = i == roles.size() - 1;
            require(goal || theory.members.contains(std::string(fields[i])),
                    "Missing theory field");
            const auto& collection = goal ? root.members.at("goals")
                : theory.members.at(std::string(fields[i]));
            require(collection.kind == 'a', "Expected collection array");
            for (const auto& item : collection.items) {
                require(++count <= max_records, "Record limit exceeded");
                payload(i, item);
                result += std::string(roles[i]) + " " + item.raw + "\n";
            }
        }
    }
    result += "END\n";
    bounded(result);
    return result;
}

std::string decode_impl(std::string_view input_dsl, bool quantified) {
    bounded(input_dsl);
    std::array<std::vector<std::string>, 10> records;
    bool header = false, ended = false;
    std::string decline;
    std::size_t count = 0;
    while (!input_dsl.empty()) {
        const auto newline = input_dsl.find('\n');
        const auto line = trim(input_dsl.substr(0, newline));
        input_dsl = newline == std::string_view::npos ? std::string_view{}
            : input_dsl.substr(newline + 1);
        if (line.empty()) continue;
        require(!ended, "Content after END");
        if (!header) {
            require(line == (quantified ? "GAMMATHINK_DSL 2" : "GAMMATHINK_DSL 1"),
                    "Missing or invalid header");
            header = true;
            continue;
        }
        if (line == "END") { ended = true; continue; }
        const auto separator = line.find(' ');
        require(separator != std::string_view::npos, "Expected role and ASCII space");
        const auto role = line.substr(0, separator);
        const auto item = parse(line.substr(separator + 1));
        require(++count <= max_records, "Record limit exceeded");
        if (role == "CLARIFY" || role == "UNSUPPORTED") {
            require(count == 1 && decline.empty(), "Mixed decline records");
            require(item.kind == 's' && !item.text.empty(), "Invalid decline reason");
            decline = "{\"kind\":\"" + std::string(role == "CLARIFY" ? "clarify" : "unsupported")
                + "\",\"reason\":" + item.raw + "}";
            continue;
        }
        require(decline.empty(), "Mixed decline records");
        std::size_t index = 0;
        while (index < roles.size() && roles[index] != role) ++index;
        require(index < roles.size() && (quantified || index != fields.size() - 1), "Unknown role");
        payload(index, item);
        records[index].push_back(item.raw);
    }
    require(header && ended, "Missing header or END");
    auto result = decline.empty() ? assemble(records, quantified) : decline;
    (void)parse(result);
    return result;
}
} // namespace

std::string encode(std::string_view input_json) { return encode_impl(input_json, false); }
std::string decode(std::string_view input_dsl) { return decode_impl(input_dsl, false); }
std::string encode_v2(std::string_view input_json) { return encode_impl(input_json, true); }
std::string decode_v2(std::string_view input_dsl) { return decode_impl(input_dsl, true); }
} // namespace gammathink::archive_dsl
