// Trusted child launcher. Resource limits are set in a fresh spawned process,
// never by fork() inside the potentially multithreaded model host.
#include <sys/resource.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 3) return 125;
    char* end = nullptr;
    const auto seconds = std::strtoul(argv[1], &end, 10);
    if (!end || *end || seconds < 1 || seconds > 60) return 125;
    const rlimit cpu{seconds, seconds};
    const rlimit file{16 * 1024 * 1024, 16 * 1024 * 1024};
    const rlimit descriptors{64, 64};
    const rlimit core{0, 0};
    if (setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_FSIZE, &file)
        || setrlimit(RLIMIT_NOFILE, &descriptors) || setrlimit(RLIMIT_CORE, &core)) {
        std::perror("execution resource limits");
        return 125;
    }
    execv(argv[2], argv + 2);
    std::perror("execution target");
    return 125;
}
