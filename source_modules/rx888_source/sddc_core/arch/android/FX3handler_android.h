#pragma once

#include "../../FX3Class.h"
#include "../../config.h"
#include "../../dsp/ringbuffer.h"
#include <atomic>
#include <libusb.h>
#include <string>
#include <thread>
#include <vector>

void rx888_android_set_usb_fd(int fd, int vid, int pid);
void rx888_android_set_firmware_path(const char* path);
bool rx888_android_upload_firmware();
void rx888_android_reset_usb_stats();
uint64_t rx888_android_get_usb_bytes();
uint64_t rx888_android_get_usb_transfers();
uint64_t rx888_android_get_usb_errors();

class android_fx3handler : public fx3class {
public:
    struct AsyncTransferSlot;

    android_fx3handler();
    ~android_fx3handler() override;

    bool Open() override;
    bool Control(FX3Command command, uint8_t data = 0) override;
    bool Control(FX3Command command, uint32_t data) override;
    bool Control(FX3Command command, uint64_t data) override;
    bool SetArgument(uint16_t index, uint16_t value) override;
    bool GetHardwareInfo(uint32_t* data) override;
    bool ReadDebugTrace(uint8_t* pdata, uint8_t len) override;
    void StartStream(ringbuffer<int16_t>& input, int numofblock) override;
    void StopStream() override;
    bool Enumerate(unsigned char& idx, char* lbuf) override;
    void onAsyncTransfer(AsyncTransferSlot* slot, libusb_transfer* transfer);

private:
    bool control(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length, bool read);
    bool close();
    bool findBulkInEndpoint();
    void streamLoop();
    bool submitAsyncTransfer(AsyncTransferSlot* slot);

    libusb_context* ctx = nullptr;
    libusb_device_handle* dev = nullptr;
    uint8_t bulkInEndpoint = 0x81;
    uint16_t bulkPacketSize = 1024;
    uint8_t bulkMaxBurst = 15;
    ringbuffer<int16_t>* inputBuffer = nullptr;
    std::atomic<bool> run{false};
    std::thread streamThread;
    std::vector<AsyncTransferSlot*> asyncSlots;
    std::atomic<int> activeAsyncTransfers{0};
    uint32_t asyncFrameSize = transferSize;
};
