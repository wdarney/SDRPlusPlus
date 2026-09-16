#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <dsp/sink/handler_sink.h>

#include "vdl2_dsp.h"
#include "aviation_jsonl.h"
#include "channel_input.h"
#include "acars_dsp.h"
#include "adsb_dsp.h"

extern "C" {
#include <libacars/libacars.h>
#include <libacars/vstring.h>
}

#include <deque>
#include <algorithm>
#include <array>
#include <fstream>
#include <atomic>
#include <sqlite3.h>

// UDP
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET sock_t;
#define INVALID_SOCK INVALID_SOCKET
#define CLOSE_SOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCK -1
#define CLOSE_SOCK close
#endif

#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "vdl2_decoder",
    /* Description:     */ "Aviation data link decoder (VDL2 + ACARS)",
    /* Author:          */ "SDR++ Custom",
    /* Version:         */ 0, 3, 0,
    /* Max instances    */ -1
};

enum ChannelMode { MODE_VDL2, MODE_ACARS, MODE_ADSB };

static const struct {
    const char* label;
    uint32_t freq;
    ChannelMode mode;
    bool defaultEnabled;
    float bwOverride;   // 0 = use mode default
    float srOverride;   // 0 = use mode default
} ALL_CHANNELS[] = {
    // ADS-B / Mode S
    { "1090.000 ADS-B",              1090000000, MODE_ADSB,  false, 0, 0 },
    { "133.334 ADS-B (?)",           133334000,  MODE_ADSB,  true,  50000, 2000000 },
    // ACARS (AM/MSK) — North America
    { "129.125 ACARS USA/CAN",       129125000, MODE_ACARS, false, 0, 0 },
    { "130.025 ACARS USA/CAN",       130025000, MODE_ACARS, false, 0, 0 },
    { "130.425 ACARS USA",           130425000, MODE_ACARS, false, 0, 0 },
    { "130.450 ACARS USA/CAN",       130450000, MODE_ACARS, false, 0, 0 },
    { "131.125 ACARS USA",           131125000, MODE_ACARS, false, 0, 0 },
    // ACARS (AM/MSK) — Worldwide / Regional
    { "131.450 ACARS Japan",         131450000, MODE_ACARS, false, 0, 0 },
    { "131.475 ACARS Air Canada",    131475000, MODE_ACARS, false, 0, 0 },
    { "131.525 ACARS Europe 2",      131525000, MODE_ACARS, false, 0, 0 },
    { "131.550 ACARS Worldwide",     131550000, MODE_ACARS, false, 0, 0 },
    { "131.725 ACARS Europe 1",      131725000, MODE_ACARS, false, 0, 0 },
    { "131.825 ACARS Europe",        131825000, MODE_ACARS, false, 0, 0 },
    { "131.850 ACARS Europe",        131850000, MODE_ACARS, false, 0, 0 },
    // VDL2 / ACARS mixed band (136 MHz)
    { "136.300 VDL2",                136300000, MODE_VDL2,  true,  0, 0 },
    { "136.100 VDL2 SITA",           136100000, MODE_VDL2,  true,  0, 0 },
    { "136.650 VDL2 SITA Transit",   136650000, MODE_VDL2,  true,  0, 0 },
    { "136.675 VDL2 SITA",           136675000, MODE_VDL2,  true,  0, 0 },
    { "136.700 ACARS USA",           136700000, MODE_ACARS, false, 0, 0 },
    { "136.725 VDL2 ARINC EU",      136725000, MODE_VDL2,  true,  0, 0 },
    { "136.750 ACARS USA/EU",        136750000, MODE_ACARS, false, 0, 0 },
    { "136.775 VDL2 SITA EU",       136775000, MODE_VDL2,  true,  0, 0 },
    { "136.800 ACARS USA",           136800000, MODE_ACARS, false, 0, 0 },
    { "136.825 VDL2 ARINC",         136825000, MODE_VDL2,  true,  0, 0 },
    { "136.850 ACARS SITA NA",       136850000, MODE_ACARS, false, 0, 0 },
    { "136.875 VDL2 SITA EU",       136875000, MODE_VDL2,  true,  0, 0 },
    { "136.900 ACARS SITA EU 2",     136900000, MODE_ACARS, false, 0, 0 },
    { "136.925 ACARS ARINC EU",      136925000, MODE_ACARS, false, 0, 0 },
    { "136.975 VDL2 CSC World",      136975000, MODE_VDL2,  true,  0, 0 },
};
static const int ALL_CHANNEL_COUNT = sizeof(ALL_CHANNELS) / sizeof(ALL_CHANNELS[0]);

static const int MAX_MESSAGES = 500;
static const float VDL2_VFO_BANDWIDTH = 25000.f;
static const float ADSB_VFO_BANDWIDTH = 2000000.f;
static const float ADSB_VFO_SAMPLERATE = 2000000.f;
static const int DEFAULT_UDP_PORT = 5555;

static ConfigManager config;

// ============================================================================
// UDP sender
// ============================================================================

class UDPSender {
public:
    bool open(const char* host, int port) {
        closeSocket();
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCK) return false;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host, &addr.sin_addr);
        active = true;
        return true;
    }
    void closeSocket() {
        if (sock != INVALID_SOCK) { CLOSE_SOCK(sock); sock = INVALID_SOCK; }
        active = false;
    }
    void send(const std::string& data) {
        if (!active) return;
        sendto(sock, data.c_str(), data.size(), 0, (struct sockaddr*)&addr, sizeof(addr));
    }
    bool isActive() const { return active; }
    ~UDPSender() { closeSocket(); }
private:
    sock_t sock = INVALID_SOCK;
    struct sockaddr_in addr = {};
    bool active = false;
};

// ============================================================================
// SQLite message store
// ============================================================================

class MessageDB {
public:
    bool open(const std::string& path) {
        closeDB();
        int rc = sqlite3_open(path.c_str(), &db);
        if (rc != SQLITE_OK) { db = nullptr; return false; }

        // Create table + index
        const char* sql =
            "CREATE TABLE IF NOT EXISTS messages ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  timestamp TEXT NOT NULL,"
            "  timestamp_unix REAL NOT NULL,"
            "  freq REAL NOT NULL,"
            "  type TEXT NOT NULL,"
            "  snr REAL,"
            "  fec INTEGER,"
            "  ppm REAL,"
            "  text TEXT NOT NULL,"
            "  json TEXT NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_messages_time ON messages(timestamp_unix);"
            "CREATE INDEX IF NOT EXISTS idx_messages_type ON messages(type);"
            "PRAGMA journal_mode=WAL;"    // WAL mode for concurrent read/write
            "PRAGMA synchronous=NORMAL;"; // faster writes, still safe with WAL

        char* err = nullptr;
        sqlite3_exec(db, sql, nullptr, nullptr, &err);
        if (err) sqlite3_free(err);

        // Prepare insert statement
        const char* insertSQL =
            "INSERT INTO messages (timestamp, timestamp_unix, freq, type, snr, fec, ppm, text, json) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
        sqlite3_prepare_v2(db, insertSQL, -1, &insertStmt, nullptr);

        active = true;
        return true;
    }

    void insert(const VDL2Message& msg, const std::string& jsonStr) {
        if (!active || !insertStmt) return;

        char timebuf[64];
        time_t t = (time_t)msg.timestamp;
        struct tm* tm_info = gmtime(&t);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%SZ", tm_info);

        sqlite3_reset(insertStmt);
        sqlite3_bind_text(insertStmt, 1, timebuf, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(insertStmt, 2, msg.timestamp);
        sqlite3_bind_double(insertStmt, 3, (double)msg.freq / 1e6);
        sqlite3_bind_text(insertStmt, 4, msg.is_acars ? "ACARS" : "VDL2", -1, SQLITE_STATIC);
        sqlite3_bind_double(insertStmt, 5, msg.snr);
        sqlite3_bind_int(insertStmt, 6, msg.num_fec_corrections);
        sqlite3_bind_double(insertStmt, 7, msg.ppm_error);
        sqlite3_bind_text(insertStmt, 8, msg.formatted_text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insertStmt, 9, jsonStr.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(insertStmt);
    }

    // Prune messages older than maxAge seconds
    void prune(double maxAgeSeconds) {
        if (!active) return;
        char sql[128];
        snprintf(sql, sizeof(sql),
            "DELETE FROM messages WHERE timestamp_unix < %.3f",
            (double)time(nullptr) - maxAgeSeconds);
        sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
    }

    void closeDB() {
        if (insertStmt) { sqlite3_finalize(insertStmt); insertStmt = nullptr; }
        if (db) { sqlite3_close(db); db = nullptr; }
        active = false;
    }

    bool isActive() const { return active; }
    ~MessageDB() { closeDB(); }

private:
    sqlite3* db = nullptr;
    sqlite3_stmt* insertStmt = nullptr;
    bool active = false;
};

// ============================================================================
// Per-channel state
// ============================================================================

struct ChannelSlot {
    ~ChannelSlot() { input.stop(); } // join before decoder members are destroyed
    VFOManager::VFO* vfo = nullptr;
    VDL2ChannelInput input;
    std::mutex decoderMtx;
    VDL2Channel vdl2Decoder;
    ACARSChannel acarsDecoder;
    ADSBChannel adsbDecoder;
    ChannelMode mode = MODE_VDL2;
    bool stopping = false; // lifecycle thread only
    bool active = false; // lifecycle thread only; workers use input.accepting()
    std::string vfoName;

    int getMessageCount() const {
        if (mode == MODE_VDL2) return vdl2Decoder.getMessageCount();
        if (mode == MODE_ACARS) return acarsDecoder.getMessageCount();
        return adsbDecoder.getMessageCount();
    }
    int getSyncCount() const {
        if (mode == MODE_VDL2) return vdl2Decoder.getSyncCount();
        if (mode == MODE_ACARS) return acarsDecoder.getSyncCount();
        return adsbDecoder.getSyncCount();
    }
    long long getSamplesProcessed() const {
        if (mode == MODE_VDL2) return vdl2Decoder.getSamplesProcessed();
        if (mode == MODE_ACARS) return acarsDecoder.getSamplesProcessed();
        return adsbDecoder.getSamplesProcessed();
    }
};

// ============================================================================
// Module
// ============================================================================

class VDL2DecoderModule : public ModuleManager::Instance {
public:
    VDL2DecoderModule(std::string name) : name(name) {
        // Load config
        config.acquire();
        if (!config.conf.contains(name)) {
            config.conf[name] = json({});
        }
        if (config.conf[name].contains("channelEnabled")) {
            auto& ce = config.conf[name]["channelEnabled"];
            if ((int)ce.size() < ALL_CHANNEL_COUNT) {
                // Stale config — channel list grew. Use defaults for all.
                for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
                    channelEnabled[i] = ALL_CHANNELS[i].defaultEnabled;
                }
                config.conf[name].erase("channelEnabled");
            }
            else {
                for (int i = 0; i < ALL_CHANNEL_COUNT && i < (int)ce.size(); i++) {
                    channelEnabled[i] = ce[i].get<bool>();
                }
            }
        }
        else {
            for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
                channelEnabled[i] = ALL_CHANNELS[i].defaultEnabled;
            }
        }
        if (config.conf[name].contains("udpEnabled")) udpEnabled = config.conf[name]["udpEnabled"];
        if (config.conf[name].contains("udpPort")) udpPort = config.conf[name]["udpPort"];
        if (config.conf[name].contains("fileEnabled")) fileEnabled = config.conf[name]["fileEnabled"];
        if (config.conf[name].contains("filePath")) filePath = config.conf[name]["filePath"].get<std::string>();
        if (config.conf[name].contains("dbEnabled")) dbEnabled = config.conf[name]["dbEnabled"];
        if (config.conf[name].contains("dbPath")) dbPath = config.conf[name]["dbPath"].get<std::string>();
        if (config.conf[name].contains("dbRetentionDays")) dbRetentionDays = config.conf[name]["dbRetentionDays"];
        config.release(false);

        // Set up message callback
        auto msgCb = [this](ChannelSlot& slot, const VDL2Message& msg) {
            std::lock_guard<std::mutex> callbackLock(callbackMtx);
            if (!acceptingCallbacks || !slot.input.accepting()) return;
            // Add to display buffer
            {
                std::lock_guard<std::mutex> lock(msgMtx);
                messages.push_front(msg);
                if ((int)messages.size() > MAX_MESSAGES) messages.pop_back();
                totalMessages++;
                newMessage = true;
            }

            // The decoder owns serialization; outputs consume it unchanged.
            const std::string& j = msg.json_text;

            // UDP output
            if (udpEnabled && udpSender.isActive()) {
                udpSender.send(j + "\n");
            }

            // File output
            {
                std::lock_guard<std::mutex> lock(fileMtx);
                if (fileEnabled && !filePath.empty() && !appendAviationJSONL(filePath, j)) ++fileWriteErrors;
            }

            // SQLite output
            if (dbEnabled && messageDB.isActive()) {
                std::lock_guard<std::mutex> lock(dbMtx);
                messageDB.insert(msg, j);
                if (totalMessages % 1000 == 0) {
                    messageDB.prune(dbRetentionDays * 86400.0);
                }
            }
        };

        for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
            slots[i].vfoName = name + "_ch" + std::to_string(i);
            auto cb = [this, i, msgCb](const VDL2Message& msg) { msgCb(slots[i], msg); };
            slots[i].vdl2Decoder.setMessageCallback(cb);
            slots[i].acarsDecoder.setMessageCallback(cb);
            slots[i].adsbDecoder.setMessageCallback(cb);
        }

        gui::menu.registerEntry(name, menuHandler, this, this);
    }

    ~VDL2DecoderModule() {
        stop(); // unconditional: also cleans up a partially started module
        udpSender.closeSocket();
        gui::menu.removeEntry(name);
    }

    void postInit() override {}
    void enable() override { std::lock_guard<std::recursive_mutex> lock(lifecycleMtx); enabled = true; }
    void disable() override { std::lock_guard<std::recursive_mutex> lock(lifecycleMtx); stop(); enabled = false; }
    bool isEnabled() override { std::lock_guard<std::recursive_mutex> lock(lifecycleMtx); return enabled; }

private:
    static void sinkHandler(dsp::complex_t* data, int count, void* ctx) {
        ChannelSlot* slot = (ChannelSlot*)ctx;
        if (!slot->input.accepting()) return;
        std::lock_guard<std::mutex> decoderLock(slot->decoderMtx);
        if (!slot->input.accepting()) return;
        if (slot->mode == MODE_VDL2) {
            slot->vdl2Decoder.processIQ((const float*)data, count);
        } else if (slot->mode == MODE_ACARS) {
            slot->acarsDecoder.processIQ((const float*)data, count);
        } else {
            slot->adsbDecoder.processIQ((const float*)data, count);
        }
    }

    // Reposition all active VFOs to match current SDR center frequency
    void repositionVFOs() {
        std::lock_guard<std::recursive_mutex> lock(lifecycleMtx);
        double sdrCenter = gui::waterfall.getCenterFrequency();
        for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
            if (slots[i].active && slots[i].vfo) {
                double offset = (double)ALL_CHANNELS[i].freq - sdrCenter;
                slots[i].vfo->setOffset(offset);
            }
        }
    }

    void startChannel(int idx) {
        std::lock_guard<std::recursive_mutex> lock(lifecycleMtx);
        if (stopping) return;
        if (idx < 0 || idx >= ALL_CHANNEL_COUNT) return;
        if (slots[idx].active || slots[idx].stopping || slots[idx].vfo) return;

        double bw = gui::waterfall.getBandwidth();
        double sdrCenter = gui::waterfall.getCenterFrequency();
        double channelOffset = (double)ALL_CHANNELS[idx].freq - sdrCenter;
        if (std::abs(channelOffset) > bw / 2.0) return;

        // Reposition existing VFOs in case center has shifted
        repositionVFOs();

        slots[idx].mode = ALL_CHANNELS[idx].mode;

        double sampleRate, vfoBw;
        if (ALL_CHANNELS[idx].mode == MODE_VDL2) {
            sampleRate = VDL2_SAMPLE_RATE;
            vfoBw = VDL2_VFO_BANDWIDTH;
            slots[idx].vdl2Decoder.init(ALL_CHANNELS[idx].freq, VDL2_SAMPLE_RATE);
        } else if (ALL_CHANNELS[idx].mode == MODE_ACARS) {
            sampleRate = ACARS_INTRATE;
            vfoBw = 10000.0;
            slots[idx].acarsDecoder.init(ALL_CHANNELS[idx].freq);
        } else {
            sampleRate = ADSB_VFO_SAMPLERATE;
            vfoBw = ADSB_VFO_BANDWIDTH;
            slots[idx].adsbDecoder.init(ALL_CHANNELS[idx].freq);
        }
        // Per-channel overrides
        if (ALL_CHANNELS[idx].bwOverride > 0) vfoBw = ALL_CHANNELS[idx].bwOverride;
        if (ALL_CHANNELS[idx].srOverride > 0) sampleRate = ALL_CHANNELS[idx].srOverride;

        try {
            slots[idx].vfo = sigpath::vfoManager.createVFO(
                slots[idx].vfoName, ImGui::WaterfallVFO::REF_CENTER,
                channelOffset, vfoBw, sampleRate, vfoBw, vfoBw, true);
            if (!slots[idx].vfo) throw std::runtime_error("VDL2 VFO name already registered");
            slots[idx].input.start(slots[idx].vfo->output, sinkHandler, &slots[idx]);
            slots[idx].active = true;
        } catch (...) {
            // Cleanup, then propagate startup failures. Never ignore stop errors.
            stopChannel(idx);
            throw;
        }
    }

    void releaseChannel(ChannelSlot& slot) {
        // The Handler is already joined/destroyed. VFO deletion now stops/joins
        // the producer and removes its registration before freeing its stream.
        if (!slot.vfo && !slot.active) { slot.stopping = false; return; }
        if (slot.vfo) {
            auto* vfo = slot.vfo;
            slot.vfo = nullptr;
            sigpath::vfoManager.deleteVFO(vfo);
        }
        slot.vdl2Decoder.reset();
        slot.acarsDecoder.reset();
        slot.adsbDecoder.reset();
        slot.active = false;
        slot.stopping = false;
    }

    void stopChannel(int idx) {
        std::lock_guard<std::recursive_mutex> lock(lifecycleMtx);
        if (idx < 0 || idx >= ALL_CHANNEL_COUNT) return;
        if (slots[idx].stopping) return;
        slots[idx].stopping = true;
        // No active-only guard: partial startup can own a VFO/Handler already.
        slots[idx].input.rejectWork();
        slots[idx].input.stop();
        releaseChannel(slots[idx]);
    }

    void start() {
        std::lock_guard<std::recursive_mutex> lock(lifecycleMtx);
        if (running || stopping || !enabled) return;
        try {
            {
                std::lock_guard<std::mutex> callbackLock(callbackMtx);
                if (udpEnabled) udpSender.open("127.0.0.1", udpPort);
                if (!dbPath.empty()) messageDB.open(dbPath);
                acceptingCallbacks = true;
            }
            for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
                if (channelEnabled[i]) startChannel(i);
            }
            running = true;
        } catch (...) {
            stop(); // roll back every previously started channel, too
            throw;
        }
    }

    void stop() {
        std::lock_guard<std::recursive_mutex> lock(lifecycleMtx);
        if (stopping) return;
        stopping = true;
        // Reject all channels first, including those whose workers are still
        // finishing a decode. They cannot publish another message.
        stopVDL2Channels(slots, [this] {
            std::lock_guard<std::mutex> callbackLock(callbackMtx);
            acceptingCallbacks = false; // waits for any admitted output callback
        }, [this](ChannelSlot& slot) { releaseChannel(slot); });
        {
            std::lock_guard<std::mutex> callbackLock(callbackMtx);
            udpSender.closeSocket();
            messageDB.closeDB();
        }
        running = false;
        stopping = false;
    }

    int countActiveChannels() {
        int c = 0;
        for (int i = 0; i < ALL_CHANNEL_COUNT; i++) if (slots[i].active) c++;
        return c;
    }

    static void menuHandler(void* ctx) {
        VDL2DecoderModule* _this = (VDL2DecoderModule*)ctx;
        std::lock_guard<std::recursive_mutex> lifecycleLock(_this->lifecycleMtx);
        float menuWidth = ImGui::GetContentRegionAvail().x;

        if (!_this->enabled) { style::beginDisabled(); }

        // Reposition VFOs if SDR center has drifted (user retuned)
        if (_this->running) { _this->repositionVFOs(); }

        // ---- Start/Stop ----
        if (_this->running) {
            if (ImGui::Button(CONCAT("Stop All##startstop_", _this->name), ImVec2(menuWidth, 0)))
                _this->stop();
        } else {
            if (ImGui::Button(CONCAT("Start All##startstop_", _this->name), ImVec2(menuWidth, 0)))
                _this->start();
        }

        // ---- Output settings ----
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(CONCAT("Output##output_hdr_", _this->name))) {
            std::lock_guard<std::mutex> callbackLock(_this->callbackMtx);
            // UDP
            bool udpChanged = false;
            if (ImGui::Checkbox(CONCAT("UDP JSON##udp_", _this->name), &_this->udpEnabled)) udpChanged = true;
            if (_this->udpEnabled) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80);
                if (ImGui::InputInt(CONCAT("##udp_port_", _this->name), &_this->udpPort, 0, 0)) udpChanged = true;
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "127.0.0.1");
            }
            if (udpChanged) {
                if (_this->running) {
                    _this->udpSender.closeSocket();
                    if (_this->udpEnabled) _this->udpSender.open("127.0.0.1", _this->udpPort);
                }
                config.acquire();
                config.conf[_this->name]["udpEnabled"] = _this->udpEnabled;
                config.conf[_this->name]["udpPort"] = _this->udpPort;
                config.release(true);
            }

            // File
            {
                std::lock_guard<std::mutex> fileSettingsLock(_this->fileMtx);
                bool fileChanged = false;
                if (ImGui::Checkbox(CONCAT("JSON File##file_", _this->name), &_this->fileEnabled)) fileChanged = true;
                if (_this->fileEnabled) {
                    char pathBuf[512];
                    strncpy(pathBuf, _this->filePath.c_str(), sizeof(pathBuf) - 1);
                    pathBuf[sizeof(pathBuf) - 1] = 0;
                    ImGui::SetNextItemWidth(menuWidth - 10);
                    if (ImGui::InputText(CONCAT("##file_path_", _this->name), pathBuf, sizeof(pathBuf))) {
                        _this->filePath = pathBuf;
                        fileChanged = true;
                    }
                }
                if (fileChanged) {
                    config.acquire();
                    config.conf[_this->name]["fileEnabled"] = _this->fileEnabled;
                    config.conf[_this->name]["filePath"] = _this->filePath;
                    config.release(true);
                }

            }

            // SQLite database (always on)
            ImGui::Spacing();
            ImGui::TextUnformatted("SQLite History:");
            bool dbChanged = false;
            {
                char dbBuf[512];
                strncpy(dbBuf, _this->dbPath.c_str(), sizeof(dbBuf) - 1);
                dbBuf[sizeof(dbBuf) - 1] = 0;
                ImGui::SetNextItemWidth(menuWidth - 10);
                if (ImGui::InputText(CONCAT("##db_path_", _this->name), dbBuf, sizeof(dbBuf))) {
                    _this->dbPath = dbBuf;
                    dbChanged = true;
                }
                ImGui::LeftLabel("Keep");
                ImGui::SetNextItemWidth(60);
                if (ImGui::InputInt(CONCAT("days##db_ret_", _this->name), &_this->dbRetentionDays, 0, 0)) {
                    if (_this->dbRetentionDays < 1) _this->dbRetentionDays = 1;
                    if (_this->dbRetentionDays > 30) _this->dbRetentionDays = 30;
                    dbChanged = true;
                }
            }
            if (dbChanged) {
                if (_this->running) {
                    _this->messageDB.closeDB();
                    if (_this->dbEnabled && !_this->dbPath.empty())
                        _this->messageDB.open(_this->dbPath);
                }
                config.acquire();
                config.conf[_this->name]["dbEnabled"] = _this->dbEnabled;
                config.conf[_this->name]["dbPath"] = _this->dbPath;
                config.conf[_this->name]["dbRetentionDays"] = _this->dbRetentionDays;
                config.release(true);
            }
        }

        // ---- Channels ----
        ImGui::Spacing();
        if (ImGui::CollapsingHeader(CONCAT("Channels##ch_hdr_", _this->name), ImGuiTreeNodeFlags_DefaultOpen)) {
            bool configChanged = false;
            for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
                bool isVDL2 = (ALL_CHANNELS[i].mode == MODE_VDL2);
                bool wasActive = _this->slots[i].active;  // snapshot to avoid push/pop mismatch
                if (wasActive) {
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        isVDL2 ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f) : ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
                }

                std::string label = std::string(ALL_CHANNELS[i].label) + "##ch_" + std::to_string(i) + _this->name;
                if (ImGui::Checkbox(label.c_str(), &_this->channelEnabled[i])) {
                    configChanged = true;
                    if (_this->running) {
                        if (_this->channelEnabled[i] && !_this->slots[i].active) _this->startChannel(i);
                        else if (!_this->channelEnabled[i] && _this->slots[i].active) _this->stopChannel(i);
                    }
                }
                if (wasActive) {
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                    std::lock_guard<std::mutex> decoderLock(_this->slots[i].decoderMtx);
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "(%d)", _this->slots[i].getMessageCount());
                }
            }
            if (configChanged) {
                config.acquire();
                json arr = json::array();
                for (int i = 0; i < ALL_CHANNEL_COUNT; i++) arr.push_back(_this->channelEnabled[i]);
                config.conf[_this->name]["channelEnabled"] = arr;
                config.release(true);
            }
        }

        // ---- Stats ----
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%d messages | %d channels",
            _this->totalMessages.load(), _this->countActiveChannels());

        if (ImGui::CollapsingHeader(CONCAT("Debug Stats##debug_", _this->name))) {
            ImGui::Text("JSONL write errors: %llu", (unsigned long long)_this->fileWriteErrors.load());
            for (int i = 0; i < ALL_CHANNEL_COUNT; i++) {
                if (!_this->channelEnabled[i] || !_this->slots[i].active) continue;
                auto& s = _this->slots[i];
                std::lock_guard<std::mutex> decoderLock(s.decoderMtx);
                ImVec4 chColor;
                if (ALL_CHANNELS[i].mode == MODE_VDL2) chColor = ImVec4(0.4f, 0.6f, 1.0f, 1.0f);
                else if (ALL_CHANNELS[i].mode == MODE_ACARS) chColor = ImVec4(1.0f, 0.8f, 0.3f, 1.0f);
                else chColor = ImVec4(0.3f, 1.0f, 0.9f, 1.0f);  // cyan for ADS-B
                ImGui::PushStyleColor(ImGuiCol_Text, chColor);
                if (ALL_CHANNELS[i].mode == MODE_VDL2) {
                    ImGui::Text("%.3f: Samp %lldK | Sync %d | Hdr %d/%d | RS %d | CRC %d | Msg %d",
                        ALL_CHANNELS[i].freq / 1e6,
                        s.vdl2Decoder.getSamplesProcessed() / 1000,
                        s.vdl2Decoder.getSyncCount(),
                        s.vdl2Decoder.getHeaderOkCount(),
                        s.vdl2Decoder.getHeaderFailCount(),
                        s.vdl2Decoder.getRsFailCount(),
                        s.vdl2Decoder.getCrcFailCount(),
                        s.vdl2Decoder.getMessageCount());
                    auto p=s.vdl2Decoder.getProtocolCounters();
                    ImGui::Text("  ACARS %llu | FANS CPDLC %llu | ATN X.25 %llu | CLNP %llu | CPDLC %llu | ADS-C %llu | CM %llu",
                        (unsigned long long)p.acars, (unsigned long long)p.fansCpdlc,
                        (unsigned long long)p.atnX25, (unsigned long long)p.atnClnp,
                        (unsigned long long)p.atnCpdlc, (unsigned long long)p.atnAdsc,
                        (unsigned long long)p.atnCm);
                }
                else if (ALL_CHANNELS[i].mode == MODE_ACARS) {
                    ImGui::Text("%.3f: Samp %lldK | Sync %d | Msg %d",
                        ALL_CHANNELS[i].freq / 1e6,
                        s.acarsDecoder.getSamplesProcessed() / 1000,
                        s.acarsDecoder.getSyncCount(),
                        s.acarsDecoder.getMessageCount());
                }
                else {
                    ImGui::Text("%.3f: Samp %lldK | Sync %d | CRC %d | Msg %d | AC %d",
                        ALL_CHANNELS[i].freq / 1e6,
                        s.adsbDecoder.getSamplesProcessed() / 1000,
                        s.adsbDecoder.getSyncCount(),
                        s.adsbDecoder.getCrcFailCount(),
                        s.adsbDecoder.getMessageCount(),
                        s.adsbDecoder.getAircraftCount());
                }
                ImGui::PopStyleColor();
            }
        }

        // ---- Search + controls ----
        ImGui::SetNextItemWidth(menuWidth);
        ImGui::InputTextWithHint(CONCAT("##search_", _this->name), "Search messages...",
            _this->searchBuf, sizeof(_this->searchBuf));

        float thirdW = (menuWidth - ImGui::GetStyle().ItemSpacing.x * 2.f) / 3.f;
        if (ImGui::Button(CONCAT("Clear##clear_", _this->name), ImVec2(thirdW, 0))) {
            std::lock_guard<std::mutex> lock(_this->msgMtx);
            _this->messages.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox(CONCAT("Auto-scroll##scroll_", _this->name), &_this->autoScroll);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ---- Message display ----
        bool hasFilter = _this->searchBuf[0] != '\0';
        ImVec2 msgBoxSize(menuWidth, 350);
        if (ImGui::BeginChild(CONCAT("##msgs_", _this->name), msgBoxSize, true,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            std::lock_guard<std::mutex> lock(_this->msgMtx);
            for (const auto& msg : _this->messages) {
                // Filter
                if (hasFilter && msg.formatted_text.find(_this->searchBuf) == std::string::npos)
                    continue;

                time_t t = (time_t)msg.timestamp;
                struct tm* tm_info = localtime(&t);
                char timebuf[32];
                strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);

                ImVec4 color;
                if (msg.formatted_text.compare(0, 5, "ADS-B") == 0 ||
                    msg.formatted_text.compare(0, 6, "Mode S") == 0)
                    color = ImVec4(0.3f, 1.0f, 0.9f, 1.0f);  // cyan for ADS-B/Mode S
                else if (msg.is_acars)
                    color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);  // green for ACARS
                else
                    color = ImVec4(0.6f, 0.8f, 1.0f, 1.0f);  // blue for VDL2

                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                ImGui::TextUnformatted(timebuf);
                ImGui::PopStyleColor();

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::TextWrapped("%s", msg.formatted_text.c_str());
                ImGui::PopStyleColor();

                ImGui::Separator();
            }

            // Auto-scroll: newest messages are at the top (push_front), so scroll to top
            if (_this->autoScroll && _this->newMessage && !hasFilter) {
                ImGui::SetScrollY(0.0f);
                _this->newMessage = false;
            }
        }
        ImGui::EndChild();

        if (!_this->enabled) { style::endDisabled(); }
    }

    std::recursive_mutex lifecycleMtx; // UI/control only; workers never acquire it
    std::mutex callbackMtx;
    bool acceptingCallbacks = false; // callbackMtx
    bool stopping = false; // lifecycleMtx
    std::string name;
    bool enabled = false;
    bool running = false;
    bool autoScroll = true;
    std::atomic<bool> newMessage{false};
    char searchBuf[128] = {};
    std::atomic<int> totalMessages{0};

    // Output
    bool udpEnabled = true;
    int udpPort = DEFAULT_UDP_PORT;
    UDPSender udpSender;
    bool fileEnabled = false;
    std::string filePath = "/tmp/aviation_messages.jsonl";
    std::mutex fileMtx;
    std::atomic<uint64_t> fileWriteErrors{0};
    bool dbEnabled = true;  // always on
    std::string dbPath = "/tmp/aviation_messages.db";
    int dbRetentionDays = 4;
    MessageDB messageDB;
    std::mutex dbMtx;

    // Channels
    ChannelSlot slots[ALL_CHANNEL_COUNT];
    bool channelEnabled[ALL_CHANNEL_COUNT] = {};

    // Messages
    std::mutex msgMtx;
    std::deque<VDL2Message> messages;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/vdl2_decoder_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new VDL2DecoderModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (VDL2DecoderModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
