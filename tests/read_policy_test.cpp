#include "read_http.hpp"
#include "loopback_http_fixture.hpp"

#include <iostream>
#include <source_location>

namespace {
using namespace axiom::knowledge;
using namespace axiom::test;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Read policy assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected read policy rejection");
}
ReadHttpOptions options(const HttpFixture& server) {
    ReadHttpOptions result;
    result.origin = server.origin();
    result.network = true;
    result.allow_insecure_loopback = true;
    return result;
}
}

int main() {
    try {
        HttpFixture server([](const auto&) { return HttpResponse{{'a', 'b', 'c'}}; });
        auto gate = std::make_shared<std::atomic<bool>>(false);
        auto configuration = options(server);
        configuration.network_gate = gate;
        ReadHttp held(configuration);
        rejects(ErrorCode::Unavailable, [&] { held.raw_get("/disabled", 16); });
        check(held.stats().requests == 0);
        gate->store(true);
        check(held.raw_get("/enabled", 16) == Bytes({'a', 'b', 'c'}));
        gate->store(false);
        rejects(ErrorCode::Unavailable, [&] { held.raw_get("/revoked", 16); });
        check(held.stats().requests == 1);

        auto requests = std::make_shared<ReadHttpBudget>(ReadHttpBudgetLimits{.requests = 1});
        configuration = options(server);
        configuration.budget = requests;
        ReadHttp first(configuration), new_view(configuration);
        check(first.raw_get("/first-client", 16).size() == 3);
        rejects(ErrorCode::Budget, [&] { new_view.raw_get("/new-view", 16); });
        check(requests->stats().requests == 1 && new_view.stats().requests == 0);

        auto bytes = std::make_shared<ReadHttpBudget>(ReadHttpBudgetLimits{.total_bytes = 5});
        configuration.budget = bytes;
        ReadHttp before(configuration), after(configuration);
        check(before.raw_get("/body-one", 16).size() == 3);
        rejects(ErrorCode::Budget, [&] { after.raw_get("/body-two", 16); });
        check(bytes->stats().requests == 2 && bytes->stats().accepted_bytes <= 5);

        auto queries = std::make_shared<ReadHttpBudget>(ReadHttpBudgetLimits{.query_bytes = 3});
        configuration.budget = queries;
        ReadHttp query_one(configuration), query_two(configuration);
        const Bytes body{'q', '1'};
        check(query_one.query_post("/query-one", body, 16).size() == 3);
        rejects(ErrorCode::Budget, [&] { query_two.query_post("/query-two", body, 16); });
        check(queries->stats().query_bytes == 2 && queries->stats().requests == 1);
        server.finish();
        check(server.requests().size() == 5);

        auto active_gate = std::make_shared<std::atomic<bool>>(true);
        HttpFixture revoking([active_gate](const auto&) {
            // The observed request is the exact signal; revocation precedes
            // response headers/body. No fixed delay or timing-based assertion.
            active_gate->store(false, std::memory_order_release);
            return HttpResponse{{'s', 't', 'o', 'p'}};
        });
        auto active = options(revoking);
        active.network_gate = active_gate;
        ReadHttp in_flight(active);
        rejects(ErrorCode::Unavailable, [&] { in_flight.raw_get("/revoke-during-transfer", 16); });
        check(in_flight.stats().requests == 1 && in_flight.stats().accepted_bytes == 0);
        revoking.finish();
        check(revoking.requests().size() == 1);

        auto failed_budget = std::make_shared<ReadHttpBudget>(ReadHttpBudgetLimits{.query_bytes = 2});
        HttpFixture missing([](const auto&) { return HttpResponse{{'n', 'o'}, 404}; });
        auto failed = options(missing);
        failed.budget = failed_budget;
        ReadHttp failed_client(failed), another_client(failed);
        rejects(ErrorCode::Unavailable, [&] { failed_client.query_post("/missing", body, 16); });
        rejects(ErrorCode::Budget, [&] { another_client.query_post("/not-retried", body, 16); });
        check(failed_budget->stats().requests == 1 && failed_budget->stats().query_bytes == 2);
        missing.finish();
        check(missing.requests().size() == 1);
        std::cout << "shared read policy passed: " << checks << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
