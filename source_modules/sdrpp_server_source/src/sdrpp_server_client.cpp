#include "sdrpp_server_client.h"
#include <volk/volk.h>
#include <cstring>
#include <utils/flog.h>
#include <core.h>

using namespace std::chrono_literals;

namespace {
    constexpr uint64_t SLOW_SWAP_NS = 10'000'000;
    constexpr auto TRANSPORT_STATS_INTERVAL = std::chrono::seconds(5);

    void updateMax(std::atomic<uint64_t>& maximum, uint64_t value) {
        uint64_t current = maximum.load(std::memory_order_relaxed);
        while (current < value && !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    }

    void recordSwapWait(
        uint64_t waitNs,
        std::atomic<uint64_t>& total,
        std::atomic<uint64_t>& count,
        std::atomic<uint64_t>& maximum,
        std::atomic<uint64_t>& slowCount
    ) {
        total.fetch_add(waitNs, std::memory_order_relaxed);
        count.fetch_add(1, std::memory_order_relaxed);
        updateMax(maximum, waitNs);
        if (waitNs >= SLOW_SWAP_NS) { slowCount.fetch_add(1, std::memory_order_relaxed); }
    }
}

namespace server {
    Client::Client(std::shared_ptr<net::Socket> sock, dsp::stream<dsp::complex_t>* out) {
        this->sock = sock;
        output = out;

        // Allocate buffers
        rbuffer = new uint8_t[SERVER_MAX_PACKET_SIZE];
        sbuffer = new uint8_t[SERVER_MAX_PACKET_SIZE];

        // Initialize headers
        r_pkt_hdr = (PacketHeader*)rbuffer;
        r_pkt_data = &rbuffer[sizeof(PacketHeader)];
        r_cmd_hdr = (CommandHeader*)r_pkt_data;
        r_cmd_data = &rbuffer[sizeof(PacketHeader) + sizeof(CommandHeader)];

        s_pkt_hdr = (PacketHeader*)sbuffer;
        s_pkt_data = &sbuffer[sizeof(PacketHeader)];
        s_cmd_hdr = (CommandHeader*)s_pkt_data;
        s_cmd_data = &sbuffer[sizeof(PacketHeader) + sizeof(CommandHeader)];

        // Initialize decompressor
        dctx = ZSTD_createDCtx();

        // Initialize DSP
        decompIn.setBufferSize(STREAM_BUFFER_SIZE*sizeof(dsp::complex_t) + 8);
        decompIn.clearWriteStop();
        decomp.init(&decompIn);
        link.init(&decomp.out, output);
        link.setSwapWaitHandler(outputSwapWaitHandler, this);
        decomp.start();
        link.start();

        // Start worker thread
        workerThread = std::thread(&Client::worker, this);

        // Ask for a UI
        int res = getUI();
        if (res < 0) {
            // Close client
            close();

            // Throw error
            switch (res) {
            case CONN_ERR_TIMEOUT:
                throw std::runtime_error("Timed out");
            case CONN_ERR_BUSY:
                throw std::runtime_error("Server busy");
            default:
                throw std::runtime_error("Unknown error");
            }
        }
    }

    Client::~Client() {
        close();
        ZSTD_freeDCtx(dctx);
        delete[] rbuffer;
        delete[] sbuffer;
    }

    void Client::showMenu() {
        std::string diffId = "";
        SmGui::DrawListElem diffValue;
        bool syncRequired = false;
        {
            std::lock_guard<std::mutex> lck(dlMtx);
            dl.draw(diffId, diffValue, syncRequired);
        }

        if (!diffId.empty()) {
            // Save ID
            SmGui::DrawListElem elemId;
            elemId.type = SmGui::DRAW_LIST_ELEM_TYPE_STRING;
            elemId.str = diffId;

            // Encore packet
            int size = 0;
            s_cmd_data[size++] = syncRequired;
            size += SmGui::DrawList::storeItem(elemId, &s_cmd_data[size], SERVER_MAX_PACKET_SIZE - size);
            size += SmGui::DrawList::storeItem(diffValue, &s_cmd_data[size], SERVER_MAX_PACKET_SIZE - size);

            // Send
            if (syncRequired) {
                flog::warn("Action requires resync");
                auto waiter = awaitCommandAck(COMMAND_UI_ACTION);
                sendCommand(COMMAND_UI_ACTION, size);
                if (waiter->await(PROTOCOL_TIMEOUT_MS)) {
                    std::lock_guard lck(dlMtx);
                    dl.load(r_cmd_data, r_pkt_hdr->size - sizeof(PacketHeader) - sizeof(CommandHeader));
                }
                else {
                    flog::error("Timeout out after asking for UI");
                }
                waiter->handled();
                flog::warn("Resync done");
            }
            else {
                flog::warn("Action does not require resync");
                sendCommand(COMMAND_UI_ACTION, size);
            }
        }
    }

    void Client::setFrequency(double freq) {
        if (!isOpen()) { return; }
        *(double*)s_cmd_data = freq;
        sendCommand(COMMAND_SET_FREQUENCY, sizeof(double));
        auto waiter = awaitCommandAck(COMMAND_SET_FREQUENCY);
        waiter->await(PROTOCOL_TIMEOUT_MS);
        waiter->handled();
    }

    double Client::getSampleRate() {
        return currentSampleRate;
    }

    void Client::setSampleType(dsp::compression::PCMType type) {
        if (!isOpen()) { return; }
        s_cmd_data[0] = type;
        sendCommand(COMMAND_SET_SAMPLE_TYPE, 1);
    }

    void Client::setCompression(bool enabled) {
        if (!isOpen()) { return; }
         s_cmd_data[0] = enabled;
        sendCommand(COMMAND_SET_COMPRESSION, 1);
    }

    void Client::start() {
        if (!isOpen()) { return; }
        sendCommand(COMMAND_START, 0);
        getUI();
    }

    void Client::stop() {
        if (!isOpen()) { return; }
        sendCommand(COMMAND_STOP, 0);
        getUI();
    }

    void Client::close() {
        // Stop worker
        decompIn.stopWriter();
        if (sock) { sock->close(); }
        if (workerThread.joinable()) { workerThread.join(); }
        decompIn.clearWriteStop();

        // Stop DSP
        decomp.stop();
        link.stop();
    }

    bool Client::isOpen() {
        return sock && sock->isOpen();
    }

    void Client::worker() {
        while (true) {
            // Receive header
            if (sock->recv(rbuffer, sizeof(PacketHeader), true) <= 0) {
                break;
            }

            // Receive remaining data
            if (sock->recv(&rbuffer[sizeof(PacketHeader)], r_pkt_hdr->size - sizeof(PacketHeader), true, PROTOCOL_TIMEOUT_MS) <= 0) {
                break;
            }

            // Increment data counter
            bytes += r_pkt_hdr->size;
            transportBytes += r_pkt_hdr->size;

            // Decode packet
            if (r_pkt_hdr->type == PACKET_TYPE_COMMAND) {
                // TODO: Move to command handler
                if (r_cmd_hdr->cmd == COMMAND_SET_SAMPLERATE && r_pkt_hdr->size == sizeof(PacketHeader) + sizeof(CommandHeader) + sizeof(double)) {
                    currentSampleRate = *(double*)r_cmd_data;
                    core::setInputSampleRate(currentSampleRate);
                }
                else if (r_cmd_hdr->cmd == COMMAND_DISCONNECT) {
                    flog::error("Asked to disconnect by the server");
                    serverBusy = true;

                    // Cancel waiters
                    std::vector<PacketWaiter*> toBeRemoved;
                    for (auto& [waiter, cmd] : commandAckWaiters) {
                        waiter->cancel();
                        toBeRemoved.push_back(waiter);
                    }

                    // Remove handled waiters
                    for (auto& waiter : toBeRemoved) {
                        commandAckWaiters.erase(waiter);
                        delete waiter;
                    }
                }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_COMMAND_ACK) {
                // Notify waiters
                std::vector<PacketWaiter*> toBeRemoved;
                for (auto& [waiter, cmd] : commandAckWaiters) {
                    if (cmd != r_cmd_hdr->cmd) { continue; }
                    waiter->notify();
                    toBeRemoved.push_back(waiter);
                }

                // Remove handled waiters
                for (auto& waiter : toBeRemoved) {
                    commandAckWaiters.erase(waiter);
                    delete waiter;
                }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_BASEBAND) {
                memcpy(decompIn.writeBuf, &rbuffer[sizeof(PacketHeader)], r_pkt_hdr->size - sizeof(PacketHeader));
                if (!swapDecompInput(r_pkt_hdr->size - sizeof(PacketHeader))) { break; }
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_BASEBAND_COMPRESSED) {
                size_t outCount = ZSTD_decompressDCtx(dctx, decompIn.writeBuf, STREAM_BUFFER_SIZE*sizeof(dsp::complex_t)+8, r_pkt_data, r_pkt_hdr->size - sizeof(PacketHeader));
                if (outCount) {
                    if (!swapDecompInput(outCount)) { break; }
                };
            }
            else if (r_pkt_hdr->type == PACKET_TYPE_ERROR) {
                flog::error("SDR++ Server Error: {0}", rbuffer[sizeof(PacketHeader)]);
            }
            else {
                flog::error("Invalid packet type: {0}", r_pkt_hdr->type);
            }

            maybeLogTransportStats();
        }
    }

    bool Client::swapDecompInput(int size) {
        auto swapStart = std::chrono::steady_clock::now();
        bool swapped = decompIn.swap(size);
        uint64_t waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - swapStart
        ).count();
        recordSwapWait(waitNs, decompSwapWaitNs, decompSwapCount, decompSwapMaxNs, decompSlowSwaps);
        return swapped;
    }

    void Client::outputSwapWaitHandler(uint64_t waitNs, int count, void* ctx) {
        (void)count;
        Client* client = (Client*)ctx;
        recordSwapWait(
            waitNs,
            client->outputSwapWaitNs,
            client->outputSwapCount,
            client->outputSwapMaxNs,
            client->outputSlowSwaps
        );
    }

    void Client::maybeLogTransportStats() {
        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - transportStatsStart).count();
        if (elapsedSec < std::chrono::duration<double>(TRANSPORT_STATS_INTERVAL).count()) { return; }

        uint64_t decompWait = decompSwapWaitNs.exchange(0, std::memory_order_relaxed);
        uint64_t decompCount = decompSwapCount.exchange(0, std::memory_order_relaxed);
        uint64_t decompMax = decompSwapMaxNs.exchange(0, std::memory_order_relaxed);
        uint64_t decompSlow = decompSlowSwaps.exchange(0, std::memory_order_relaxed);
        uint64_t outputWait = outputSwapWaitNs.exchange(0, std::memory_order_relaxed);
        uint64_t outputCount = outputSwapCount.exchange(0, std::memory_order_relaxed);
        uint64_t outputMax = outputSwapMaxNs.exchange(0, std::memory_order_relaxed);
        uint64_t outputSlow = outputSlowSwaps.exchange(0, std::memory_order_relaxed);

        double mbps = (transportBytes * 8.0) / (elapsedSec * 1000000.0);
        double decompAvgMs = decompCount ? (decompWait / (double)decompCount) / 1000000.0 : 0.0;
        double outputAvgMs = outputCount ? (outputWait / (double)outputCount) / 1000000.0 : 0.0;
        flog::info(
            "[Server client transport] {:.1f} Mb/s, decompIn.swap avg/max={:.3f}/{:.3f} ms "
            "(count={}, >=10 ms={}), output.swap avg/max={:.3f}/{:.3f} ms (count={}, >=10 ms={})",
            mbps,
            decompAvgMs,
            decompMax / 1000000.0,
            decompCount,
            decompSlow,
            outputAvgMs,
            outputMax / 1000000.0,
            outputCount,
            outputSlow
        );

        transportBytes = 0;
        transportStatsStart = now;
    }

    int Client::getUI() {
        if (!isOpen()) { return -1; }
        auto waiter = awaitCommandAck(COMMAND_GET_UI);
        sendCommand(COMMAND_GET_UI, 0);
        if (waiter->await(PROTOCOL_TIMEOUT_MS)) {
            std::lock_guard lck(dlMtx);
            dl.load(r_cmd_data, r_pkt_hdr->size - sizeof(PacketHeader) - sizeof(CommandHeader));
        }
        else {
            if (!serverBusy) { flog::error("Timeout out after asking for UI"); };
            waiter->handled();
            return serverBusy ? CONN_ERR_BUSY : CONN_ERR_TIMEOUT;
        }
        waiter->handled();
        return 0;
    }

    void Client::sendPacket(PacketType type, int len) {
        s_pkt_hdr->type = type;
        s_pkt_hdr->size = sizeof(PacketHeader) + len;
        sock->send(sbuffer, s_pkt_hdr->size);
    }

    void Client::sendCommand(Command cmd, int len) {
        s_cmd_hdr->cmd = cmd;
        sendPacket(PACKET_TYPE_COMMAND, sizeof(CommandHeader) + len);
    }

    void Client::sendCommandAck(Command cmd, int len) {
        s_cmd_hdr->cmd = cmd;
        sendPacket(PACKET_TYPE_COMMAND_ACK, sizeof(CommandHeader) + len);
    }

    PacketWaiter* Client::awaitCommandAck(Command cmd) {
        PacketWaiter* waiter = new PacketWaiter;
        commandAckWaiters[waiter] = cmd;
        return waiter;
    }

    std::shared_ptr<Client> connect(std::string host, uint16_t port, dsp::stream<dsp::complex_t>* out) {
        return std::make_shared<Client>(net::connect(host, port), out);
    }
}
