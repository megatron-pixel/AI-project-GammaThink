#include "remote_query.hpp"

#include <iostream>

int main(int argc, char** argv) {
    using namespace axiom::knowledge;
    if (argc != 3) return 2;
    try {
        Bytes body(remote_query_request_bytes + 1);
        std::cin.read(reinterpret_cast<char*>(body.data()), static_cast<std::streamsize>(body.size()));
        const auto received = std::cin.gcount();
        if (std::cin.bad() || received <= 0 || static_cast<std::size_t>(received) > remote_query_request_bytes)
            throw Error(ErrorCode::Budget, "Invalid synthetic query request size");
        body.resize(static_cast<std::size_t>(received));
        const auto request = decode_remote_query(body);
        if (request.root != parse_block_id(argv[2])) throw Error(ErrorCode::Integrity, "Fixture root changed");
        GksArchive archive(argv[1], request.root);
        const auto page = archive.query(request.query, request.after, request.limit);
        const auto response = encode_remote_query_page({identify_block(body).digest, page});
        std::cout.write(reinterpret_cast<const char*>(response.data()), static_cast<std::streamsize>(response.size()));
        return std::cout.good() ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
