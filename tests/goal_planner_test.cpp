#include "goal_planner.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace axiom::knowledge;
std::size_t assertions = 0;
void check(bool value) {
    ++assertions;
    if (!value) throw std::runtime_error("Goal planner assertion " + std::to_string(assertions));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++assertions;
    try { operation(); } catch (const Error& error) {
        if (error.code == code) return;
        throw;
    }
    throw std::runtime_error("Expected goal planner rejection");
}
GoalIR goal(std::string id, std::vector<std::vector<std::string>> methods = {}) {
    GoalIR result;
    result.id = std::move(id);
    result.methods = std::move(methods);
    return result;
}
std::size_t index(const GoalGraph& graph, std::string_view name) {
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes[i].id == name) return i;
    }
    throw std::runtime_error("Missing goal in test graph");
}
GoalOutcome done(GoalConditions observed = {}) {
    return {true, true, std::move(observed), {}};
}
}

int main() {
    try {
        auto application = goal("build_application", {{"authentication", "database"}});
        application.constraints = {{"language", std::string("cpp")}, {"offline_capable", true}};
        application.success_conditions = {{"tests_pass", true}};
        const std::vector<GoalIR> definitions{
            application, goal("authentication", {{"local_auth"}, {"remote_auth"}}),
            goal("database"), goal("local_auth"), goal("remote_auth")
        };
        const GoalConditions environment{{"language", std::string("cpp")}, {"offline_capable", true}};
        const auto graph = decompose_goals(application.id, definitions);
        check(graph.nodes.size() == 5 && graph.cycle_edges.empty());
        const auto root = graph.root, auth = index(graph, "authentication"), db = index(graph, "database");
        check(graph.nodes[root].methods == std::vector<std::vector<std::size_t>>{{auth, db}});
        check(graph.nodes[auth].methods.size() == 2);
        auto evaluated = evaluate_goals(graph, {}, environment);
        check(evaluated.states[root] == GoalState::Pending && !evaluated.root_satisfied);
        check(evaluated.states[index(graph, "local_auth")] == GoalState::Ready);
        std::map<std::string, GoalOutcome> outcomes{{"local_auth", done()}, {"authentication", done()}};
        evaluated = evaluate_goals(graph, outcomes, environment);
        check(evaluated.states[auth] == GoalState::Satisfied);
        check(evaluated.states[index(graph, "remote_auth")] == GoalState::Ready);
        check(evaluated.states[root] == GoalState::Pending);
        outcomes["database"] = done();
        evaluated = evaluate_goals(graph, outcomes, environment);
        check(evaluated.states[root] == GoalState::Ready && !evaluated.root_satisfied);
        outcomes["build_application"] = done();
        check(evaluate_goals(graph, outcomes, environment).states[root] == GoalState::UnresolvedOutcome);
        outcomes["build_application"] = done({{"tests_pass", false}});
        check(evaluate_goals(graph, outcomes, environment).states[root] == GoalState::Failed);
        outcomes["build_application"] = done({{"tests_pass", true}});
        check(evaluate_goals(graph, outcomes, environment).root_satisfied);
        outcomes["build_application"].verified = false;
        check(evaluate_goals(graph, outcomes, environment).states[root] == GoalState::UnresolvedOutcome);
        outcomes["build_application"] = done({{"tests_pass", true}});
        outcomes["build_application"].succeeded = false;
        check(evaluate_goals(graph, outcomes, environment).states[root] == GoalState::Failed);
        check(evaluate_goals(graph, outcomes, {}).states[root] == GoalState::ConstraintBlocked);
        auto wrong_type = environment;
        wrong_type["offline_capable"] = std::int64_t{1};
        check(evaluate_goals(graph, outcomes, wrong_type).states[root] == GoalState::ConstraintBlocked);

        const ObjectId assumption{digest(Bytes{1, 2, 3})};
        auto conditional = goal("conditional");
        conditional.assumptions = {assumption};
        conditional.formal_request = assumption;
        const auto conditioned = decompose_goals("conditional", {conditional});
        check(conditioned.nodes[0].definition->formal_request == assumption);
        check(evaluate_goals(conditioned, {{"conditional", done()}}, {}).states[0] ==
              GoalState::AssumptionBlocked);
        check(evaluate_goals(conditioned, {{"conditional", done()}}, {}, {assumption}).root_satisfied);

        const auto absent = decompose_goals("root", {goal("root", {{"unavailable"}})});
        check(!absent.nodes[index(absent, "unavailable")].definition);
        const auto missing = evaluate_goals(absent, {{"root", done()}}, {});
        check(!missing.root_satisfied && missing.states[0] == GoalState::Pending);
        check(missing.states[index(absent, "unavailable")] == GoalState::MissingRequirement);
        const auto missing_root = decompose_goals("absent", {});
        check(evaluate_goals(missing_root, {{"absent", done()}}, {}).states[0] ==
              GoalState::MissingRequirement);
        const auto shared = decompose_goals("root", {
            goal("root", {{"a", "b"}}), goal("a", {{"shared", "shared"}}),
            goal("b", {{"shared"}}), goal("shared")});
        check(shared.nodes.size() == 4);
        check(shared.nodes[index(shared, "a")].methods[0].size() == 2);
        const auto cyclic = decompose_goals("a", {goal("a", {{"b"}}), goal("b", {{"a"}})});
        check(!cyclic.cycle_edges.empty());
        const auto ungrounded = evaluate_goals(cyclic, {{"a", done()}, {"b", done()}}, {});
        check(!ungrounded.root_satisfied && ungrounded.states[0] == GoalState::Pending);
        const auto escape = decompose_goals("a", {
            goal("a", {{"b"}, {"leaf"}}), goal("b", {{"a"}}), goal("leaf")});
        check(!escape.cycle_edges.empty());
        check(evaluate_goals(escape, {{"a", done()}, {"leaf", done()}}, {}).root_satisfied);

        rejects(ErrorCode::Schema, [&] { decompose_goals("a", {goal("a"), goal("a")}); });
        rejects(ErrorCode::Budget, [&] { decompose_goals("a", {goal("a", {{"b"}})}, {1, 2}); });
        rejects(ErrorCode::Budget, [&] { decompose_goals("a", {goal("a", {{"b", "c"}})}, {3, 1}); });
        auto forged = graph;
        forged.nodes[root].methods.clear();
        rejects(ErrorCode::Schema, [&] { evaluate_goals(forged, outcomes, environment); });
        forged = graph;
        forged.nodes[root].methods[0].pop_back();
        rejects(ErrorCode::Schema, [&] { evaluate_goals(forged, outcomes, environment); });
        forged = graph;
        forged.nodes[root].methods[0][0] = root;
        rejects(ErrorCode::Schema, [&] { evaluate_goals(forged, outcomes, environment); });
        forged = graph;
        forged.nodes[root].methods[0][0] = graph.nodes.size();
        rejects(ErrorCode::Schema, [&] { evaluate_goals(forged, outcomes, environment); });
        rejects(ErrorCode::Schema, [&] { evaluate_goals({}, {}, {}); });
        for (const auto& [comparison, satisfied] : {
             std::pair{GoalComparison::Equal, false}, {GoalComparison::NotEqual, true},
             {GoalComparison::Less, true}, {GoalComparison::LessEqual, true},
             {GoalComparison::Greater, false}, {GoalComparison::GreaterEqual, false}}) {
            auto typed = goal("typed");
            typed.requirements = {{"count", comparison, std::int64_t{3}}};
            const auto plan = decompose_goals("typed", {typed});
            const auto actual = evaluate_goals(plan, {{"typed", done()}}, {{"count", std::int64_t{2}}});
            check(actual.root_satisfied == satisfied);
            check(evaluate_goals(plan, {{"typed", done()}}, {{"count", std::string("2")}}).states[0]
                == GoalState::ConstraintBlocked);
        }
        auto bounded = goal("bounded");
        bounded.requirements = {{"ram_bytes", GoalComparison::Less, std::int64_t{3000000000}},
            {"ram_bytes", GoalComparison::GreaterEqual, std::int64_t{0}},
            {"network", GoalComparison::NotEqual, true}};
        bounded.success_requirements = {{"passed", GoalComparison::GreaterEqual, std::int64_t{1}},
            {"errors", GoalComparison::Equal, std::int64_t{0}}};
        const auto budgeted = decompose_goals("bounded", {bounded});
        const GoalConditions allowed{{"ram_bytes", std::int64_t{2999999999}}, {"network", false}};
        check(evaluate_goals(budgeted, {{"bounded", done()}}, allowed).states[0] == GoalState::UnresolvedOutcome);
        check(evaluate_goals(budgeted, {{"bounded", done({{"passed", std::int64_t{1}}, {"errors", std::int64_t{0}}})}}, allowed)
            .root_satisfied);
        auto too_large = allowed; too_large["ram_bytes"] = std::int64_t{3000000000};
        check(evaluate_goals(budgeted, {}, too_large).states[0] == GoalState::ConstraintBlocked);
        check(evaluate_goals(budgeted, {}, {}).states[0] == GoalState::ConstraintBlocked);
        check(evaluate_goals(budgeted, {{"bounded", done({{"passed", std::string("1")}, {"errors", std::int64_t{0}}})}}, allowed)
            .states[0] == GoalState::Failed);
        bounded.requirements.push_back({"ram_bytes", GoalComparison::Less, std::int64_t{4000000000}});
        check(evaluate_goals(decompose_goals("bounded", {bounded}), {}, too_large).states[0] == GoalState::ConstraintBlocked);
        bounded.requirements = {{"network", GoalComparison::Less, true}};
        rejects(ErrorCode::Schema, [&] { decompose_goals("bounded", {bounded}); });
        bounded.requirements = {{"count", static_cast<GoalComparison>(99), std::int64_t{0}}};
        rejects(ErrorCode::Schema, [&] { decompose_goals("bounded", {bounded}); });
        bounded.requirements = {{"minimum", GoalComparison::GreaterEqual, std::int64_t{INT64_MIN}},
            {"maximum", GoalComparison::LessEqual, std::int64_t{INT64_MAX}}};
        bounded.success_requirements.clear();
        check(evaluate_goals(decompose_goals("bounded", {bounded}), {{"bounded", done()}},
            {{"minimum", std::int64_t{INT64_MIN}}, {"maximum", std::int64_t{INT64_MAX}}}).root_satisfied);
        auto parent = goal("parent", {{"leaf"}});
        auto leaf = goal("leaf");
        leaf.requirements = {{"ram_bytes", GoalComparison::Less, std::int64_t{4000000000}}};
        const auto global_plan = decompose_goals("parent", {parent, leaf});
        const std::array active{GoalPredicate{"ram_bytes", GoalComparison::Less, std::int64_t{3000000000}}};
        const auto globally_blocked = evaluate_goals(global_plan, {},
            {{"ram_bytes", std::int64_t{3500000000}}}, {}, active);
        check(std::all_of(globally_blocked.states.begin(), globally_blocked.states.end(),
            [](GoalState state) { return state == GoalState::ConstraintBlocked; }));
        check(evaluate_goals(global_plan, {}, {}, {}, active).states[0] == GoalState::ConstraintBlocked);
        std::cout << "goal planner tests passed: " << assertions << " deterministic assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
