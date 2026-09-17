#include "dashboard_service.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <filesystem>

int main() {
#ifndef _WIN32
    char folder[] = "/tmp/vdl2-service-XXXXXX";
    assert(mkdtemp(folder));
    std::string root = folder;
    auto script = root + "/child with spaces.sh";
    auto log = root + "/log";
    // Stand-in for Python: EOF on the private stdin pipe ends the process.
    { std::ofstream out(script); out << "while read line; do :; done\n"; }
    DashboardService service;
    service.stop(); service.stop();
    assert(!service.start("/bin/sh", script, "", 5057, log));
    assert(!service.start(root + "/missing", script, "source", 5057, log));
    for (int i = 0; i < 5; ++i) {
        assert(service.start("/bin/sh", script, "source with spaces", 5057, log));
        assert(service.active());
        assert(!service.start("/bin/sh", script, "source", 5057, log));
        service.stop(); service.stop();
        for (int n = 0; n < 250 && service.active(); ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        assert(!service.active());
    }
    { DashboardService owned; assert(owned.start("/bin/sh", script, "source", 5057, log)); }
    // Immediate child startup failure must be reaped and allow a retry.
    assert(service.start("/bin/sh", root + "/missing", "source", 5057, log));
    for (int n = 0; n < 100 && service.active(); ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!service.active());
    // A stuck server ignoring EOF cannot hold up module destruction forever.
    { std::ofstream out(script); out << "while :; do :; done\n"; }
    auto before = std::chrono::steady_clock::now();
    { DashboardService stuck; assert(stuck.start("/bin/sh", script, "source", 5057, log)); }
    assert(std::chrono::steady_clock::now() - before < std::chrono::seconds(6));
    std::filesystem::remove_all(root);
#endif
}
