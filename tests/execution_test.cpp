#include "execution.hpp"

#include <fstream>
#include <iostream>
#include <json-c/json.h>
#include <source_location>
#include <unistd.h>

namespace {
using namespace axiom::execution;
using namespace axiom::knowledge;
using namespace axiom::context;
std::size_t checks = 0;
void check(bool condition, const std::source_location where = std::source_location::current()) {
    ++checks;
    if (!condition) throw std::runtime_error("Execution assertion at " + std::to_string(where.line()));
}
template<class F> void rejects(ErrorCode code, F&& operation) {
    ++checks;
    try { operation(); }
    catch (const Error& error) { if (error.code == code) return; throw; }
    throw std::runtime_error("Expected execution rejection");
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string pattern = "/private/tmp/execution-test-XXXXXX";
        const auto made = mkdtemp(pattern.data());
        if (!made) throw std::runtime_error("Cannot create execution fixture");
        path = made;
    }
    ~Directory() { std::filesystem::remove_all(path); }
};
std::string field(Journal& journal, const ObjectId& id, const char* name) {
    const auto object = journal.get(id);
    if (!object) throw std::runtime_error("Missing execution receipt");
    const std::string text(object->payload.begin(), object->payload.end());
    std::unique_ptr<json_object, decltype(&json_object_put)> root(
        json_tokener_parse(text.c_str()), json_object_put);
    if (!root) throw std::runtime_error("Invalid execution receipt JSON");
    auto* value = json_object_object_get(root.get(), name);
    if (!value || json_object_get_type(value) != json_type_string)
        throw std::runtime_error("Invalid execution receipt field");
    return json_object_get_string(value);
}
std::uint64_t number(Journal& journal, const ObjectId& id, const char* name) {
    const auto object = journal.get(id);
    if (!object) throw std::runtime_error("Missing execution receipt");
    const std::string text(object->payload.begin(), object->payload.end());
    std::unique_ptr<json_object, decltype(&json_object_put)> root(
        json_tokener_parse(text.c_str()), json_object_put);
    if (!root) throw std::runtime_error("Invalid execution receipt JSON");
    auto* value = json_object_object_get(root.get(), name);
    if (!value || json_object_get_type(value) != json_type_int)
        throw std::runtime_error("Invalid numeric execution receipt field");
    return json_object_get_uint64(value);
}
void resource_receipt(Journal& journal, const ObjectId& id) {
    check(field(journal, id, "resident_scope") == "controller-and-execution-process-group");
    check(number(journal, id, "resident_sample_count") > 0);
    const auto group = number(journal, id, "sampled_peak_resident_bytes");
    const auto controller = number(journal, id, "sampled_peak_controller_resident_bytes");
    const auto application = number(journal, id, "sampled_peak_application_resident_bytes");
    check(controller > 0);
    check(application > group && application >= controller);
    // Maxima from different instants must not be added into a fictitious peak.
    check(application <= group + controller);
}
}

int main() {
    try {
        Directory directory;
        ConversationMemory memory(directory.path / "conversation.db");
        SessionTasks tasks(directory.path / "tasks.db");
        Journal journal(directory.path / "knowledge.db");
        memory.open_session("a", "test");
        const auto message = memory.append("a", Role::User, "1", "Compile and test the proposed program locally");
        Object task;
        task.kind = ObjectKind::Goal;
        task.format = "synthetic.execution-task-fixture";
        task.payload = Bytes{1};
        const auto task_id = journal.put(task);
        tasks.begin(memory, "a", message.id, task_id, 0);
        Toolchain tools{"/Library/Developer/CommandLineTools/usr/bin/clang++",
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
            std::filesystem::canonical("build/execution-helper")};
        axiom::neural::NativeModelManager model;
        Executor executor(memory, tasks, journal, model, tools);
        rejects(ErrorCode::Permission, [&] { Executor second(memory, tasks, journal, model, tools); });
        rejects(ErrorCode::Permission, [&] { executor.permit("a", 0, {{"", ""}}); });
        rejects(ErrorCode::Budget, [&] { executor.permit("a", 1, {}); });
        auto permit = executor.permit("a", 1, {{"", "ok\n"}});
        const auto failed = executor.attempt(permit, "this is not C++");
        if (failed.diagnostics.empty()) {
            const auto receipt = journal.get(failed.receipt);
            std::cerr << std::string(receipt->payload.begin(), receipt->payload.end()) << '\n';
        }
        check(failed.status == Status::CompileFailed);
        check(!failed.diagnostics.empty());
        resource_receipt(journal, failed.receipt);
        check(journal.get(failed.receipt)->visibility == Visibility::Private);
        check(!journal.assessment(failed.receipt));
        rejects(ErrorCode::Permission, [&] { executor.attempt(permit, "int main(){}"); });
        const auto wrong = executor.attempt(permit, "int main(){}", failed.receipt);
        if (wrong.status != Status::TestFailed) std::cerr << wrong.diagnostics << '\n';
        check(wrong.status == Status::TestFailed && wrong.attempt == 2);
        const auto passed = executor.attempt(permit,
            "#include <stdio.h>\nint main(){puts(\"ok\");}", wrong.receipt);
        if (passed.status != Status::Passed) std::cerr << passed.diagnostics << '\n';
        check(passed.status == Status::Passed && passed.attempt == 3);
        resource_receipt(journal, passed.receipt);
        check(!journal.assessment(passed.receipt));
        check(journal.get(passed.artifact)->payload.size() > 0);
        const auto dependencies = references(*journal.get(passed.receipt));
        check(std::find(dependencies.begin(), dependencies.end(), wrong.receipt) != dependencies.end());
        rejects(ErrorCode::Budget, [&] { executor.attempt(permit, "int main(){}", passed.receipt); });
        auto moved = executor.permit("a", 1, {{"", ""}});
        auto destination = std::move(moved);
        rejects(ErrorCode::Permission, [&] { executor.attempt(moved, "int main(){}"); });
        SessionTasks foreign_tasks(directory.path / "foreign.db");
        Executor foreign(memory, foreign_tasks, journal, model, tools);
        rejects(ErrorCode::Permission, [&] { foreign.attempt(destination, "int main(){}"); });
        {
            memory.open_session("lifetime", "test");
            const auto original = memory.append("lifetime", Role::User, "1", "capability-lifetime-fixture");
            SessionTasks scoped_tasks(directory.path / "lifetime.db");
            scoped_tasks.begin(memory, "lifetime", original.id, task_id, 0);
            std::optional<Executor> slot;
            slot.emplace(memory, scoped_tasks, journal, model, tools);
            const void* address = &*slot;
            auto orphan = slot->permit("lifetime", 1, {{"", ""}});
            slot.reset();
            slot.emplace(memory, scoped_tasks, journal, model, tools);
            check(address == &*slot);
            const auto objects_before = journal.objects().objects.size();
            rejects(ErrorCode::Permission, [&] { (void)slot->attempt(orphan, "int main(){}"); });
            check(journal.objects().objects.size() == objects_before);
        }
        {
            memory.open_session("execution-pins", "test");
            const auto original = memory.append("execution-pins", Role::User, "1", "execution-pin-fixture");
            memory.revise_constraint("execution-pins", {"network", "offline", 0, original.id, true}, 0);
            SessionTasks pin_tasks(directory.path / "pin-execution.db");
            pin_tasks.begin(memory, "execution-pins", original.id, task_id, 0);
            Executor pin_executor(memory, pin_tasks, journal, model, tools);
            auto grant = pin_executor.permit("execution-pins", 1, {{"", ""}});
            memory.revise_constraint("execution-pins", {"network", "local-only", 0, original.id, true}, 1);
            const auto before = journal.objects().objects.size();
            rejects(ErrorCode::Permission, [&] { (void)pin_executor.attempt(grant, "int main(){}"); });
            check(journal.objects().objects.size() == before);
        }
        // Use only a synthetic owned file outside the execution directory.
        const auto sentinel = directory.path / "private-sentinel";
        { std::ofstream stream(sentinel); stream << "private"; }
        auto isolation = executor.permit("a", 1, {{"", "isolated\n"}});
        const std::string adversary =
            "#include <stdio.h>\n#include <fcntl.h>\n#include <unistd.h>\n"
            "#include <sys/socket.h>\n#include <netinet/in.h>\n#include <errno.h>\n"
            "int main(){"
            "if(open(\"" + sentinel.string() + "\",O_RDONLY)>=0)return 1;"
            "if(open(\"" + sentinel.string() + "\",O_WRONLY)>=0)return 2;"
            "if(open(\"made\",O_WRONLY|O_CREAT,0600)>=0)return 3;"
            "if(fork()>=0)return 4;"
            "int s=socket(AF_INET,SOCK_STREAM,0);"
            "if(s>=0){sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(9);"
            "a.sin_addr.s_addr=htonl(0x7f000001);"
            "if(connect(s,(sockaddr*)&a,sizeof(a))==0||errno!=EPERM)return 5;}"
            "puts(\"isolated\");}";
        const auto isolated = executor.attempt(isolation, adversary);
        if (isolated.status != Status::Passed) std::cerr << isolated.diagnostics << '\n';
        check(isolated.status == Status::Passed);
        check(std::filesystem::file_size(sentinel) == 7);
        auto binary = executor.permit("a", 1, {{"", std::string(1, static_cast<char>(255))}});
        const auto raw = executor.attempt(binary,
            "#include <unistd.h>\nint main(){unsigned char c=255;return write(1,&c,1)!=1;}");
        check(raw.status == Status::Passed);
        check(field(journal, raw.receipt, "diagnostics_hex") == "ff");
        auto output = executor.permit("a", 1, {{"", ""}}, Limits{1, 10, 1024, 65536});
        const auto flood = executor.attempt(output,
            "#include <unistd.h>\nint main(){char b[4096]{};for(;;)write(1,b,sizeof(b));}");
        check(flood.status == Status::ResourceLimit && flood.diagnostics.size() == 1024);
        check(field(journal, flood.receipt, "stage") == "test");
        auto time = executor.permit("a", 1, {{"", ""}}, Limits{1, 1});
        const auto loop = executor.attempt(time, "int main(){for(;;){}}");
        check(loop.status == Status::ResourceLimit);
        check(field(journal, loop.receipt, "stage") == "test");
        Limits memory_limits;
        memory_limits.attempts = 1;
        memory_limits.resident_bytes = 64 * 1024 * 1024;
        auto memory_permit = executor.permit("a", 1, {{"", ""}}, memory_limits);
        const auto allocation = executor.attempt(memory_permit,
            "#include <stdlib.h>\nint main(){for(;;){"
            "volatile char* p=(volatile char*)malloc(1024*1024);"
            "if(!p)return 1;for(int i=0;i<1024*1024;i+=4096)p[i]=1;}}");
        check(allocation.status == Status::ResourceLimit);
        check(field(journal, allocation.receipt, "stage") == "test");
        resource_receipt(journal, allocation.receipt);
        auto stale = executor.permit("a", 1, {{"", ""}});
        rejects(ErrorCode::Storage, [&] {
            model.load({"missing", directory.path / "missing.gguf", digest(Bytes{1})});
        });
        check(model.state() == axiom::neural::ModelState::Failed);
        rejects(ErrorCode::Permission, [&] { executor.attempt(stale, "int main(){}"); });
        model.unload();
        {
            Journal reopened(directory.path / "knowledge.db");
            check(reopened.get(passed.receipt) == journal.get(passed.receipt));
            check(reopened.get(failed.artifact) == journal.get(failed.artifact));
            check(!reopened.assessment(passed.receipt));
        }
        memory.append("a", Role::User, "2", "Cancel that program");
        rejects(ErrorCode::Permission, [&] { executor.attempt(stale, "int main(){}"); });
        std::cout << "Execution checks passed: " << checks << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
