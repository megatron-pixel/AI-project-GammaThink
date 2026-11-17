#include "contribution_publish.hpp"

#include <json-c/json.h>

#include <fstream>
#include <iostream>

namespace {
using namespace axiom::knowledge;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
void text(json_object* output, const char* key, std::string_view value) {
    json_object_object_add(output, key, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void ids(json_object* output, const char* key, std::span<const ObjectId> values) {
    auto* array = json_object_new_array();
    for (const auto& id : values) json_object_array_add(array, json_object_new_string(hex(id).c_str()));
    json_object_object_add(output, key, array);
}
std::vector<ObjectId> roots(char** argv, int start, int end) {
    if (end <= start || end - start > static_cast<int>(maximum_items))
        throw Error(ErrorCode::Budget, "Invalid contribution root count");
    std::vector<ObjectId> result;
    for (int i = start; i < end; ++i) result.push_back(parse_id(argv[i]));
    return result;
}
Json receipt(const PublicationReceipt& value) {
    Json output(json_object_new_object(), json_object_put);
    text(output.get(), "root_cid", cid_text(block_cid(value.root)));
    json_object_object_add(output.get(), "root_stored", json_object_new_boolean(value.root_stored));
    json_object_object_add(output.get(), "attempted_requests", json_object_new_uint64(value.attempted_requests));
    json_object_object_add(output.get(), "confirmed_payload_bytes", json_object_new_uint64(value.confirmed_payload_bytes));
    json_object_object_add(output.get(), "uploaded_http_body_bytes", json_object_new_uint64(value.uploaded_http_body_bytes));
    auto* confirmed = json_object_new_array();
    for (const auto& id : value.confirmed) json_object_array_add(confirmed, json_object_new_string(cid_text(id).c_str()));
    json_object_object_add(output.get(), "confirmed", confirmed); return output;
}
Bytes feed_file(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) throw Error(ErrorCode::Missing, "Publisher feed file is absent");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (size <= 0 || size > 1024 * 1024) throw Error(ErrorCode::Budget, "Publisher feed file exceeds byte budget");
    Bytes value(static_cast<std::size_t>(size)); file.seekg(0);
    file.read(reinterpret_cast<char*>(value.data()), static_cast<std::streamsize>(value.size()));
    if (!file || file.peek() != std::char_traits<char>::eof())
        throw Error(ErrorCode::Storage, "Publisher feed file changed during read");
    return value;
}
}
int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    std::unique_ptr<ContributionPublisher> publisher;
    try {
        Json output(json_object_new_object(), json_object_put);
        const auto command = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if ((command == "prepare" && argc >= 7) || (command == "review" && argc >= 5)) {
            PreparedContribution packet;
            if (command == "prepare") {
                if (!std::filesystem::is_regular_file(argv[2]))
                    throw Error(ErrorCode::Missing, "Contribution preparation requires an existing journal");
                Journal journal(argv[2]);
                packet = prepare_contribution(journal, roots(argv, 6, argc), argv[3], argv[4], argv[5]);
            } else packet = review_contribution(argv[2], BlockId{parse_id(argv[3]).digest}, roots(argv, 4, argc));
            const auto plan = inspect_publication(packet);
            text(output.get(), "directory", std::filesystem::absolute(packet.directory).string());
            text(output.get(), "root", hex(plan.root));
            text(output.get(), "root_cid", cid_text(block_cid(plan.root)));
            ids(output.get(), "roots", packet.roots); ids(output.get(), "objects", packet.objects);
            json_object_object_add(output.get(), "blocks", json_object_new_uint64(plan.blocks.size()));
            json_object_object_add(output.get(), "payload_bytes", json_object_new_uint64(plan.payload_bytes));
            json_object_object_add(output.get(), "published", json_object_new_boolean(false));
        } else if (command == "publish" && argc >= 9) {
            if (std::string_view(argv[argc - 4]) != "--approve-root" || std::string_view(argv[argc - 2]) != "--node"
                || parse_id(argv[argc - 3]).digest != parse_id(argv[3]).digest)
                throw Error(ErrorCode::Permission, "Publication requires --approve-root EXACT_ROOT --node LOOPBACK_ORIGIN");
            auto packet = review_contribution(argv[2], BlockId{parse_id(argv[3]).digest}, roots(argv, 4, argc - 4));
            publisher = std::make_unique<ContributionPublisher>(argv[argc - 1]);
            auto approval = publisher->approve(packet);
            output = receipt(publisher->publish(approval));
        } else if (command == "publish-feed" && argc == 8) {
            const auto feed = verify_publisher_feed(feed_file(argv[2]), parse_publisher_key(argv[3]));
            if (std::string_view(argv[4]) != "--approve-feed" || std::string_view(argv[6]) != "--node"
                || parse_id(argv[5]).digest != feed.id().digest)
                throw Error(ErrorCode::Permission, "Feed publication requires --approve-feed EXACT_FEED_ID --node LOOPBACK_ORIGIN");
            publisher = std::make_unique<ContributionPublisher>(argv[7]);
            auto approval = publisher->approve_feed(feed);
            output = receipt(publisher->publish(approval));
        } else throw Error(ErrorCode::Schema,
            "Usage: knowledge-contribute prepare JOURNAL DIRECTORY NAME REVISION ROOT_ID... | "
            "review DIRECTORY ROOT_HASH ROOT_ID... | publish DIRECTORY ROOT_HASH ROOT_ID... "
            "--approve-root EXACT_ROOT --node LOOPBACK_ORIGIN | "
            "publish-feed FEED_FILE PINNED_KEY --approve-feed EXACT_FEED_ID --node LOOPBACK_ORIGIN");
        std::cout << json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (publisher) {
            const auto partial = receipt(publisher->last_receipt());
            std::cerr << "publication_receipt " << json_object_to_json_string_ext(partial.get(), JSON_C_TO_STRING_PLAIN) << '\n';
        }
        return 1;
    }
}
