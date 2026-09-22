#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <utils/event.h>

class SourceManager {
public:
    SourceManager();

    struct SourceHandler {
        dsp::stream<dsp::complex_t>* stream;
        void (*menuHandler)(void* ctx);
        void (*selectHandler)(void* ctx);
        void (*deselectHandler)(void* ctx);
        void (*startHandler)(void* ctx);
        void (*stopHandler)(void* ctx);
        void (*tuneHandler)(double freq, void* ctx);
        // Optional independent-source metadata. Sources that provide these
        // callbacks can be leased by modules such as Channel Bank without
        // changing SDR++'s globally selected discovery source.
        double (*getSampleRateHandler)(void* ctx) = nullptr;
        bool (*isRunningHandler)(void* ctx) = nullptr;
        void* ctx;
    };

    struct IndependentSourceInfo {
        std::string name;
        double sampleRate = 0.0;
        bool selected = false;
        bool claimed = false;
        std::string owner;
    };

    enum TuningMode {
        NORMAL,
        PANADAPTER
    };

    void registerSource(std::string name, SourceHandler* handler);
    void unregisterSource(std::string name);
    void selectSource(std::string name);
    void showSelectedMenu();
    void start();
    void stop();
    void tune(double freq);
    void setTuningOffset(double offset);
    void setTuningMode(TuningMode mode);
    void setPanadapterIF(double freq);

    std::vector<std::string> getSourceNames();
    std::string getSelectedSourceName();

    // Independent source leases are deliberately small: SourceManager retains
    // physical start/stop/tune ownership while the caller consumes the source's
    // IQ stream. A selected source cannot be leased, so discovery remains on
    // the normal SDR++ frontend and transmission receivers stay independent.
    std::vector<IndependentSourceInfo> getIndependentSources();
    bool claimIndependentSource(const std::string& name, const std::string& owner);
    void releaseIndependentSource(const std::string& name, const std::string& owner);
    bool startIndependentSource(const std::string& name, const std::string& owner);
    void stopIndependentSource(const std::string& name, const std::string& owner);
    bool tuneIndependentSource(const std::string& name, const std::string& owner, double freq);
    dsp::stream<dsp::complex_t>* getIndependentSourceStream(const std::string& name,
                                                             const std::string& owner);
    double getIndependentSourceSampleRate(const std::string& name,
                                          const std::string& owner);

    Event<std::string> onSourceRegistered;
    Event<std::string> onSourceUnregister;
    Event<std::string> onSourceUnregistered;
    Event<double> onRetune;

private:
    std::map<std::string, SourceHandler*> sources;
    std::map<std::string, std::string> independentOwners;
    mutable std::recursive_mutex sourceMtx;
    std::string selectedName;
    SourceHandler* selectedHandler = NULL;
    double tuneOffset;
    double currentFreq;
    double ifFreq = 0.0;
    TuningMode tuneMode = TuningMode::NORMAL;
    dsp::stream<dsp::complex_t> nullSource;
};
