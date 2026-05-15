#pragma once
#include <string>
#include <vector>
#include <core.h>

// Cross-module interface to the Frequency Manager via core::modComManager.
//
// Usage: add `../frequency_manager/src/` to your module's include path and
// `#include <frequency_manager_interface.h>`. All functions are inline — no
// link-time dependency on the FM module. Calls are safe no-ops when FM is
// not loaded.
//
// FM registers the interface name "frequency_manager" in its constructor
// and unregisters it in its destructor.

namespace fm_iface {

    // Command codes (must match FM's handler switch)
    enum {
        FM_IFACE_CMD_SET_SHOWN  = 0,  // in: SetShownArgs*, out: bool*
        FM_IFACE_CMD_IS_SHOWN   = 1,  // in: const std::string*, out: bool*
        FM_IFACE_CMD_GET_NAMES  = 2,  // in: nullptr, out: std::vector<std::string>*
    };

    struct SetShownArgs {
        const std::string* listName;
        bool shown;
    };

    // Set whether a list's bookmarks are drawn on the main waterfall.
    // Returns true on success, false if FM not loaded or list doesn't exist.
    inline bool setListShownOnWaterfall(const std::string& listName, bool shown) {
        if (!core::modComManager.interfaceExists("frequency_manager")) return false;
        bool result = false;
        SetShownArgs args{ &listName, shown };
        core::modComManager.callInterface("frequency_manager", FM_IFACE_CMD_SET_SHOWN, &args, &result);
        return result;
    }

    // Read current visibility state of a list.
    // Returns false if FM not loaded or list doesn't exist.
    inline bool isListShownOnWaterfall(const std::string& listName) {
        if (!core::modComManager.interfaceExists("frequency_manager")) return false;
        bool result = false;
        core::modComManager.callInterface("frequency_manager", FM_IFACE_CMD_IS_SHOWN,
                                         (void*)&listName, &result);
        return result;
    }

    // Returns the names of all lists in FM's config (arbitrary order).
    // Returns empty vector if FM not loaded.
    inline std::vector<std::string> getListNames() {
        if (!core::modComManager.interfaceExists("frequency_manager")) return {};
        std::vector<std::string> result;
        core::modComManager.callInterface("frequency_manager", FM_IFACE_CMD_GET_NAMES,
                                         nullptr, &result);
        return result;
    }

}  // namespace fm_iface
