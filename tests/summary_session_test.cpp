#include "task_session.hpp"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdlib.h>
#include <unistd.h>

namespace {
using namespace axiom;
using namespace axiom::context;
using namespace axiom::knowledge;
using namespace axiom::neural;
using namespace axiom::runtime;
std::size_t checks = 0;
void check(bool value, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!value) throw std::runtime_error("Summary session assertion at " + std::to_string(where.line()));
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto pattern = (std::filesystem::temp_directory_path() / "summary-session-XXXXXX").string();
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create summary session directory");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "journal.db");
        NativeModelManager model;
        TaskSession controller(memory, tasks, journal, model);
        ModelSpec missing;
        missing.id = "summary-integration-missing-model";
        missing.path = directory.path / "absent.gguf";
        missing.expected_sha256[0] = 1;
        ProblemContext host;
        host.unit_registry_id = "summary-session";
        host.unit_registry_version = "1";
        auto attempt = [&](std::string_view session, std::size_t interval) {
            TaskInterpretationOptions options;
            options.summary_interval = interval;
            ++checks;
            try { controller.interpret_plan(session, missing, host, 0, options); }
            catch (const Error& error) {
                // The exact model-open failure proves that context maintenance
                // ran before model loading, without neural inference.
                if (error.code == ErrorCode::Storage &&
                    std::string_view(error.what()) == "Cannot open local model") return;
                throw;
            }
            throw std::runtime_error("Expected missing model rejection");
        };
        memory.open_session("automatic", "project");
        const auto first = memory.append("automatic", Role::User, "1", "Keep the original design.");
        const auto second = memory.append("automatic", Role::User, "2", "Continue the task.");
        attempt("automatic", 2);
        const auto saved = memory.summary("automatic");
        check(saved && saved->version == 1);
        check(saved->first_message == first.id && saved->last_message == second.id);
        check(memory.get("automatic", first.id) == first);
        check(memory.get("automatic", second.id) == second);
        check(model.state() == ModelState::Unloaded);
        check(!tasks.active("automatic") && journal.objects().objects.empty());
        attempt("automatic", 2);
        check(memory.summary("automatic") == saved);
        const auto third = memory.append("automatic", Role::User, "3", "A newer request.");
        attempt("automatic", 2);
        check(memory.summary("automatic") == saved);
        const auto fourth = memory.append("automatic", Role::User, "4", "Preserve correction precedence.");
        attempt("automatic", 2);
        const auto updated = memory.summary("automatic");
        check(updated && updated->version == 2 && updated->last_message == fourth.id);
        check(memory.summary("automatic", 1) == saved);
        check(memory.get("automatic", third.id) == third);
        memory.open_session("manual", "project");
        memory.append("manual", Role::User, "1", "Manual summary profile.");
        attempt("manual", 0);
        check(!memory.summary("manual"));
        check(model.state() == ModelState::Unloaded);
        check(!tasks.active("manual") && journal.objects().objects.empty());
        std::cout << "Summary session: " << checks << " checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
