#include "../src/channel_identity.h"

#include <cassert>
#include <cmath>

int main() {
    constexpr double spacingHz = 6000.0;
    constexpr double manualAHz = 6622000.0;
    constexpr double manualBHz = 6623000.0;

    const double sharedAutoGridA = channel_bank::automaticGridIdentityHz(manualAHz, spacingHz);
    const double sharedAutoGridB = channel_bank::automaticGridIdentityHz(manualBHz, spacingHz);

    // Regression: two exact manual targets may occupy the same automatic grid
    // channel, but they must remain independently blockable identities.
    assert(sharedAutoGridA == sharedAutoGridB);
    assert(channel_bank::slotIdentityHz(manualAHz, 0.0) == manualAHz);
    assert(channel_bank::slotIdentityHz(manualBHz, 0.0) == manualBHz);
    assert(channel_bank::slotIdentityHz(manualAHz, 0.0) !=
           channel_bank::slotIdentityHz(manualBHz, 0.0));

    // Automatic slots consistently retain the canonical grid identity.
    assert(channel_bank::slotIdentityHz(NAN, sharedAutoGridA) == sharedAutoGridA);
    return 0;
}
