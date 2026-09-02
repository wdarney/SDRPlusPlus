#pragma once

#include <string>
#include <utility>
#include <vector>
#include <imgui.h>
#include <utils/optionlist.h>
#include "demod.h"

enum RadioModuleInterfaceDemodID {
    RADIO_INTERFACE_DEMOD_NFM,
    RADIO_INTERFACE_DEMOD_WFM,
    RADIO_INTERFACE_DEMOD_AM,
    RADIO_INTERFACE_DEMOD_DSB,
    RADIO_INTERFACE_DEMOD_USB,
    RADIO_INTERFACE_DEMOD_CW,
    RADIO_INTERFACE_DEMOD_LSB,
    RADIO_INTERFACE_DEMOD_RAW,
    _RADIO_INTERFACE_DEMOD_COUNT,
};

struct RadioModuleInterface {
    typedef demod::Demodulator* (*demodProviderFunction)(int);

    std::vector<demodProviderFunction> demodulatorProviders;
    std::vector<std::pair<std::string, int>> radioModes;

    RadioModuleInterface() {
        radioModes.push_back(std::make_pair("NFM", RADIO_INTERFACE_DEMOD_NFM));
        radioModes.push_back(std::make_pair("WFM", RADIO_INTERFACE_DEMOD_WFM));
        radioModes.push_back(std::make_pair("AM", RADIO_INTERFACE_DEMOD_AM));
        radioModes.push_back(std::make_pair("DSB", RADIO_INTERFACE_DEMOD_DSB));
        radioModes.push_back(std::make_pair("USB", RADIO_INTERFACE_DEMOD_USB));
        radioModes.push_back(std::make_pair("CW", RADIO_INTERFACE_DEMOD_CW));
        radioModes.push_back(std::make_pair("LSB", RADIO_INTERFACE_DEMOD_LSB));
        radioModes.push_back(std::make_pair("RAW", RADIO_INTERFACE_DEMOD_RAW));
    }
};
