#include <neograph/mcp/client.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <iostream>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// A separate process keeps the irreversible seccomp filter out of other tests.
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    constexpr int high_descriptor = 70000;
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0
        || limit.rlim_max <= high_descriptor) {
        std::cerr << "Descriptor limit cannot exercise the high-FD boundary\n";
        return 77;
    }
    if (limit.rlim_cur <= high_descriptor) {
        limit.rlim_cur = high_descriptor + 1;
        if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) return 77;
    }
    const int source = ::open("/dev/null", O_RDONLY);
    if (source < 0) return 2;
    const int inherited = ::fcntl(source, F_DUPFD, high_descriptor);
    ::close(source);
    if (inherited < 0) return 2;
    // Existing descriptors survive a lowered soft limit.
    limit.rlim_cur = 1024;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) return 2;

#ifdef SYS_close_range
    sock_filter instructions[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close_range, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog filter {
        static_cast<unsigned short>(sizeof(instructions) / sizeof(instructions[0])),
        instructions,
    };
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) != 0
        || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &filter) != 0) {
        std::cerr << "Seccomp is unavailable; cannot force the close_range fallback\n";
        return 77;
    }
#endif

    try {
        neograph::mcp::StdioClientConfig config;
        config.argv = {argv[1], "-c", R"PY(
import errno, json, os, sys
for line in sys.stdin:
    request = json.loads(line)
    if 'id' not in request:
        continue
    if request['method'] == 'initialize':
        result = {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                  'serverInfo': {'name': 'descriptor-boundary', 'version': '1'}}
        print(json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                          'result': result}), flush=True)
        continue
    try:
        os.fstat(int(sys.argv[1]))
        inherited = True
    except OSError as error:
        if error.errno != errno.EBADF:
            raise
        inherited = False
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                      'result': {'content': [], 'inherited': inherited}}), flush=True)
)PY", std::to_string(inherited)};
        config.request_timeout = std::chrono::seconds(5);
        neograph::mcp::MCPClient client(std::move(config));
        const auto result = client.call_tool("probe", neograph::json::object());
        if (result.at("inherited").get<bool>()) {
            std::cerr << "MCP child inherited descriptor " << inherited
                      << " when close_range was unavailable\n";
            return 1;
        }
        std::cout << "MCP child closed descriptor " << inherited
                  << " through the close_range fallback\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    ::close(inherited);
    return 0;
}
