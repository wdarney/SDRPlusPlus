#include <imgui.h>
#include <module.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <dsp/channel/rx_vfo.h>
#include <dsp/sink/handler_sink.h>
#include <signal_path/signal_path.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <config.h>
#include <core.h>
#include <cmath>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:        */ "selcal_decoder",
    /* Description: */ "SELCAL decoder — monitors one or more bookmark lists from the frequency manager",
    /* Author:      */ "SDR++ Contributors",
    /* Version:     */ 0, 11, 0,
    /* Max instances*/ -1
};

static ConfigManager config;

// ── SELCAL constants (ICAO Annex 10 / ARINC 714A) ───────────────────────────

static const double SELCAL_FREQS[16] = {
    312.6, 346.7, 384.6, 426.6, 473.2, 524.8, 582.1, 645.7,
    716.1, 794.3, 881.0, 977.2, 1083.9, 1202.3, 1333.5, 1479.1
};
static const char SELCAL_CHARS[16] = {
    'A','B','C','D','E','F','G','H','J','K','L','M','P','Q','R','S'
};

// VFO output sample rate fed to the Goertzel bank.
// Setting VFO_BW == VFO_SR bypasses RxVFO's internal lowpass filter entirely
// (filterNeeded = false when bandwidth == outSampleRate).
// The Goertzel bank provides its own narrow-band filtering.
static constexpr int    VFO_SR          = 48000;  // VFO output / Goertzel sample rate
static constexpr double VFO_BW          = 48000.0; // == VFO_SR → no RxVFO lowpass filter

static constexpr int    GOERTZEL_N      = 2400;   // block size = 50 ms @ 48 kHz
// Only 3 matching blocks (150 ms) needed to declare a valid pair.
// Pair transmissions last ~1000 ms, so there's 850 ms of headroom even for weak signals.
static constexpr int    MIN_PAIR_BLOCKS  = 3;
// MAX_TRANS_BLOCKS: from TRANSITION entry (~150ms into pair1) we cover the remaining
// pair1 tail (~850ms=17 blk) + inter-pair gap (~200ms=4 blk) + pair2 accumulation.
// 35 blocks (1750ms) gives ~14 blk of pair2 window — enough even with fading.
static constexpr int    MAX_TRANS_BLOCKS = 35;

// ── Bookmark from frequency_manager_config.json ──────────────────────────────

struct SelcalBookmark {
    std::string listName;  // which bookmark list this came from
    std::string name;
    double      frequency; // Hz
    int         fmMode;    // 0=NFM,1=WFM,2=AM,3=DSB,4=USB,5=CW,6=LSB,7=RAW

    // Unique slot key: list + "/" + name avoids collisions when two lists
    // contain a bookmark with the same name.
    std::string slotKey() const { return listName + "/" + name; }
};

// Map frequency_manager mode → demod mode (0=USB/default, 1=LSB, 2=AM)
static int fmModeToDemod(int fmMode) {
    if (fmMode == 2 || fmMode == 3) return 2; // AM / DSB
    if (fmMode == 6)                return 1; // LSB
    return 0;                                  // USB (default)
}

// ── Decode event ─────────────────────────────────────────────────────────────

struct DecodeEvent {
    std::string code;
    std::string bmName;
    double      freqHz;
    std::chrono::steady_clock::time_point when;
};

// ── Forward declaration ───────────────────────────────────────────────────────

class SelcalDecoderModule;

// ── Per-frequency monitoring slot ────────────────────────────────────────────

struct SelcalSlot {
    std::string bmName;
    double      frequency;
    int         demodMode; // 0=USB, 1=LSB, 2=AM

    SelcalDecoderModule* module = nullptr;

    dsp::stream<dsp::complex_t>*        iqIn = nullptr;
    dsp::channel::RxVFO*                vfo  = nullptr;
    dsp::sink::Handler<dsp::complex_t>* sink = nullptr;

    // Precomputed Goertzel coefficients (set once in spawnSlot)
    float gCoeff[16] = {};  // 2*cos(2π*k/N)
    float gCosW[16]  = {};  // cos(2π*k/N) — for final magnitude calculation
    float gSinW[16]  = {};  // sin(2π*k/N)

    // Running Goertzel state — dual I/Q channels
    float gS1_re[16] = {};
    float gS2_re[16] = {};
    float gS1_im[16] = {};
    float gS2_im[16] = {};
    int   gPos       = 0;

    // Per-block tone powers (updated after each GOERTZEL_N samples)
    float tonePower[16] = {};

    // ── Diagnostic (read from UI thread, written from DSP thread) ────────────
    float displayPower[16] = {};
    float displayThresh    = 0.0f;
    float displayRatio     = 0.0f; // top-2 / top-3 power ratio
    std::atomic<int> blockCount{0};
    std::mutex displayMtx;

    // State machine snapshot for UI (updated under displayMtx)
    char displaySmState[16]  = "IDLE";
    char displayPair1[4]     = "--";
    char displayPair2[4]     = "--";
    int  displayPairBlocks   = 0;
    int  displayTransBlocks  = 0;
    int  displayMissedBlocks = 0;
    // "Held" last interesting state — shown for 3 s after the SM resets,
    // so the user can read what pair codes were seen.
    char heldSmState[16] = "IDLE";
    char heldPair1[4]    = "--";
    char heldPair2[4]    = "--";
    std::chrono::steady_clock::time_point heldUntil{};

    // Threshold multiplier — atomic so UI thread writes don't need a lock.
    std::atomic<float> threshMult{2.0f};

    // ── SELCAL state machine ──────────────────────────────────────────────────
    enum class State { IDLE, PAIR1, TRANSITION, PAIR2 } smState = State::IDLE;
    int pair1[2]            = {-1, -1};
    int pair2[2]            = {-1, -1};
    int pairBlocks          = 0;
    int transBlocks         = 0;
    int missedBlocks        = 0;
    // Candidate pair2 tones in TRANSITION — must appear on 2+ blocks of the same
    // tones before we commit.  pair2CandidateFades counts consecutive silent blocks
    // seen while a candidate is in progress; we tolerate up to 3 (150 ms) so that
    // a briefly-fading pair2 can still accumulate, but reset after a longer silence
    // so that two widely-separated noise events can't fake a pair2.
    int pair2Candidate[2]     = {-1, -1};
    int pair2CandidateBlocks  = 0;
    int pair2CandidateFades   = 0;   // consecutive silent blocks since last candidate tone

    // Post-decode cooldown: after a successful decode the remaining pair2 tone
    // tail would otherwise get adopted as a new pair1. We sit in IDLE ignoring
    // all tones until the cooldown expires (one count per Goertzel block = 50 ms).
    // 16 blocks = 800 ms — long enough to outlast the tail, short enough that
    // a back-to-back second SELCAL (pair1 starts ~200 ms later) is not missed.
    int cooldownBlocks = 0;

    void runStateMachine(const int* tones, int n);
};

// ── Module ────────────────────────────────────────────────────────────────────

class SelcalDecoderModule : public ModuleManager::Instance {
public:
    SelcalDecoderModule(std::string name) {
        this->name = name;

        config.acquire();
        // Multi-list selection (v0.10+)
        if (config.conf[name].contains("selectedLists")) {
            for (auto& ln : config.conf[name]["selectedLists"])
                selectedLists.insert(ln.get<std::string>());
        }
        // Backward compat: single-list config from older versions
        else if (config.conf[name].contains("selectedList")) {
            selectedLists.insert(config.conf[name]["selectedList"].get<std::string>());
        }
        if (config.conf[name].contains("threshMult"))
            threshMult.store(config.conf[name]["threshMult"].get<float>(), std::memory_order_relaxed);
        if (config.conf[name].contains("showDebug"))
            showDebug = config.conf[name]["showDebug"].get<bool>();
        config.release();

        refreshLists();

        retuneHandler.ctx     = this;
        retuneHandler.handler = retuneHandlerFunc;
        sigpath::sourceManager.onRetune.bindHandler(&retuneHandler);

        gui::menu.registerEntry(name, menuHandler, this);
    }

    ~SelcalDecoderModule() {
        gui::menu.removeEntry(name);
        sigpath::sourceManager.onRetune.unbindHandler(&retuneHandler);
        if (running) stop();
    }

    void postInit() {}
    void enable()    { enabled = true; }
    void disable()   { enabled = false; }
    bool isEnabled() { return enabled; }

    void start() {
        std::lock_guard<std::mutex> lck(runMtx);
        if (running) return;
        running = true;

        lastKnownSr     = sigpath::iqFrontEnd.getSampleRate();
        lastKnownCenter = gui::waterfall.getCenterFrequency();

        std::lock_guard<std::mutex> slck(slotsMtx);
        spawnAllSlots();
    }

    void stop() {
        std::lock_guard<std::mutex> lck(runMtx);
        if (!running) return;
        running = false;

        std::lock_guard<std::mutex> slck(slotsMtx);
        destroyAllSlots();
    }

    void onDecoded(const std::string& code, const std::string& bName, double freqHz) {
        DecodeEvent ev{ code, bName, freqHz, std::chrono::steady_clock::now() };
        std::lock_guard<std::mutex> lck(eventsMtx);
        // De-duplicate: suppress identical code on same freq within 5 s
        for (auto& e : events) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(ev.when - e.when).count();
            if (age < 5 && e.code == code && std::abs(e.freqHz - freqHz) < 10.0) return;
        }
        events.push_front(ev);
        if (events.size() > 50) events.pop_back();
    }

private:
    // ── Load list names + bookmarks from frequency_manager_config.json ────────

    void refreshLists() {
        listNames.clear();
        currentBookmarks.clear();

        std::string path = core::args["root"].s() + "/frequency_manager_config.json";
        std::ifstream f(path);
        if (!f.is_open()) return;

        json cfg;
        try { f >> cfg; } catch (...) { return; }
        if (!cfg.contains("lists")) return;

        for (auto& [lname, list] : cfg["lists"].items())
            listNames.push_back(lname);
        std::sort(listNames.begin(), listNames.end());

        // Remove any selected list that no longer exists in the file
        std::set<std::string> validNames(listNames.begin(), listNames.end());
        for (auto it = selectedLists.begin(); it != selectedLists.end(); ) {
            if (!validNames.count(*it)) it = selectedLists.erase(it);
            else ++it;
        }

        loadBookmarksForSelectedLists(cfg);
    }

    // Collect bookmarks from every selected list into currentBookmarks.
    // Uses slotKey() = "listName/bookmarkName" so the same bookmark name
    // in two different lists produces two distinct slots.
    void loadBookmarksForSelectedLists(const json& cfg) {
        currentBookmarks.clear();
        if (!cfg.contains("lists")) return;

        for (auto& lname : selectedLists) {
            if (!cfg["lists"].contains(lname)) continue;
            if (!cfg["lists"][lname].contains("bookmarks")) continue;
            for (auto& [bname, bm] : cfg["lists"][lname]["bookmarks"].items()) {
                SelcalBookmark sbm;
                sbm.listName  = lname;
                sbm.name      = bname;
                sbm.frequency = bm.value("frequency", 0.0);
                sbm.fmMode    = bm.value("mode", 4);
                currentBookmarks.push_back(sbm);
            }
        }
        std::sort(currentBookmarks.begin(), currentBookmarks.end(),
            [](const SelcalBookmark& a, const SelcalBookmark& b) {
                return a.frequency < b.frequency;
            });
    }

    // ── Slot lifecycle ────────────────────────────────────────────────────────

    void spawnAllSlots() {
        for (auto& bm : currentBookmarks) {
            double offset = bm.frequency - lastKnownCenter;
            if (std::abs(offset) > lastKnownSr / 2.0 - VFO_BW / 2.0) continue;

            auto* slot      = new SelcalSlot();
            slot->bmName    = bm.name;
            slot->frequency = bm.frequency;
            slot->demodMode = fmModeToDemod(bm.fmMode);
            slot->module    = this;
            slot->threshMult.store(threshMult.load(std::memory_order_relaxed), std::memory_order_relaxed);
            spawnSlot(*slot, offset);
            activeSlots[bm.slotKey()] = slot;
        }
    }

    void destroyAllSlots() {
        for (auto& [key, slot] : activeSlots) { destroySlot(*slot); delete slot; }
        activeSlots.clear();
    }

    void spawnSlot(SelcalSlot& slot, double vfoOffset) {
        // Precompute Goertzel coefficients
        for (int t = 0; t < 16; t++) {
            double k        = std::round((double)GOERTZEL_N * SELCAL_FREQS[t] / VFO_SR);
            double w        = 2.0 * M_PI * k / GOERTZEL_N;
            slot.gCoeff[t]  = 2.0f * (float)std::cos(w);
            slot.gCosW[t]   = (float)std::cos(w);
            slot.gSinW[t]   = (float)std::sin(w);
        }

        // Create IQ stream, attach RxVFO, bind to IQ front-end.
        // VFO_BW == VFO_SR so RxVFO skips its internal lowpass filter —
        // all frequencies up to VFO_SR/2 pass through unfiltered.
        slot.iqIn = new dsp::stream<dsp::complex_t>();
        slot.vfo  = new dsp::channel::RxVFO(slot.iqIn, lastKnownSr, VFO_SR, VFO_BW, vfoOffset);
        sigpath::iqFrontEnd.bindIQStream(slot.iqIn);

        slot.sink = new dsp::sink::Handler<dsp::complex_t>(
            &slot.vfo->out, vfoComplexHandler, &slot);

        slot.vfo->start();
        slot.sink->start();
    }

    void destroySlot(SelcalSlot& slot) {
        slot.sink->stop();
        slot.vfo->stop();
        sigpath::iqFrontEnd.unbindIQStream(slot.iqIn);
        delete slot.sink; slot.sink = nullptr;
        delete slot.vfo;  slot.vfo  = nullptr;
        delete slot.iqIn; slot.iqIn = nullptr;
    }

    // ── Goertzel / IQ handler ─────────────────────────────────────────────────

    // Dual-channel Goertzel: run on both Re and Im separately, sum powers.
    // For a complex tone A·exp(j·(2πft + φ)):
    //   |Goertzel(Re)|² + |Goertzel(Im)|² = A²·N²/2  (phase-independent)
    static void processGoertzelSample(SelcalSlot* slot, float re_in, float im_in) {
        for (int t = 0; t < 16; t++) {
            float c = slot->gCoeff[t];

            float s_re      = re_in + c * slot->gS1_re[t] - slot->gS2_re[t];
            slot->gS2_re[t] = slot->gS1_re[t];
            slot->gS1_re[t] = s_re;

            float s_im      = im_in + c * slot->gS1_im[t] - slot->gS2_im[t];
            slot->gS2_im[t] = slot->gS1_im[t];
            slot->gS1_im[t] = s_im;
        }

        if (++slot->gPos < GOERTZEL_N) return;
        slot->gPos = 0;

        for (int t = 0; t < 16; t++) {
            float cosw = slot->gCosW[t];
            float sinw = slot->gSinW[t];

            float rr = slot->gS1_re[t] - slot->gS2_re[t] * cosw;
            float ri = slot->gS2_re[t] * sinw;
            float pwr_re = rr * rr + ri * ri;

            float ir = slot->gS1_im[t] - slot->gS2_im[t] * cosw;
            float ii = slot->gS2_im[t] * sinw;
            float pwr_im = ir * ir + ii * ii;

            slot->tonePower[t] = sqrtf(pwr_re + pwr_im) / (float)GOERTZEL_N;

            slot->gS1_re[t] = slot->gS2_re[t] = 0.0f;
            slot->gS1_im[t] = slot->gS2_im[t] = 0.0f;
        }

        // Sort bins by power (descending) to identify top-2 candidates.
        int order[16];
        for (int t = 0; t < 16; t++) order[t] = t;
        std::sort(order, order + 16,
            [&](int a, int b) { return slot->tonePower[a] > slot->tonePower[b]; });

        // Threshold from noise floor only — use the 14 LOWEST bins (order[2..15]).
        // Excluding the top-2 means a strong signal cannot inflate the baseline and
        // prevent detection of weaker subsequent signals ("auto-leveling" problem).
        float noiseSum = 0.0f, noiseSum2 = 0.0f;
        for (int i = 2; i < 16; i++) {
            float p = slot->tonePower[order[i]];
            noiseSum  += p;
            noiseSum2 += p * p;
        }
        float noiseMean  = noiseSum / 14.0f;
        float noiseVar   = noiseSum2 / 14.0f - noiseMean * noiseMean;
        float noiseSigma = sqrtf(std::max(0.0f, noiseVar));
        float tmult      = slot->threshMult.load(std::memory_order_relaxed);
        float thresh     = noiseMean + tmult * noiseSigma;

        // Precompute ratio here so it's available for both the snapshot and detection.
        float top3pwr = slot->tonePower[order[2]];
        float ratio   = slot->tonePower[order[1]] / std::max(top3pwr, 1e-12f);

        // Update diagnostic snapshot (non-blocking)
        if (slot->displayMtx.try_lock()) {
            for (int t = 0; t < 16; t++) slot->displayPower[t] = slot->tonePower[t];
            slot->displayThresh = thresh;
            slot->displayRatio  = ratio;
            const char* stateStr = "IDLE";
            switch (slot->smState) {
                case SelcalSlot::State::PAIR1:      stateStr = "PAIR1";      break;
                case SelcalSlot::State::TRANSITION: stateStr = "TRANSITION"; break;
                case SelcalSlot::State::PAIR2:      stateStr = "PAIR2";      break;
                default: break;
            }
            snprintf(slot->displaySmState, sizeof(slot->displaySmState), "%s", stateStr);
            // pair1 chars
            if (slot->pair1[0] >= 0 && slot->pair1[1] >= 0)
                snprintf(slot->displayPair1, sizeof(slot->displayPair1), "%c%c",
                    SELCAL_CHARS[slot->pair1[0]], SELCAL_CHARS[slot->pair1[1]]);
            else
                snprintf(slot->displayPair1, sizeof(slot->displayPair1), "--");
            // pair2 chars
            if (slot->pair2[0] >= 0 && slot->pair2[1] >= 0)
                snprintf(slot->displayPair2, sizeof(slot->displayPair2), "%c%c",
                    SELCAL_CHARS[slot->pair2[0]], SELCAL_CHARS[slot->pair2[1]]);
            else
                snprintf(slot->displayPair2, sizeof(slot->displayPair2), "--");
            slot->displayPairBlocks   = slot->pairBlocks;
            slot->displayTransBlocks  = slot->transBlocks;
            slot->displayMissedBlocks = slot->missedBlocks;
            // Hold the last non-IDLE state for 3 s so it's readable in the UI
            if (slot->smState != SelcalSlot::State::IDLE) {
                snprintf(slot->heldSmState, sizeof(slot->heldSmState), "%s", stateStr);
                snprintf(slot->heldPair1,   sizeof(slot->heldPair1),   "%s", slot->displayPair1);
                snprintf(slot->heldPair2,   sizeof(slot->heldPair2),   "%s", slot->displayPair2);
                slot->heldUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            }
            slot->displayMtx.unlock();
        }
        slot->blockCount.fetch_add(1, std::memory_order_relaxed);

        // ── Tone detection ────────────────────────────────────────────────────
        // A block is "active" when both the top-2 bins clear the adaptive
        // threshold (noiseMean + threshMult*noiseSigma).  The temporal state
        // machine (MIN_PAIR_BLOCKS, pair2CandidateBlocks, cooldownBlocks) plus
        // the elevated threshMult=2.0 default are sufficient to reject broadband
        // impulse noise (lightning, static crashes) without per-block ratio
        // guards that would reject weak legitimate signals at low SNR.
        // ratio/top3pwr are still captured above for the diagnostic overlay.

        int activeTones[2] = {-1, -1};
        int nActive = 0;
        if (slot->tonePower[order[0]] > thresh &&
            slot->tonePower[order[1]] > thresh) {
            activeTones[0] = std::min(order[0], order[1]);
            activeTones[1] = std::max(order[0], order[1]);
            nActive = 2;
        }

        slot->runStateMachine(activeTones, nActive);
    }

    static void vfoComplexHandler(dsp::complex_t* data, int count, void* ctx) {
        auto* slot = (SelcalSlot*)ctx;
        for (int i = 0; i < count; i++) {
            if (slot->demodMode == 2) {
                // AM: envelope magnitude, no imaginary component
                float mag = sqrtf(data[i].re * data[i].re + data[i].im * data[i].im);
                processGoertzelSample(slot, mag, 0.0f);
            } else {
                // USB / LSB: full complex IQ — phase-independent detection
                processGoertzelSample(slot, data[i].re, data[i].im);
            }
        }
    }

    // ── Retune handler ────────────────────────────────────────────────────────

    static void retuneHandlerFunc(double, void* ctx) {
        auto* _this = (SelcalDecoderModule*)ctx;
        if (!_this->running) return;

        _this->lastKnownSr     = sigpath::iqFrontEnd.getSampleRate();
        _this->lastKnownCenter = gui::waterfall.getCenterFrequency();

        std::lock_guard<std::mutex> lck(_this->slotsMtx);
        _this->destroyAllSlots();
        _this->spawnAllSlots();
    }

    // ── Menu / UI ─────────────────────────────────────────────────────────────

    static void menuHandler(void* ctx) {
        auto* _this = (SelcalDecoderModule*)ctx;
        float w = ImGui::GetContentRegionAvail().x;

        // ── Bookmark list multi-selector ─────────────────────────────────────
        ImGui::Text("Bookmark lists");
        ImGui::SameLine();
        if (ImGui::SmallButton(CONCAT("Refresh##selcal_ref_", _this->name))) {
            _this->refreshLists();
            if (_this->running) {
                std::lock_guard<std::mutex> lck(_this->slotsMtx);
                _this->destroyAllSlots();
                _this->spawnAllSlots();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-read frequency_manager_config.json");

        // Scrollable checkbox list — height shows ~4 rows, scrolls for more
        float listH = std::min((float)_this->listNames.size() * ImGui::GetFrameHeightWithSpacing() + 4.0f, 100.0f);
        ImGui::BeginChild(CONCAT("##selcal_listbox_", _this->name), { w, listH }, true);
        for (auto& lname : _this->listNames) {
            bool checked = _this->selectedLists.count(lname) > 0;
            if (ImGui::Checkbox(CONCAT("##selcal_chk_", lname + _this->name), &checked)) {
                if (checked) _this->selectedLists.insert(lname);
                else         _this->selectedLists.erase(lname);

                // Rebuild bookmarks from updated selection
                std::string path = core::args["root"].s() + "/frequency_manager_config.json";
                std::ifstream f2(path);
                if (f2.is_open()) {
                    json cfg2;
                    try { f2 >> cfg2; _this->loadBookmarksForSelectedLists(cfg2); }
                    catch (...) {}
                }

                if (_this->running) {
                    std::lock_guard<std::mutex> lck(_this->slotsMtx);
                    _this->destroyAllSlots();
                    _this->spawnAllSlots();
                }

                // Save selection to config
                config.acquire();
                config.conf[_this->name]["selectedLists"] = json::array();
                for (auto& sl : _this->selectedLists)
                    config.conf[_this->name]["selectedLists"].push_back(sl);
                config.release(true);
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(lname.c_str());
        }
        ImGui::EndChild();

        // Bookmark / active slot counts
        {
            int total  = (int)_this->currentBookmarks.size();
            int active = 0;
            { std::lock_guard<std::mutex> lck(_this->slotsMtx); active = (int)_this->activeSlots.size(); }
            if (_this->running && total > 0)
                ImGui::TextDisabled("%d bookmark%s | %d in band", total, total==1?"":"s", active);
            else
                ImGui::TextDisabled("%d bookmark%s selected", total, total==1?"":"s");
        }

        // ── Sensitivity slider ────────────────────────────────────────────────
        ImGui::LeftLabel("Sensitivity");
        ImGui::FillWidth();
        float tm = _this->threshMult.load(std::memory_order_relaxed);
        if (ImGui::SliderFloat(CONCAT("##selcal_thresh_", _this->name), &tm, 0.5f, 3.0f, "%.2f")) {
            _this->threshMult.store(tm, std::memory_order_relaxed);
            // Update all active slots
            {
                std::lock_guard<std::mutex> lck(_this->slotsMtx);
                for (auto& [k, s] : _this->activeSlots)
                    s->threshMult.store(tm, std::memory_order_relaxed);
            }
            config.acquire();
            config.conf[_this->name]["threshMult"] = tm;
            config.release(true);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Lower = more sensitive (catches weak signals, more false positives)\n"
                              "Higher = fewer false positives, state stays IDLE during silence\n"
                              "Default 2.0 — lower only if missing very weak signals");

        ImGui::Separator();

        // ── Start / Stop ──────────────────────────────────────────────────────
        if (_this->selectedLists.empty() || _this->currentBookmarks.empty())
            style::beginDisabled();

        if (_this->running) {
            if (ImGui::Button(CONCAT("Stop##selcal_", _this->name), { w, 0 }))
                _this->stop();
        } else {
            if (ImGui::Button(CONCAT("Start##selcal_", _this->name), { w, 0 }))
                _this->start();
        }

        if (_this->selectedLists.empty() || _this->currentBookmarks.empty())
            style::endDisabled();

        // ── Debug panel toggle ────────────────────────────────────────────────
        if (ImGui::Checkbox(CONCAT("Show debug##selcal_dbg_", _this->name), &_this->showDebug)) {
            config.acquire();
            config.conf[_this->name]["showDebug"] = _this->showDebug;
            config.release(true);
        }

        // ── Live tone-power diagnostics ───────────────────────────────────────
        if (_this->running && _this->showDebug) {
            ImGui::Separator();
            ImGui::Text("Tone power (live)");

            std::lock_guard<std::mutex> slck(_this->slotsMtx);
            if (_this->activeSlots.empty()) {
                ImGui::TextDisabled("No slots in band");
            } else {
                for (auto& [key, slot] : _this->activeSlots) {
                    int bc = slot->blockCount.load(std::memory_order_relaxed);
                    ImGui::TextDisabled("%.3f MHz  [%d blk]",
                        slot->frequency / 1e6, bc);

                    float pwr[16];
                    float thresh = 0.0f;
                    float ratio  = 0.0f;
                    {
                        std::lock_guard<std::mutex> dlck(slot->displayMtx);
                        for (int t = 0; t < 16; t++) pwr[t] = slot->displayPower[t];
                        thresh = slot->displayThresh;
                        ratio  = slot->displayRatio;
                    }

                    // Find max for normalising bars
                    float maxPwr = 1e-9f;
                    for (int t = 0; t < 16; t++) maxPwr = std::max(maxPwr, pwr[t]);

                    // Two rows of 8: first 8 tones (A-H), then 8 (J-S)
                    float barW = (w - ImGui::GetStyle().ItemSpacing.x * 7.0f) / 8.0f;
                    for (int row = 0; row < 2; row++) {
                        for (int col = 0; col < 8; col++) {
                            int t = row * 8 + col;
                            if (col > 0) ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);

                            float frac = pwr[t] / maxPwr;
                            bool  active = (pwr[t] > thresh);
                            ImVec4 col4 = active
                                ? ImVec4(0.0f, 1.0f, 0.4f, 1.0f)   // green = above threshold
                                : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);  // grey  = below threshold
                            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col4);
                            char lbl[8];
                            snprintf(lbl, sizeof(lbl), "%c", SELCAL_CHARS[t]);
                            ImGui::ProgressBar(frac, ImVec2(barW, 18.0f), lbl);
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::TextDisabled("thresh=%.4f  max=%.4f  ratio=%.2f", thresh, maxPwr, ratio);

                    // State machine status
                    char smState[16]; char p1[4]; char p2[4];
                    int pb, tb, mb;
                    {
                        std::lock_guard<std::mutex> dlck2(slot->displayMtx);
                        snprintf(smState, sizeof(smState), "%s", slot->displaySmState);
                        snprintf(p1, sizeof(p1), "%s", slot->displayPair1);
                        snprintf(p2, sizeof(p2), "%s", slot->displayPair2);
                        pb = slot->displayPairBlocks;
                        tb = slot->displayTransBlocks;
                        mb = slot->displayMissedBlocks;
                    }
                    ImVec4 stateCol = { 0.5f, 0.5f, 0.5f, 1.0f };
                    if      (strcmp(smState, "PAIR1")      == 0) stateCol = { 1.0f, 0.8f, 0.0f, 1.0f };
                    else if (strcmp(smState, "TRANSITION") == 0) stateCol = { 0.0f, 0.8f, 1.0f, 1.0f };
                    else if (strcmp(smState, "PAIR2")      == 0) stateCol = { 0.0f, 1.0f, 0.4f, 1.0f };
                    ImGui::TextColored(stateCol, "%s", smState);
                    ImGui::SameLine();
                    ImGui::TextDisabled(" p1=%s p2=%s  blk=%d trans=%d miss=%d",
                        p1, p2, pb, tb, mb);

                    // Show the last non-IDLE state for 3 s after it clears
                    char hState[16]; char hp1[4]; char hp2[4];
                    auto heldUntil = slot->heldUntil;
                    {
                        std::lock_guard<std::mutex> dlck3(slot->displayMtx);
                        snprintf(hState, sizeof(hState), "%s", slot->heldSmState);
                        snprintf(hp1, sizeof(hp1), "%s", slot->heldPair1);
                        snprintf(hp2, sizeof(hp2), "%s", slot->heldPair2);
                    }
                    if (std::chrono::steady_clock::now() < heldUntil) {
                        ImGui::TextDisabled("  last: %s %s-%s", hState, hp1, hp2);
                    }
                }
            }
        }

        ImGui::Separator();

        // ── Decoded SELCAL log ────────────────────────────────────────────────
        ImGui::Text("Decoded SELCALs");
        if (ImGui::Button(CONCAT("Clear##selcal_clr_", _this->name), { w, 0 })) {
            std::lock_guard<std::mutex> lck(_this->eventsMtx);
            _this->events.clear();
        }

        ImGui::BeginChild(CONCAT("##selcal_log_", _this->name), { w, 200.0f }, true);
        {
            std::lock_guard<std::mutex> lck(_this->eventsMtx);
            for (auto& ev : _this->events) {
                auto wall = std::chrono::system_clock::now() -
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        std::chrono::steady_clock::now() - ev.when);
                time_t     t  = std::chrono::system_clock::to_time_t(wall);
                struct tm* lt = localtime(&t);
                char ts[16];
                snprintf(ts, sizeof(ts), "%02d:%02d:%02d",
                         lt->tm_hour, lt->tm_min, lt->tm_sec);

                ImGui::TextUnformatted(ts);
                ImGui::SameLine();
                ImGui::TextColored({ 0.0f, 1.0f, 0.5f, 1.0f }, "%s", ev.code.c_str());
                ImGui::SameLine();
                ImGui::Text("%.3f MHz", ev.freqHz / 1e6);
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)", ev.bmName.c_str());
            }
        }
        ImGui::EndChild();
    }

    // ── Members ───────────────────────────────────────────────────────────────

    std::string name;
    bool enabled = true;
    bool running = false;
    std::mutex runMtx;

    double lastKnownSr     = 0.0;
    double lastKnownCenter = 0.0;

    std::vector<std::string> listNames;       // all lists found in frequency_manager_config.json (sorted)
    std::set<std::string>    selectedLists;   // which lists are currently checked
    std::vector<SelcalBookmark> currentBookmarks; // combined bookmarks from all selected lists

    std::mutex                         slotsMtx;
    std::map<std::string, SelcalSlot*> activeSlots;

    std::mutex              eventsMtx;
    std::deque<DecodeEvent> events;

    std::atomic<float> threshMult{2.0f};  // threshold = mean + threshMult*sigma
    bool showDebug = true;               // show/hide live tone-power diagnostic panel

    EventHandler<double> retuneHandler;
};

// ── SelcalSlot::runStateMachine ───────────────────────────────────────────────

void SelcalSlot::runStateMachine(const int* tones, int n) {
    switch (smState) {

    case State::IDLE:
        // Burn off the post-decode cooldown before accepting a new pair1.
        // This prevents the remaining tail of the just-decoded pair2 from
        // being adopted as pair1 of a phantom second decode.
        if (cooldownBlocks > 0) {
            // If the recently-decoded pair2 tones are still present (fading
            // signal whose tail extends past our initial cooldown window),
            // keep refreshing: wait at least 8 blocks (400 ms) after the
            // LAST time those tones are seen before accepting a new pair1.
            if (n == 2 && tones[0] == pair2[0] && tones[1] == pair2[1])
                cooldownBlocks = std::max(cooldownBlocks, 8);
            --cooldownBlocks;
            break;
        }
        if (n == 2) {
            pair1[0] = tones[0]; pair1[1] = tones[1];
            pairBlocks = 1; missedBlocks = 0;
            smState = State::PAIR1;
        }
        break;

    case State::PAIR1:
        if (n == 2 && tones[0] == pair1[0] && tones[1] == pair1[1]) {
            missedBlocks = 0;
            if (++pairBlocks >= MIN_PAIR_BLOCKS) {
                transBlocks = 0;
                pair2CandidateBlocks = 0;
                pair2CandidateFades  = 0;
                pair2Candidate[0] = pair2Candidate[1] = -1;
                smState = State::TRANSITION;
            }
        } else {
            // Up to 9 consecutive missed blocks (~450 ms) before giving up,
            // giving weak/fading signals time to keep accumulating.
            if (++missedBlocks > 8) {
                smState = State::IDLE;
                if (n == 2) {
                    pair1[0] = tones[0]; pair1[1] = tones[1];
                    pairBlocks = 1; missedBlocks = 0;
                    smState = State::PAIR1;
                }
            }
        }
        break;

    case State::TRANSITION:
        if (n == 2 && tones[0] == pair1[0] && tones[1] == pair1[1]) {
            // Still seeing pair1 tones — reset the pair2 candidate
            pair2CandidateBlocks = 0;
            pair2CandidateFades  = 0;
            if (++transBlocks > MAX_TRANS_BLOCKS) smState = State::IDLE;
        }
        else if (n == 0) {
            // Silence / fade during candidate accumulation.
            // Tolerate up to 3 consecutive silent blocks (150 ms) so that a
            // briefly-fading pair2 signal can still accumulate across the gap.
            // After 4+ silent blocks the signal has truly ended; reset so that
            // two widely-separated noise events can't fake a pair2.
            if (pair2CandidateBlocks > 0) {
                if (++pair2CandidateFades > 3) {
                    pair2CandidateBlocks = 0;
                    pair2CandidateFades  = 0;
                }
            }
            if (++transBlocks > MAX_TRANS_BLOCKS) smState = State::IDLE;
        }
        else if (n == 2) {
            // Candidate pair2 tones — require 2 blocks of the SAME new tones
            // (possibly with brief fades between them) before committing.
            if (tones[0] == pair2Candidate[0] && tones[1] == pair2Candidate[1]) {
                pair2CandidateFades = 0;  // signal came back — reset fade counter
                if (++pair2CandidateBlocks >= 2) {
                    pair2[0] = pair2Candidate[0]; pair2[1] = pair2Candidate[1];
                    pairBlocks = pair2CandidateBlocks; missedBlocks = 0;
                    smState = State::PAIR2;
                }
            } else {
                // Different tones — start the candidate over
                pair2Candidate[0] = tones[0]; pair2Candidate[1] = tones[1];
                pair2CandidateBlocks = 1;
                pair2CandidateFades  = 0;
                if (++transBlocks > MAX_TRANS_BLOCKS) smState = State::IDLE;
            }
        }
        else {
            smState = State::IDLE;
        }
        break;

    case State::PAIR2:
        if (n == 2 && tones[0] == pair2[0] && tones[1] == pair2[1]) {
            missedBlocks = 0;
            if (++pairBlocks >= MIN_PAIR_BLOCKS) {

                int a = std::min(pair1[0], pair1[1]);
                int b = std::max(pair1[0], pair1[1]);
                int c = std::min(pair2[0], pair2[1]);
                int d = std::max(pair2[0], pair2[1]);
                char code[6] = {
                    SELCAL_CHARS[a], SELCAL_CHARS[b], '-',
                    SELCAL_CHARS[c], SELCAL_CHARS[d], '\0'
                };
                module->onDecoded(std::string(code), bmName, frequency);
                cooldownBlocks = 16; // 16 × 50 ms = 800 ms tail-drain
                smState = State::IDLE;
            }
        } else {
            if (++missedBlocks > 8) smState = State::IDLE;
        }
        break;
    }
}

// ── Module entry points ───────────────────────────────────────────────────────

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/selcal_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SelcalDecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (SelcalDecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
