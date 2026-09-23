#include "../src/scan_readiness.h"
#include "../src/spectral_floor.h"
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

using channel_bank_scan::Readiness;
using namespace std::chrono;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    Readiness gate;
    const auto t = Readiness::Clock::time_point{} + seconds(10);
    gate.request();
    check(gate.discard(1000000, t + seconds(5)), "IQ must be ignored before tune acknowledgement");
    gate.frame(t);
    check(!gate.ready(), "old detections cannot make an unacknowledged stop ready");
    gate.acknowledge(1000, t, 350);
    check(gate.discard(350, t + milliseconds(1)), "fast buffered delivery cannot bypass wall settling");
    gate.frame(t + milliseconds(2));
    check(!gate.ready(), "settling frames cannot be counted");
    check(gate.discard(1, t + milliseconds(350)), "discard block crossing settling boundary");
    check(!gate.discard(1, t + milliseconds(351)), "fresh collection should now start");
    gate.frame(t + milliseconds(351));
    gate.frame(t + milliseconds(401));
    check(!gate.ready(), "two fresh frames are insufficient");
    gate.frame(t + milliseconds(451));
    check(gate.ready(), "three fresh frames permit decisions");
    check(gate.firstFrame == t + milliseconds(351), "empty dwell starts at first fresh frame");

    auto generation = gate.generation;
    gate.request(); // Even a repeated/same-frequency stop starts a new epoch.
    check(gate.generation != generation && !gate.ready(), "hop must invalidate previous readiness");
    gate.acknowledge(1000, t + seconds(1), 350);
    check(gate.discard(10, t + seconds(3)), "wall time alone cannot substitute for fresh IQ");
    gate.frame(t + seconds(3));
    check(!gate.ready(), "stalled source cannot skip an unmeasured stop");
    check(gate.discard(340, t + seconds(3)), "drain the remaining settling samples");

    // Feed samples through the real FFT collector. Old-source blocks cannot
    // contribute votes, and the post-settle window must span three analyses.
    channel_bank_detector::FrameCollector collector;
    std::vector<float> block(50, 1.0f), fft(32);
    int frames = 0;
    auto feed = [&](int ms) {
        auto now = t + seconds(3) + milliseconds(ms);
        if (gate.discard((int)block.size(), now)) return;
        collector.feed(block.data(), (int)block.size(), fft.data(), (int)fft.size(), 1000,
            [&](int) { ++frames; gate.frame(now); });
    };
    feed(0); feed(50);
    check(frames == 2 && !gate.ready(), "collector cannot advance after two frames");
    feed(100);
    check(frames == 3 && gate.ready(), "collector opens the fresh scan window");

    // Exercise the exact teardown helper used by the retune callback with
    // both discovery channels and two independently hosted recording slots.
    struct Slot { bool multiReceiver; bool fileOpen; };
    Slot discovery{false, true}, receiverA{true, true}, receiverB{true, true};
    std::map<int, Slot*> channels{{1, &discovery}, {2, &receiverA}, {3, &receiverB}};
    int disposed = 0;
    channel_bank_scan::clearDiscoveryChannels(channels, [&](Slot* slot) {
        ++disposed; slot->fileOpen = false;
    });
    check(disposed == 1 && channels.size() == 2, "retune destroys only discovery-owned channels");
    check(receiverA.fileOpen && receiverB.fileOpen, "independent recordings survive discovery retune");
    channel_bank_scan::clearDiscoveryChannels(channels, [&](Slot*) { ++disposed; });
    check(disposed == 1 && channels.size() == 2, "repeated hops preserve independent receivers");
    gate.reset();
    check(!gate.ready() && gate.generation == 0, "restart clears prior readiness");
    // Default/minimum and configurable per-retune settling use both budgets.
    gate.request();
    gate.acknowledge(1000, t);
    check(gate.settlingMs() == 250, "default returns to the user-tested 250 ms");
    check(gate.discard(249, t + milliseconds(250)), "default still requires 250 samples at 1 kHz");
    check(gate.discard(1, t + milliseconds(250)), "discard boundary at default settling");
    check(!gate.discard(1, t + milliseconds(251)), "default allows collection after 250 ms");
    gate.request();
    gate.acknowledge(1000, t, 50);
    check(gate.settlingMs() == 250, "values below tested minimum are clamped");
    gate.request();
    gate.acknowledge(1000, t, 1000);
    check(gate.discard(1000, t + milliseconds(999)), "longer setting holds despite enough IQ");
    check(gate.discard(1, t + milliseconds(1000)), "longer setting discards boundary block");
    check(!gate.discard(1, t + milliseconds(1001)), "longer setting resumes collection");
    gate.request();
    gate.acknowledge(1000, t, 10000);
    check(gate.settlingMs() == 2000, "upper setting bound is enforced");
    std::cout << "Scan readiness and independent receiver preservation passed\n";
}
