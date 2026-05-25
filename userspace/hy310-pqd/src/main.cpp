/*
 * hy310-pqd — daemon entry point.
 *
 * Boot sequence:
 *   1. parse argv → Options
 *   2. load config / open devices / install stubs
 *   3. apply initial PQ state
 *   4. either exit (oneshot) or enter socket command loop (daemon)
 */
#include "pqservice.h"

#include <cstdio>
#include <csignal>

using namespace hy310::pqservice;

static PQService* g_service = nullptr;

static void on_term(int) {
    if (g_service) g_service->stop();
}

int main(int argc, char** argv) {
    Options opt;
    if (parse_args(argc, argv, opt) != 0) {
        std::fprintf(stderr,
            "usage: hy310-pqd [--config DIR] [--socket PATH]\n"
            "                 [--no-apply] [--no-stubs]\n"
            "                 [--daemon | --oneshot]\n"
            "                 [-v|-q]\n");
        return 2;
    }

    PQService svc(opt);
    g_service = &svc;
    std::signal(SIGINT,  on_term);
    std::signal(SIGTERM, on_term);
    std::signal(SIGPIPE, SIG_IGN);

    if (opt.daemon_mode)
        return svc.run();
    return svc.start();
}
