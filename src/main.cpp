#include "edge/gateway.hpp"
#include "edge/model.hpp"
#include <csignal>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <sys/stat.h>
int main(int argc, char** argv) {
    try {
        std::string path = "config/gateway.conf";
        bool check = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--config" && i + 1 < argc) path = argv[++i];
            else if (arg == "--check") check = true;
            else if (arg == "--version") { std::cout << "edge-gateway 1.0.0\n"; return 0; }
            else if (arg == "--help") {
                std::cout << "Usage: edge-gateway [--config FILE] [--check] [--version]\n"
                          << "EDGE_API_TOKEN sets optional Bearer authentication; mandatory for non-loopback binds.\n";
                return 0;
            } else throw std::invalid_argument("unknown/missing argument: " + arg);
        }
        const auto config = edge::Config::load(path);
        if (check) { std::cout << "configuration valid\n"; return 0; }
        umask(0077);
        // Block before starting any worker: signalfd owns termination delivery.
        sigset_t mask;
        sigemptyset(&mask); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM);
        if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) throw std::runtime_error("pthread_sigmask failed");
        return edge::run_gateway(config);
    } catch (const std::exception& e) {
        std::cerr << "level=error event=fatal message=\"" << edge::json_escape(e.what()) << "\"\n";
        return 1;
    }
}
