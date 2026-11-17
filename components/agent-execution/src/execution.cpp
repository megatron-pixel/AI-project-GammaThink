#include "execution.hpp"

#include <json-c/json.h>
#include <algorithm>
#include <thread>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <libproc.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace axiom::execution {
namespace {
using namespace knowledge;
void require(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {
        require(fd >= 0, ErrorCode::Storage, "Cannot open execution descriptor");
    }
    ~Fd() { close(value); }
    Fd(const Fd&) = delete;
};
struct Directory {
    std::filesystem::path path;
    Directory() {
        std::string pattern = "/private/tmp/gammathink-execution-XXXXXX";
        const auto made = mkdtemp(pattern.data());
        require(made, ErrorCode::Storage, "Cannot create private execution directory");
        path = std::filesystem::canonical(made);
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void write_file(const std::filesystem::path& path, std::string_view value) {
    Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    while (!value.empty()) {
        const auto size = write(fd.value, value.data(), value.size());
        if (size < 0 && errno == EINTR) continue;
        require(size > 0, ErrorCode::Storage, "Cannot write execution input");
        value.remove_prefix(static_cast<std::size_t>(size));
    }
}
std::string quoted(const std::filesystem::path& path) {
    const auto text = path.string();
    require(text.find_first_of("\"\\\n\r") == std::string::npos,
            ErrorCode::Schema, "Unsupported sandbox path");
    return "\"" + text + "\"";
}
std::string profile(const Toolchain& tools, const Directory& dir, bool compile) {
    // Explicit read roots, not ambient home/repository access. No networking,
    // process inspection, signal-other, devices, or arbitrary process execution.
    std::string text = "(version 1)(deny default)"
        "(allow sysctl-read)(allow process-info* (target self))"
        "(allow file-read-metadata (path-ancestors " + quoted(tools.compiler) + " "
        + quoted(tools.sdk) + " " + quoted(tools.helper) + " " + quoted(dir.path) + "))"
        // macOS 27 libignition requires a root descriptor for openat().
        // This is literal '/', not recursive access to its children.
        "(allow file-read* (literal \"/\") (subpath \"/System\") (subpath \"/usr/lib\")"
        " (subpath \"/usr/share\") (literal \"/dev/null\")"
        " (literal " + quoted(tools.helper) + ") (subpath " + quoted(dir.path) + "))"
        "(allow file-write* (literal \"/dev/null\"))"
        "(allow process-exec (literal " + quoted(tools.helper) + ")";
    if (compile) {
        text += " (subpath \"/Library/Developer/CommandLineTools/usr/bin\"))"
            "(allow process-fork)"
            "(allow file-write* (subpath " + quoted(dir.path) + "))"
            "(allow file-read* (subpath \"/Library/Developer/CommandLineTools\")"
            " (subpath \"/usr/include\") (literal \"/private/var/db/xcode_select_link\"))";
    } else {
        text += " (literal " + quoted(dir.path / "program") + "))";
    }
    return text;
}
struct Process {
    int code = -1, signal = 0;
    bool limited = false;
    std::string output;
    std::uint64_t peak_resident = 0, peak_controller_resident = 0, peak_application_resident = 0;
    std::uint64_t resident_samples = 0;
};
Process run(const Toolchain& tools, const Directory& dir, const Limits& limits,
            bool compile, const std::filesystem::path& input) {
    int pipe_fds[2];
    require(pipe(pipe_fds) == 0, ErrorCode::Storage, "Cannot create execution pipe");
    Fd reader(pipe_fds[0]), writer(pipe_fds[1]);
    Fd stdin_fd(open(input.c_str(), O_RDONLY | O_CLOEXEC));
    Fd queue(kqueue());
    require(fcntl(reader.value, F_SETFL, O_NONBLOCK) == 0, ErrorCode::Storage,
            "Cannot configure execution pipe");
    std::vector<std::string> args{"/usr/bin/sandbox-exec", "-p", profile(tools, dir, compile),
        tools.helper.string(), std::to_string(compile ? 60 : limits.seconds)};
    if (compile) {
        args.insert(args.end(), {tools.compiler.string(), "-std=c++20", "-O0",
            "-fno-modules", "-fuse-ld=/Library/Developer/CommandLineTools/usr/bin/ld",
            "-isysroot", tools.sdk.string(),
            (dir.path / "source.cpp").string(), "-o", (dir.path / "program").string()});
    } else args.push_back((dir.path / "program").string());
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> env{"PATH=/usr/bin:/bin", "HOME=" + dir.path.string(),
        "TMPDIR=" + dir.path.string(), "LC_ALL=C"};
    std::vector<char*> envp;
    for (auto& item : env) envp.push_back(item.data());
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    require(posix_spawn_file_actions_init(&actions) == 0, ErrorCode::Storage, "Cannot create child actions");
    struct Cleanup {
        posix_spawn_file_actions_t* actions;
        posix_spawnattr_t* attributes;
        bool attr = false;
        ~Cleanup() {
            posix_spawn_file_actions_destroy(actions);
            if (attr) posix_spawnattr_destroy(attributes);
        }
    } cleanup{&actions, &attributes};
    require(posix_spawnattr_init(&attributes) == 0, ErrorCode::Storage, "Cannot create child attributes");
    cleanup.attr = true;
    require(posix_spawn_file_actions_adddup2(&actions, stdin_fd.value, STDIN_FILENO) == 0
        && posix_spawn_file_actions_adddup2(&actions, writer.value, STDOUT_FILENO) == 0
        && posix_spawn_file_actions_adddup2(&actions, writer.value, STDERR_FILENO) == 0
        && posix_spawn_file_actions_addchdir(&actions, dir.path.c_str()) == 0
        && posix_spawnattr_setflags(&attributes, POSIX_SPAWN_START_SUSPENDED
            | POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP) == 0
        && posix_spawnattr_setpgroup(&attributes, 0) == 0,
        ErrorCode::Storage, "Cannot configure isolated child");
    pid_t child = -1;
    require(posix_spawn(&child, argv.front(), &actions, &attributes, argv.data(), envp.data()) == 0,
            ErrorCode::Unavailable, "Cannot spawn sandbox");
    struct Child {
        pid_t pid;
        bool reaped = false;
        ~Child() {
            if (!reaped) {
                kill(-pid, SIGKILL);
                int status;
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            }
        }
    } owned{child};
    close(writer.value); writer.value = -1;
    struct kevent subscriptions[4];
    EV_SET(&subscriptions[0], child, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    EV_SET(&subscriptions[1], reader.value, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    EV_SET(&subscriptions[2], 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0,
           (compile ? 60 : limits.seconds) * 1000, nullptr);
    // Resource-accounting events, not a sleep/retry loop. The resident ceiling
    // is sampled and can overshoot between ticks; it is not an OS hard cap.
    EV_SET(&subscriptions[3], 2, EVFILT_TIMER, EV_ADD, 0, 10, nullptr);
    require(kevent(queue.value, subscriptions, 4, nullptr, 0, nullptr) == 0,
            ErrorCode::Storage, "Cannot subscribe to child lifecycle");
    Process result;
    const auto sample_resident = [&] {
        pid_t members[64];
        const auto size = proc_listpids(PROC_PGRP_ONLY, child, members, sizeof(members));
        require(size >= 0, ErrorCode::Unavailable, "Cannot account execution process group");
        std::uint64_t resident = 0;
        if (size >= static_cast<int>(sizeof(members))) result.limited = true;
        for (int j = 0; j < size / static_cast<int>(sizeof(pid_t)); ++j) {
            rusage_info_v2 usage{};
            if (proc_pid_rusage(members[j], RUSAGE_INFO_V2,
                reinterpret_cast<rusage_info_t*>(&usage)) == 0) resident += usage.ri_resident_size;
            else require(errno == ESRCH, ErrorCode::Unavailable, "Cannot account execution memory");
        }
        const auto controller = neural::process_memory().resident_bytes;
        require(controller > 0, ErrorCode::Unavailable, "Cannot account execution controller memory");
        result.peak_resident = std::max(result.peak_resident, resident);
        result.peak_controller_resident = std::max(result.peak_controller_resident, controller);
        result.peak_application_resident = std::max(result.peak_application_resident, resident + controller);
        ++result.resident_samples;
        return resident;
    };
    // Even a child that exits before the first timer gets an observed sample.
    sample_resident();
    require(kill(child, SIGCONT) == 0, ErrorCode::Storage, "Cannot start subscribed child");
    bool exited = false, eof = false;
    while (!exited || !eof) {
        struct kevent events[3];
        const auto count = kevent(queue.value, nullptr, 0, events, 3, nullptr);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, ErrorCode::Storage, "Cannot observe child lifecycle");
        for (int i = 0; i < count; ++i) {
            const auto& event = events[i];
            require(!(event.flags & EV_ERROR), ErrorCode::Storage, "Child event subscription failed");
            if (event.filter == EVFILT_TIMER) {
                if (event.ident == 1) {
                    result.limited = true;
                    kill(-child, SIGKILL);
                } else {
                    const auto resident = sample_resident();
                    const auto ceiling = compile ? 1024ULL * 1024 * 1024 : limits.resident_bytes;
                    if (resident > ceiling) result.limited = true;
                    if (result.limited) kill(-child, SIGKILL);
                }
            } else if (event.filter == EVFILT_PROC) {
                exited = true;
                kill(-child, SIGKILL);
            } else if (event.filter == EVFILT_READ) {
                char buffer[4096];
                while (true) {
                    const auto size = read(reader.value, buffer, sizeof(buffer));
                    if (size < 0 && errno == EINTR) continue;
                    if (size < 0 && errno == EAGAIN) break;
                    require(size >= 0, ErrorCode::Storage, "Cannot read execution output");
                    if (!size) { eof = true; break; }
                    const auto output_limit = compile ? 65536 : limits.output_bytes;
                    const auto accepted = std::min(static_cast<std::size_t>(size),
                        output_limit - result.output.size());
                    result.output.append(buffer, accepted);
                    if (accepted != static_cast<std::size_t>(size)) {
                        result.limited = true;
                        kill(-child, SIGKILL);
                    }
                }
                if (eof) {
                    struct kevent remove;
                    EV_SET(&remove, reader.value, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                    require(kevent(queue.value, &remove, 1, nullptr, 0, nullptr) == 0,
                            ErrorCode::Storage, "Cannot close output subscription");
                }
            }
        }
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        require(errno == EINTR, ErrorCode::Storage, "Cannot reap execution child");
    }
    owned.reaped = true;
    if (WIFEXITED(status)) result.code = WEXITSTATUS(status);
    if (WIFSIGNALED(status)) result.signal = WTERMSIG(status);
    result.limited = result.limited || result.signal == SIGXCPU || result.signal == SIGXFSZ;
    return result;
}
ObjectId object(Journal& journal, ObjectKind kind, std::string format,
                std::string_view payload, std::vector<Link> links) {
    Object value;
    value.kind = kind;
    value.format = std::move(format);
    value.payload = Bytes(payload.begin(), payload.end());
    value.links = std::move(links);
    return journal.put(value);
}
std::string json_text(std::string_view input) {
    auto* value = json_object_new_string_len(input.data(), static_cast<int>(input.size()));
    require(value, ErrorCode::Budget, "Cannot encode execution receipt");
    const std::string result = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    json_object_put(value);
    return result;
}
std::string json_bytes(std::string_view input) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 15]);
    }
    return json_text(encoded);
}
}

Executor::Executor(context::ConversationMemory& memory, context::SessionTasks& tasks,
                   Journal& journal, neural::NativeModelManager& model, Toolchain tools)
    : Executor(memory, tasks, journal, model, std::move(tools), -1) {}

Executor::Executor(context::ConversationMemory& memory, context::SessionTasks& tasks,
                   Journal& journal, neural::NativeModelManager& model, Toolchain tools, int session_lock)
    : memory_(memory), tasks_(tasks), journal_(journal), model_(model), tools_(std::move(tools)) {
    for (auto* path : {&tools_.compiler, &tools_.sdk, &tools_.helper}) {
        *path = std::filesystem::canonical(*path);
        (void)quoted(*path);
    }
    require(tools_.compiler == std::filesystem::canonical("/Library/Developer/CommandLineTools/usr/bin/clang++"),
            ErrorCode::Permission, "Execution profile requires the qualified Apple toolchain");
    require(tasks.path() != ":memory:", ErrorCode::Schema, "Execution requires durable task state");
    if (session_lock >= 0) {
        execution_lock_ = fcntl(session_lock, F_DUPFD_CLOEXEC, 0);
        require(execution_lock_ >= 0, ErrorCode::Storage, "Cannot share the session execution owner");
        return;
    }
    const auto path = tasks.path().string() + ".execution.lock";
    const auto descriptor = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    require(descriptor >= 0, ErrorCode::Storage, "Cannot open execution ownership lock");
    if (flock(descriptor, LOCK_EX | LOCK_NB)) {
        close(descriptor);
        throw Error(ErrorCode::Permission, "Task database already has an execution owner");
    }
    execution_lock_ = descriptor;
}
Executor::~Executor() { if (execution_lock_ >= 0) close(execution_lock_); }

void Executor::current(const context::SessionTask& task, const std::vector<context::Constraint>* pins) {
    require(owner_ == std::this_thread::get_id(), ErrorCode::Permission, "Execution requires its owner thread");
    require(model_.state() == neural::ModelState::Unloaded,
            ErrorCode::Permission, "Unload the neural model before compilation and execution");
    const auto active = tasks_.active(task.session);
    require(active && active->revision == task.revision && active->ordered_task == task.ordered_task
        && active->user_message == task.user_message && active->message_hash == task.message_hash
        && active->phase != context::TaskPhase::Cancelled,
        ErrorCode::Permission, "Execution task changed or was cancelled");
    const auto original = memory_.get(task.session, task.user_message);
    const auto recent = memory_.recent(task.session, 1);
    require(original && original->role == context::Role::User
        && original->content_hash == task.message_hash && recent.messages.size() == 1
        && recent.messages.front().id == original->id,
        ErrorCode::Permission, "Execution no longer matches the current original user message");
    require(!pins || memory_.constraints(task.session, 256) == *pins, ErrorCode::Permission,
        "Execution correction pins changed after authorization");
}

Permit Executor::permit(std::string_view session, std::uint64_t expected_revision,
                        std::vector<Test> tests, Limits limits) {
    require(owner_ == std::this_thread::get_id(), ErrorCode::Permission, "Execution requires its owner thread");
    const auto task = tasks_.active(session);
    require(task && task->revision == expected_revision, ErrorCode::Permission, "Stale execution authorization");
    current(*task);
    require(limits.attempts >= 1 && limits.attempts <= 8 && limits.seconds >= 1 && limits.seconds <= 60
        && limits.output_bytes >= 1 && limits.output_bytes <= 65536
        && limits.source_bytes >= 1 && limits.source_bytes <= 65536
        && limits.resident_bytes >= 16 * 1024 * 1024 && limits.resident_bytes <= 1024 * 1024 * 1024
        && !tests.empty() && tests.size() <= 16, ErrorCode::Budget, "Invalid execution budget");
    auto pins = memory_.constraints(session, 256);
    std::string specification = "{\"tests\":[";
    for (const auto& test : tests) {
        require(test.input.size() <= 65536 && test.output.size() <= limits.output_bytes,
                ErrorCode::Budget, "Execution test exceeds its budget");
        if (specification.back() != '[') specification += ',';
        specification += "{\"input_hex\":" + json_bytes(test.input)
            + ",\"output_hex\":" + json_bytes(test.output) + "}";
    }
    specification += "],\"session\":" + json_text(task->session) + ",\"revision\":"
        + std::to_string(task->revision) + ",\"user_message\":" + std::to_string(task->user_message)
        + ",\"message_hash\":" + json_text(hex(ObjectId{task->message_hash}))
        + ",\"attempts\":" + std::to_string(limits.attempts) + ",\"seconds\":"
        + std::to_string(limits.seconds) + ",\"output_bytes\":" + std::to_string(limits.output_bytes)
        + ",\"source_bytes\":" + std::to_string(limits.source_bytes)
        + ",\"sampled_resident_limit\":" + std::to_string(limits.resident_bytes)
        + ",\"compiler_sampled_resident_limit\":1073741824,\"compiler_seconds\":60,\"compiler_output_bytes\":65536,\"pins\":[";
    for (const auto& pin : pins) {
        if (specification.back() != '[') specification += ',';
        const auto hash = digest({reinterpret_cast<const std::uint8_t*>(pin.content.data()), pin.content.size()});
        specification += "{\"key\":" + json_text(pin.key) + ",\"revision\":" + std::to_string(pin.revision)
            + ",\"source_message\":" + std::to_string(pin.source_message)
            + ",\"content_hash\":" + json_text(hex({hash})) + "}";
    }
    specification += "]}";
    Permit permit;
    permit.owner_ = capability_;
    permit.task_ = *task;
    permit.tests_ = std::move(tests);
    permit.pins_ = std::move(pins);
    permit.limits_ = limits;
    permit.specification_ = object(journal_, ObjectKind::Action, "gammathink.execution-tests.v1",
        specification, {{Relationship::DependsOn, task->ordered_task}});
    return permit;
}

Result Executor::attempt(Permit& permit, std::string_view source, std::optional<ObjectId> repairs,
    std::optional<ObjectId> source_artifact) {
    require(permit.owner_ == capability_, ErrorCode::Permission, "Foreign execution permit");
    current(permit.task_, &permit.pins_);
    require(permit.used_ < permit.limits_.attempts, ErrorCode::Budget, "Execution repair budget exhausted");
    require(!source.empty() && source.size() <= permit.limits_.source_bytes,
            ErrorCode::Budget, "Execution source exceeds its budget");
    require(!permit.previous_ ? !repairs : repairs == permit.previous_->receipt
        && permit.previous_->status != Status::Passed, ErrorCode::Permission, "Repair must reference the last failed receipt");
    if (source_artifact) {
        const auto object = journal_.get(*source_artifact);
        const auto assessment = journal_.assessment(*source_artifact);
        require(object && object->kind == ObjectKind::Action && object->format == "gammathink.cpp-source.v1" &&
            std::string(object->payload.begin(), object->payload.end()) == source &&
            (!assessment || (assessment->state != KnowledgeState::Invalidated &&
                assessment->logical != LogicalValidity::Invalid)), ErrorCode::Integrity,
            "Execution source differs from its exact current artifact");
    }
    ++permit.used_;
    std::vector<Link> links{{Relationship::DependsOn, permit.specification_}};
    if (repairs) links.push_back({Relationship::DependsOn, *repairs});
    if (source_artifact) links.push_back({Relationship::DependsOn, *source_artifact});
    const auto artifact = object(journal_, ObjectKind::Action, "gammathink.cpp-source.v1", source, links);
    Directory directory;
    write_file(directory.path / "source.cpp", source);
    write_file(directory.path / "empty", "");
    auto process = run(tools_, directory, permit.limits_, true, directory.path / "empty");
    auto status = process.limited ? Status::ResourceLimit : process.code == 0 ? Status::Passed : Status::CompileFailed;
    std::string diagnostics = process.output;
    auto peak_resident = process.peak_resident;
    auto peak_controller_resident = process.peak_controller_resident;
    auto peak_application_resident = process.peak_application_resident;
    auto resident_samples = process.resident_samples;
    std::size_t completed = 0;
    const auto stage = status == Status::Passed ? "test" : "compile";
    if (status == Status::Passed) {
        // Compilation had write access. Never run if it replaced the expected
        // executable with a symlink or another filesystem object.
        const auto binary = std::filesystem::symlink_status(directory.path / "program");
        require(std::filesystem::is_regular_file(binary), ErrorCode::Integrity, "Compiler did not produce a regular executable");
        for (const auto& test : permit.tests_) {
            current(permit.task_, &permit.pins_);
            const auto input = directory.path / ("input-" + std::to_string(completed));
            write_file(input, test.input);
            process = run(tools_, directory, permit.limits_, false, input);
            peak_resident = std::max(peak_resident, process.peak_resident);
            peak_controller_resident = std::max(peak_controller_resident, process.peak_controller_resident);
            peak_application_resident = std::max(peak_application_resident, process.peak_application_resident);
            resident_samples += process.resident_samples;
            diagnostics = process.output;
            if (process.limited) { status = Status::ResourceLimit; break; }
            if (process.code != 0 || process.output != test.output) { status = Status::TestFailed; break; }
            ++completed;
        }
    }
    current(permit.task_, &permit.pins_);
    const std::string payload = "{\"attempt\":" + std::to_string(permit.used_)
        + ",\"status\":" + std::to_string(static_cast<int>(status))
        + ",\"tests_passed\":" + std::to_string(completed)
        + ",\"stage\":" + json_text(stage)
        + ",\"exit_code\":" + std::to_string(process.code)
        + ",\"signal\":" + std::to_string(process.signal)
        + ",\"sampled_peak_resident_bytes\":" + std::to_string(peak_resident)
        + ",\"sampled_peak_controller_resident_bytes\":" + std::to_string(peak_controller_resident)
        + ",\"sampled_peak_application_resident_bytes\":" + std::to_string(peak_application_resident)
        + ",\"resident_sample_count\":" + std::to_string(resident_samples)
        + ",\"resident_scope\":\"controller-and-execution-process-group\""
        + ",\"recorded_unix_ms\":" + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count())
        + ",\"diagnostics_hex\":" + json_bytes(diagnostics)
        + ",\"method\":\"darwin-sandbox-exact-output-v1\",\"logical_proof\":false,\"empirical_truth\":false}";
    links.push_back({Relationship::Tests, artifact});
    const auto receipt = object(journal_, ObjectKind::TestResult, "gammathink.execution-result.v1", payload, links);
    Result result{status, artifact, receipt, std::move(diagnostics), permit.used_};
    permit.previous_ = result;
    return result;
}

} // namespace axiom::execution
