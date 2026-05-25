/*
 * hy310-pqctl — CLI for sending commands to a running hy310-pqd daemon.
 *
 *   hy310-pqctl apply
 *   hy310-pqctl dump
 *   hy310-pqctl set KEY VALUE
 */
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static constexpr const char* kDefaultSocket = "/run/hy310-pqd.sock";

static int send_cmd(const std::string& sock, const std::string& cmd) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "connect(%s): %s\n", sock.c_str(), std::strerror(errno));
        ::close(fd);
        return 1;
    }
    std::string line = cmd + "\n";
    ::write(fd, line.data(), line.size());

    char buf[512];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        std::fputs(buf, stdout);
    }
    ::close(fd);
    return 0;
}

static void usage() {
    std::fputs(
        "usage: hy310-pqctl [--socket PATH] COMMAND [ARGS]\n"
        "  apply               — re-apply current settings to HW\n"
        "  dump                — show current state\n"
        "  set KEY VALUE       — set one parameter\n"
        "\n"
        "keys: brightness contrast saturation hue sharpness backlight\n"
        "      colortemperature gamma tnr snr dci blackextension picture_mode\n",
        stderr);
}

int main(int argc, char** argv) {
    std::string sock = kDefaultSocket;
    int argi = 1;
    if (argi < argc && std::string(argv[argi]) == "--socket") {
        if (argi + 1 >= argc) { usage(); return 2; }
        sock = argv[argi + 1];
        argi += 2;
    }
    if (argi >= argc) { usage(); return 2; }

    std::string cmd = argv[argi++];
    for (; argi < argc; ++argi) {
        cmd += " ";
        cmd += argv[argi];
    }
    return send_cmd(sock, cmd);
}
