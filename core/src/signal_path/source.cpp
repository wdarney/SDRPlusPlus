#include <server.h>
#include <signal_path/source.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <cmath>

SourceManager::SourceManager() {
}

void SourceManager::registerSource(std::string name, SourceHandler* handler) {
    std::unique_lock<std::recursive_mutex> lock(sourceMtx);
    if (sources.find(name) != sources.end()) {
        flog::error("Tried to register new source with existing name: {0}", name);
        return;
    }
    sources[name] = handler;
    lock.unlock();
    onSourceRegistered.emit(name);
}

void SourceManager::unregisterSource(std::string name) {
    std::unique_lock<std::recursive_mutex> lock(sourceMtx);
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to unregister non existent source: {0}", name);
        return;
    }
    lock.unlock();
    onSourceUnregister.emit(name);
    lock.lock();
    if (sources.find(name) == sources.end()) return;
    if (name == selectedName) {
        if (selectedHandler != NULL) {
            sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
        }
        sigpath::iqFrontEnd.setInput(&nullSource);
        selectedHandler = NULL;
        selectedName.clear();
    }
    sources.erase(name);
    independentOwners.erase(name);
    lock.unlock();
    onSourceUnregistered.emit(name);
}

std::vector<std::string> SourceManager::getSourceNames() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    std::vector<std::string> names;
    for (auto const& [name, src] : sources) { names.push_back(name); }
    return names;
}

std::string SourceManager::getSelectedSourceName() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    return selectedName;
}

std::vector<SourceManager::IndependentSourceInfo> SourceManager::getIndependentSources() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    std::vector<IndependentSourceInfo> result;
    result.reserve(sources.size());
    for (const auto& [name, handler] : sources) {
        IndependentSourceInfo info;
        info.name = name;
        info.selected = name == selectedName;
        auto owner = independentOwners.find(name);
        info.claimed = owner != independentOwners.end();
        if (info.claimed) info.owner = owner->second;
        if (handler && handler->getSampleRateHandler) {
            try {
                info.sampleRate = handler->getSampleRateHandler(handler->ctx);
            }
            catch (...) {
                flog::warn("Source '{0}' failed to report its independent sample rate", name);
                info.sampleRate = 0.0;
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

bool SourceManager::claimIndependentSource(const std::string& name, const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (owner.empty() || it == sources.end() || name == selectedName ||
        !it->second || !it->second->stream || !it->second->getSampleRateHandler)
        return false;
    auto claimed = independentOwners.find(name);
    if (claimed != independentOwners.end()) return claimed->second == owner;
    independentOwners[name] = owner;
    return true;
}

void SourceManager::releaseIndependentSource(const std::string& name, const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = independentOwners.find(name);
    if (it != independentOwners.end() && it->second == owner) independentOwners.erase(it);
}

static bool sourceOwnedBy(const std::map<std::string, std::string>& owners,
                          const std::string& name, const std::string& owner) {
    auto it = owners.find(name);
    return it != owners.end() && it->second == owner;
}

bool SourceManager::startIndependentSource(const std::string& name, const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (it == sources.end() || !sourceOwnedBy(independentOwners, name, owner)) return false;
    SourceHandler* handler = it->second;
    if (!handler || !handler->startHandler) return false;
    try {
        handler->startHandler(handler->ctx);
        return !handler->isRunningHandler || handler->isRunningHandler(handler->ctx);
    }
    catch (...) {
        flog::error("Independent source '{0}' threw while starting", name);
        return false;
    }
}

void SourceManager::stopIndependentSource(const std::string& name, const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (it == sources.end() || !sourceOwnedBy(independentOwners, name, owner)) return;
    if (it->second && it->second->stopHandler) it->second->stopHandler(it->second->ctx);
}

bool SourceManager::tuneIndependentSource(const std::string& name, const std::string& owner,
                                          double freq) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (it == sources.end() || !sourceOwnedBy(independentOwners, name, owner) ||
        !std::isfinite(freq) || freq <= 0.0 || !it->second || !it->second->tuneHandler)
        return false;
    try {
        it->second->tuneHandler(freq, it->second->ctx);
        return true;
    }
    catch (...) {
        flog::error("Independent source '{0}' threw while tuning to {1} Hz", name, freq);
        return false;
    }
}

dsp::stream<dsp::complex_t>* SourceManager::getIndependentSourceStream(
    const std::string& name, const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (it == sources.end() || !sourceOwnedBy(independentOwners, name, owner)) return nullptr;
    return it->second ? it->second->stream : nullptr;
}

double SourceManager::getIndependentSourceSampleRate(const std::string& name,
                                                      const std::string& owner) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    auto it = sources.find(name);
    if (it == sources.end() || !sourceOwnedBy(independentOwners, name, owner) ||
        !it->second || !it->second->getSampleRateHandler)
        return 0.0;
    try {
        return it->second->getSampleRateHandler(it->second->ctx);
    }
    catch (...) {
        flog::warn("Independent source '{0}' failed to report its sample rate", name);
        return 0.0;
    }
}

void SourceManager::selectSource(std::string name) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    if (sources.find(name) == sources.end()) {
        flog::error("Tried to select non existent source: {0}", name);
        return;
    }
    if (independentOwners.find(name) != independentOwners.end()) {
        flog::error("Tried to select independently leased source: {0}", name);
        return;
    }
    if (selectedHandler != NULL) {
        sources[selectedName]->deselectHandler(sources[selectedName]->ctx);
    }
    selectedHandler = sources[name];
    selectedHandler->selectHandler(selectedHandler->ctx);
    selectedName = name;
    if (core::args["server"].b()) {
        server::setInput(selectedHandler->stream);
    }
    else {
        sigpath::iqFrontEnd.setInput(selectedHandler->stream);
    }
    // Set server input here
}

void SourceManager::showSelectedMenu() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->menuHandler(selectedHandler->ctx);
}

void SourceManager::start() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->startHandler(selectedHandler->ctx);
}

void SourceManager::stop() {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    if (selectedHandler == NULL) {
        return;
    }
    selectedHandler->stopHandler(selectedHandler->ctx);
}

void SourceManager::tune(double freq) {
    std::lock_guard<std::recursive_mutex> lock(sourceMtx);
    if (selectedHandler == NULL) {
        return;
    }
    // TODO: No need to always retune the hardware in Panadapter mode
    selectedHandler->tuneHandler(abs(((tuneMode == TuningMode::NORMAL) ? (freq + tuneOffset) : ifFreq)), selectedHandler->ctx);
    onRetune.emit(freq + tuneOffset);
    currentFreq = freq;
}

void SourceManager::setTuningOffset(double offset) {
    tuneOffset = offset;
    tune(currentFreq);
}

void SourceManager::setTuningMode(TuningMode mode) {
    tuneMode = mode;
    tune(currentFreq);
}

void SourceManager::setPanadapterIF(double freq) {
    ifFreq = freq;
    tune(currentFreq);
}
