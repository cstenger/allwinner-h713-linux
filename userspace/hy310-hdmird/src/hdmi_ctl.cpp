/*
 * hy310-hdmi — CLI control for hy310-hdmird daemon
 *
 * Usage:
 *   hy310-hdmi src 3      switch HDMI source to 3 (HDMI_1)
 *   hy310-hdmi status     query daemon
 *   hy310-hdmi init       re-run init sequence
 *   hy310-hdmi quit       shutdown daemon
 */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr const char* SOCKET_PATH = "/run/hy310-hdmird.sock";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: hy310-hdmi <src N|status|init|quit>\n");
        return 1;
    }

    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmd += ' ';
        cmd += argv[i];
    }
    cmd += '\n';

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { perror("socket"); return 2; }
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, SOCKET_PATH, sizeof(sa.sun_path) - 1);
    if (connect(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
        std::fprintf(stderr, "connect %s: %s\n", SOCKET_PATH, std::strerror(errno));
        return 3;
    }
    if (write(sfd, cmd.data(), cmd.size()) != (ssize_t)cmd.size()) {
        perror("write");
        return 4;
    }
    char buf[256];
    ssize_t n = read(sfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        std::fputs(buf, stdout);
    }
    close(sfd);
    return 0;
}
