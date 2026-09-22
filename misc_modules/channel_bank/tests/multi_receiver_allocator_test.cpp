#include "../src/multi_receiver_allocator.h"
#include <cassert>

using namespace channel_bank_multi_receiver;

int main() {
    ReceiverAllocator a;
    a.setReceivers({{"A", 2400000.0, 0.0, true, {}},
                    {"B", 2400000.0, 0.0, true, {}},
                    {"C", 2400000.0, 0.0, true, {}}});

    assert(a.markPending(124700));
    assert(!a.markPending(124700));
    auto first = a.choose(124.700e6, 12500.0);
    assert(first && first->receiverId == "A" && !first->reusedCoverage);
    assert(a.activate(124700, *first));
    assert(!a.markPending(124700)); // duplicate while ACTIVE

    // The FFT may vote for a neighboring 8.33 kHz slot on a later frame.
    // Both pending and active assignments must suppress that second VFO.
    ReceiverAllocator nms;
    assert(nms.markPendingDistinct(124545, 8333.333, 2));
    assert(!nms.markPendingDistinct(124553, 8333.333, 2));
    assert(nms.markPendingDistinct(124570, 8333.333, 2));
    nms.failPending(124545);
    assert(nms.markPendingDistinct(124553, 8333.333, 2));
    nms.clear();
    nms.setReceivers({{"RTL-SDR", 2400000.0, 0.0, true, {}}});
    assert(nms.markPendingDistinct(124545, 8333.333, 2));
    auto nmsAllocation = nms.choose(124.545e6, 8333.333);
    assert(nmsAllocation && nms.activate(124545, *nmsAllocation));
    assert(!nms.markPendingDistinct(124553, 8333.333, 2));
    nms.release(124545, false);
    assert(nms.markPendingDistinct(124553, 8333.333, 2));

    assert(a.markPending(125100));
    auto packed = a.choose(125.100e6, 12500.0);
    assert(packed && packed->receiverId == "A" && packed->reusedCoverage);
    assert(a.activate(125100, *packed));
    assert(a.receivers()[0].channels.size() == 2);

    assert(a.markPending(131400));
    auto second = a.choose(131.400e6, 12500.0);
    assert(second && second->receiverId == "B" && !second->reusedCoverage);
    assert(a.activate(131400, *second));

    assert(a.markPending(140000));
    auto third = a.choose(140.000e6, 12500.0);
    assert(third && third->receiverId == "C");
    assert(a.activate(140000, *third));

    assert(a.markPending(150000));
    assert(!a.choose(150.000e6, 12500.0));
    a.failPending(150000);
    assert(!a.state(150000));

    a.release(125100, false);
    assert(a.receivers()[0].channels.size() == 1 && a.receivers()[0].centerHz != 0.0);
    auto stillCovered = a.choose(124.900e6, 12500.0);
    assert(stillCovered && stillCovered->receiverId == "A" && stillCovered->reusedCoverage);
    a.release(124700, true);
    assert(a.receivers()[0].idle() && a.receivers()[0].centerHz == 0.0);
    assert(a.state(124700) == DispatchState::Suppressed);
    assert(!a.markPending(124700));
    assert(a.clearSuppressed(124700));
    assert(a.markPending(124700));
    assert(!a.clearSuppressed(124700));

    // A failed first idle receiver is skipped deterministically on retry.
    a.failPending(124700);
    a.clear();
    a.setAvailable("A", false);
    assert(a.markPending(118000));
    auto afterFailure = a.choose(118.000e6, 12500.0);
    assert(afterFailure && afterFailure->receiverId == "B");
    assert(a.activate(118000, *afterFailure));

    auto disconnected = a.disconnect("B");
    assert(disconnected.size() == 1 && disconnected[0] == 118000);
    assert(!a.state(118000));
    auto afterDisconnect = a.choose(118.000e6, 12500.0);
    assert(afterDisconnect && afterDisconnect->receiverId == "C");
    return 0;
}
