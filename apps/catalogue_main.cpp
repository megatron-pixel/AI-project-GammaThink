#include "publisher_catalogue.hpp"

#include <json-c/json.h>

#include <charconv>
#include <fstream>
#include <iostream>

namespace {
using namespace axiom::knowledge;
std::uint64_t number(std::string_view text) {
    std::uint64_t value = 0;
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (code != std::errc{} || end != text.data() + text.size() || text.empty())
        throw Error(ErrorCode::Schema, "Expected an unsigned decimal catalogue argument");
    return value;
}
Bytes feed_file(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) throw Error(ErrorCode::Missing, "Publisher feed file is absent");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024) throw Error(ErrorCode::Budget, "Publisher feed file exceeds byte budget");
    Bytes bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file || file.peek() != std::char_traits<char>::eof())
        throw Error(ErrorCode::Storage, "Publisher feed file changed during read");
    return bytes;
}
void text(json_object* output, const char* key, std::string_view value) {
    json_object_object_add(output, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void mount(json_object* output, const MountState& value) {
    text(output, "alias", value.alias);
    json_object_object_add(output, "generation", json_object_new_uint64(value.generation));
    json_object_object_add(output, "mounted", json_object_new_boolean(value.binding.has_value()));
    if (value.binding) {
        text(output, "name", value.binding->archive.name);
        text(output, "revision", value.binding->archive.revision);
        text(output, "root", hex(value.binding->archive.root));
    }
}
}
int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    try {
        if (argc < 3) throw Error(ErrorCode::Schema,
            "Usage: knowledge-catalogue discover DB PINNED_KEY FEED_FILE | list DB AFTER_OR_DASH | "
            "show DB PINNED_KEY FEED_ID | accept DB PINNED_KEY FEED_ID OFFER ALIAS DIRECTORY EXPECTED_GENERATION | "
            "resolve DB ALIAS | unmount DB ALIAS EXPECTED_GENERATION");
        const auto command = std::string_view(argv[1]);
        if (command != "discover" && !std::filesystem::is_regular_file(argv[2]))
            throw Error(ErrorCode::Missing, "Publisher catalogue database is absent");
        auto output = std::unique_ptr<json_object, decltype(&json_object_put)>(json_object_new_object(), json_object_put);
        // Verify untrusted input before even creating a new catalogue.
        std::optional<VerifiedPublisherFeed> incoming;
        if (command == "discover" && argc == 5)
            incoming = verify_publisher_feed(feed_file(argv[4]), parse_publisher_key(argv[3]));
        if (command == "discover" && !incoming) throw Error(ErrorCode::Schema, "Expected a pinned key and signed feed file");
        PublisherCatalogue catalogue(argv[2]);
        if (incoming) {
            catalogue.discover(*incoming);
            text(output.get(), "feed", hex(incoming->id()));
            json_object_object_add(output.get(), "identity_verified", json_object_new_boolean(true));
            json_object_object_add(output.get(), "accepted", json_object_new_boolean(false));
        } else if (command == "list" && argc == 4) {
            const auto cursor = std::string_view(argv[3]);
            const auto page = catalogue.feeds(cursor == "-" ? std::optional<PublisherFeedId>{} :
                PublisherFeedId{parse_id(cursor).digest});
            auto* feeds = json_object_new_array();
            for (const auto& id : page.feeds) json_object_array_add(feeds, json_object_new_string(hex(id).c_str()));
            json_object_object_add(output.get(), "feeds", feeds);
            json_object_object_add(output.get(), "complete", json_object_new_boolean(page.complete));
            json_object_object_add(output.get(), "next", page.next ? json_object_new_string(hex(*page.next).c_str()) : nullptr);
        } else if (command == "show" && argc == 5) {
            const auto feed = catalogue.get({parse_id(argv[4]).digest}, parse_publisher_key(argv[3]));
            std::cout.write(reinterpret_cast<const char*>(feed.wire().data()), static_cast<std::streamsize>(feed.wire().size()));
            std::cout << '\n'; return 0;
        } else if (command == "accept" && argc == 9) {
            mount(output.get(), catalogue.accept_local(argv[6], {parse_id(argv[4]).digest}, number(argv[5]),
                parse_publisher_key(argv[3]), argv[7], number(argv[8])));
        } else if (command == "resolve" && argc == 4) {
            mount(output.get(), catalogue.resolve(argv[3]));
        } else if (command == "unmount" && argc == 5) {
            mount(output.get(), catalogue.unmount(argv[3], number(argv[4])));
        } else throw Error(ErrorCode::Schema, "Invalid publisher catalogue command or argument count");
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
