#include "task_grammar.hpp"
#include <src/llama-grammar.h>
#include <src/unicode.h>

#include <algorithm>
#include <iostream>

namespace {
using namespace axiom;
using namespace axiom::knowledge;
std::size_t checks = 0;
void check(bool value) {
    if (!value) throw std::runtime_error("Task grammar check " + std::to_string(checks + 1));
    ++checks;
}
bool accepts(const std::string& source, const std::string& candidate) {
    std::unique_ptr<llama_grammar, decltype(&llama_grammar_free_impl)> grammar(
        llama_grammar_init_impl(nullptr, source.c_str(), "root", false, nullptr, 0, nullptr, 0),
        llama_grammar_free_impl);
    if (!grammar) throw std::runtime_error("Task grammar failed to compile");
    auto& stacks = llama_grammar_get_stacks(grammar.get());
    for (const auto character : unicode_cpts_from_utf8(candidate)) {
        llama_grammar_accept(grammar.get(), character);
        if (stacks.empty()) return false;
    }
    return std::any_of(stacks.begin(), stacks.end(), [](const auto& stack) { return stack.empty(); });
}
std::string replace(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    if (position == std::string::npos) throw std::runtime_error("Missing grammar mutation target");
    value.replace(position, from.size(), to);
    return value;
}
}

int main() {
    try {
        ProblemContext host;
        host.unit_registry_id = "grammar-test"; host.unit_registry_version = "1";
        host.views = {make_evidence_view("question", PremiseBasis::Question, "Retrieve all objects.",
            "", "message:1", "synthetic-v1")};
        TaskPlanContext context(ProblemSourceCatalog(host), 0);
        const auto grammar = task_plan_grammar(context);
        const auto handle = "\"" + ProblemSourceCatalog::source_handle(0) + "\"";
        const auto support = "[" + handle + "]";
        const auto prefix = "GAMMATHINK_TASK_IR 1\nCONTEXT \"" + context.snapshot() + "\"\n";
        const auto plan = prefix + "ROOT \"root\"\n"
            + "ENTITY [\"q\",\"query\",[\"all\"]," + support + "]\n"
            + "ENTITY [\"out\",\"artifact\",null," + support + "]\n"
            + "GOAL [\"root\",[],[],[],[]," + support + "]\n"
            + "ACTION [\"retrieve\",\"root\",\"retrieve\",[\"q\"],[\"out\"]," + support + "]\nEND\n";
        check(accepts(grammar, plan));
        check(std::holds_alternative<AdmittedTaskPlan>(admit_task_plan(context, context.snapshot(), plan)));
        check(accepts(grammar, prefix + "CLARIFY [\"Which object?\"," + support + "]\nEND\n"));
        check(accepts(grammar, prefix + "UNSUPPORTED [\"Unrepresented construct\"," + support + "]\nEND"));
        check(accepts(grammar, prefix + "CLARIFY [\"\\u03c0\"," + support + "]\nEND\n"));
        check(!accepts(grammar, plan.substr(std::string("GAMMATHINK_TASK_IR 1\n").size())));
        check(!accepts(grammar, "```text\n" + plan + "```"));
        check(!accepts(grammar, replace(plan, context.snapshot(), "forged-context")));
        check(!accepts(grammar, replace(plan, "ROOT \"root\"", "ROOT root")));
        check(!accepts(grammar, replace(plan, "\"retrieve\",[\"q\"]", "\"invent\",[\"q\"]")));
        check(!accepts(grammar, replace(plan, "[],[],[],[],", "[],[],true,[],")));
        check(!accepts(grammar, replace(plan, handle, "\"not-a-source-handle\"")));
        check(!accepts(grammar, replace(plan, "[\"out\"]", "[\"out\",\"other\"]")));
        check(!accepts(grammar, replace(plan, "END\n", "")));
        check(!accepts(grammar, plan + "ENTITY [\"extra\",\"artifact\",null," + support + "]\n"));
        const auto dangling = replace(plan, "[\"q\"]", "[\"undeclared\"]");
        check(accepts(grammar, dangling));
        try {
            (void)admit_task_plan(context, context.snapshot(), dangling);
            throw std::runtime_error("Grammar bypassed native graph validation");
        } catch (const Error& error) { check(error.code == ErrorCode::Schema); }
        std::cout << checks << " task grammar checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
