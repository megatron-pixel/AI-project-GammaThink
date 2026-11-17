#include "problem_sources.hpp"
#include "problem_semantic.hpp"

#include <json-c/json.h>

#include <charconv>
#include <memory>
#include <stdexcept>

namespace axiom {
namespace {
constexpr std::size_t max_pieces = 16384, max_regions = 4096, max_input = 1048576;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

// Return the next Unicode scalar boundary, rejecting overlong encodings,
// surrogates, NUL, and scalars beyond Unicode. No linguistic segmentation.
std::size_t scalar_end(std::string_view text, std::size_t at) {
    const auto first = static_cast<unsigned char>(text[at]);
    if (first > 0 && first < 0x80) return at + 1;
    const unsigned count = first >= 0xc2 && first <= 0xdf ? 2
        : first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
    require(count && count <= text.size() - at, "Invalid catalog UTF-8");
    std::uint32_t scalar = first & ((1U << (7 - count)) - 1);
    for (unsigned i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[at + i]);
        require((next & 0xc0) == 0x80, "Invalid catalog UTF-8");
        scalar = (scalar << 6) | (next & 0x3f);
    }
    require(scalar >= (count == 2 ? 0x80U : count == 3 ? 0x800U : 0x10000U)
        && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff),
        "Invalid catalog UTF-8");
    return at + count;
}

unsigned lexical_class(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_') return 1;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 2;
    return 0;
}

std::uint32_t handle_index(std::string_view handle, char prefix) {
    require(handle.size() >= 2 && handle.front() == prefix
        && (handle.size() == 2 || handle[1] != '0'), "Invalid catalog handle");
    std::uint32_t value = 0;
    const auto result = std::from_chars(handle.data() + 1, handle.data() + handle.size(), value);
    require(result.ec == std::errc{} && result.ptr == handle.data() + handle.size(),
        "Invalid catalog handle");
    return value;
}

json_object* text(std::string_view value) {
    return json_object_new_string_len(value.data(), static_cast<int>(value.size()));
}
}

ProblemSourceCatalog::ProblemSourceCatalog(ProblemContext context, ProblemLimits limits)
    : context_(std::move(context)), limits_(limits) {
    require(limits.work > 0 && limits.work <= 4000000 && limits.steps > 0 && limits.steps <= 256,
        "Invalid catalog limits");
    const auto serialized = serialize_problem_context(context_);
    snapshot_ = evidence_digest("source-handles-v1\n" + serialized + "\n"
        + std::to_string(limits.work) + "\n" + std::to_string(limits.steps));

    std::size_t pieces = 0, regions = 0;
    for (std::size_t view_index = 0; view_index < context_.views.size(); ++view_index) {
        const auto& view = context_.views[view_index];
        // Validate even text outside structural locators; it remains in input().
        for (std::size_t i = 0; i < view.text.size();) i = scalar_end(view.text, i);
        const auto boundary = [&](std::size_t at) {
            return at == view.text.size()
                || (static_cast<unsigned char>(view.text[at]) & 0xc0) != 0x80;
        };
        const auto add_region = [&](std::uint32_t start, std::uint32_t length, std::uint32_t locator) {
            require(length > 0 && start <= view.text.size() && length <= view.text.size() - start
                && boundary(start) && boundary(start + length), "Invalid catalog region");
            require(regions < max_regions, "Catalog region limit exceeded");
            const auto region = static_cast<std::uint32_t>(regions++);
            entries_.push_back({{static_cast<std::uint32_t>(view_index), start, length, locator}, region, true});
            const auto end = static_cast<std::size_t>(start) + length;
            for (std::size_t offset = start; offset < end;) {
                auto next = scalar_end(view.text, offset);
                const auto kind = lexical_class(static_cast<unsigned char>(view.text[offset]));
                if (kind) {
                    while (next < end && lexical_class(static_cast<unsigned char>(view.text[next])) == kind) ++next;
                }
                require(next <= end, "Catalog piece crosses region");
                require(pieces++ < max_pieces, "Catalog piece limit exceeded");
                entries_.push_back({{static_cast<std::uint32_t>(view_index), static_cast<std::uint32_t>(offset),
                    static_cast<std::uint32_t>(next - offset), locator}, region, false});
                offset = next;
            }
        };
        if (view.locators.empty()) add_region(0, static_cast<std::uint32_t>(view.text.size()), no_step);
        else {
            for (std::size_t i = 0; i < view.locators.size(); ++i) {
                const auto& locator = view.locators[i];
                add_region(locator.offset, locator.length, static_cast<std::uint32_t>(i));
            }
        }
    }

    Json root(json_object_new_object(), json_object_put);
    json_object_object_add(root.get(), "version", text("source-handles-v1"));
    json_object_object_add(root.get(), "snapshot", text(snapshot_));
    json_object_object_add(root.get(), "context", json_tokener_parse(serialized.c_str()));
    auto units = json_object_new_array();
    json_object_object_add(root.get(), "unit_refs", units);
    for (std::size_t i = 0; i < context_.units.size(); ++i) {
        auto tuple = json_object_new_array();
        json_object_array_add(tuple, text("u" + std::to_string(i)));
        json_object_array_add(tuple, json_object_new_int64(i));
        json_object_array_add(units, tuple);
    }
    auto sources = json_object_new_array();
    json_object_object_add(root.get(), "sources", sources);
    std::size_t rendered = std::string_view(
        json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN)).size();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const auto& entry = entries_[i];
        const auto& span = entry.span;
        Json tuple(json_object_new_array(), json_object_put);
        json_object_array_add(tuple.get(), text(source_handle(i)));
        json_object_array_add(tuple.get(), json_object_new_int64(span.view));
        json_object_array_add(tuple.get(), span.locator == no_step ? nullptr : json_object_new_int64(span.locator));
        json_object_array_add(tuple.get(), text(entry.whole_region ? "region" : "piece"));
        json_object_array_add(tuple.get(), text(std::string_view(context_.views[span.view].text).substr(span.offset, span.length)));
        rendered += std::string_view(
            json_object_to_json_string_ext(tuple.get(), JSON_C_TO_STRING_PLAIN)).size() + (i ? 1 : 0);
        require(rendered <= max_input, "Catalog rendered-byte limit exceeded");
        json_object_array_add(sources, tuple.release());
    }
    input_ = json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
    require(input_.size() <= max_input, "Catalog rendered-byte limit exceeded");
}

std::string ProblemSourceCatalog::source_handle(std::size_t index) {
    return "s" + std::to_string(index);
}

EvidenceSpan ProblemSourceCatalog::resolve(std::string_view snapshot, std::string_view first,
                                          std::optional<std::string_view> last) const {
    require(snapshot == snapshot_, "Stale catalog snapshot");
    const auto first_index = handle_index(first, 's');
    require(first_index < entries_.size(), "Unknown source handle");
    const auto& begin = entries_[first_index];
    if (!last) return begin.span;
    const auto last_index = handle_index(*last, 's');
    require(last_index < entries_.size(), "Unknown source handle");
    const auto& end = entries_[last_index];
    require(!begin.whole_region && !end.whole_region && begin.region == end.region
        && first_index <= last_index, "Invalid source handle range");
    auto span = begin.span;
    span.length = end.span.offset + end.span.length - span.offset;
    return span;
}

std::uint32_t ProblemSourceCatalog::unit(std::string_view snapshot, std::string_view handle) const {
    require(snapshot == snapshot_, "Stale catalog snapshot");
    const auto index = handle_index(handle, 'u');
    require(index < context_.units.size(), "Unknown unit handle");
    return index;
}

} // namespace axiom
