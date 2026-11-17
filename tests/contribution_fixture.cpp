#include "contribution.hpp"
#include "publisher_feed.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    try {
        if (argc != 2 || !std::filesystem::create_directory(argv[1]))
            throw Error(ErrorCode::Schema, "Expected a new synthetic contribution fixture directory");
        const auto directory = std::filesystem::absolute(argv[1]);
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
        Journal journal(directory / "journal.db");
        Object source; source.kind = ObjectKind::Claim; source.visibility = Visibility::Public;
        source.label = "Synthetic publication source"; source.format = "synthetic.publication-cli.v1"; source.payload = {1};
        const auto premise = journal.put(source);
        source.label = "Synthetic publication conclusion"; source.links = {{Relationship::Requires, premise}};
        const auto root = journal.put(source);
        source.visibility = Visibility::Private; source.label = "Unrelated private fixture";
        const auto private_id = journal.put(source);
        const auto packet = prepare_contribution(journal, std::array{root}, directory / "packet", "synthetic-cli", "1");
        const auto seed = parse_publisher_key("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
        const auto wire = sign_publisher_feed({"synthetic", 1, {},
            {{"synthetic-cli", "1", packet.snapshot.root, {root}, {}}}}, seed);
        std::ofstream file(directory / "feed.json", std::ios::binary);
        file.write(reinterpret_cast<const char*>(wire.data()), static_cast<std::streamsize>(wire.size()));
        file.close();
        if (!file) throw Error(ErrorCode::Storage, "Cannot write synthetic signed feed");
        std::cout << "{\"root\":\"" << hex(root) << "\",\"snapshot\":\"" << hex(packet.snapshot.root)
            << "\",\"feed\":\"" << hex(ObjectId{digest(wire)}) << "\",\"publisher\":\""
            << publisher_key_text(publisher_key_from_seed(seed)) << "\",\"private\":\"" << hex(private_id) << "\"}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
