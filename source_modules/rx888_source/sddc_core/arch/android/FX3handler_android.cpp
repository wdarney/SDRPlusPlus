#include "FX3handler_android.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

static int g_android_fd = -1;
static int g_android_vid = -1;
static int g_android_pid = -1;
static std::string g_firmware_path;
static std::atomic<uint64_t> g_usb_bytes{0};
static std::atomic<uint64_t> g_usb_transfers{0};
static std::atomic<uint64_t> g_usb_errors{0};

static constexpr uint8_t FX3_BOOT_VENDOR_REQUEST = 0xA0;
static constexpr uint16_t FX3_BOOT_MAX_BLOCK_SIZE = 0x1000;
static constexpr unsigned int FX3_BOOT_TIMEOUT_MS = 1000;
static constexpr int RX888_ANDROID_ASYNC_TRANSFERS = 16;

struct android_fx3handler::AsyncTransferSlot {
    android_fx3handler* owner = nullptr;
    libusb_transfer* transfer = nullptr;
    uint8_t* buffer = nullptr;
    int frameSize = 0;
    bool submitted = false;
};

void rx888_android_set_usb_fd(int fd, int vid, int pid) {
    g_android_fd = fd;
    g_android_vid = vid;
    g_android_pid = pid;
}

void rx888_android_set_firmware_path(const char* path) {
    g_firmware_path = path ? path : "";
}

void rx888_android_reset_usb_stats() {
    g_usb_bytes = 0;
    g_usb_transfers = 0;
    g_usb_errors = 0;
}

uint64_t rx888_android_get_usb_bytes() {
    return g_usb_bytes.load();
}

uint64_t rx888_android_get_usb_transfers() {
    return g_usb_transfers.load();
}

uint64_t rx888_android_get_usb_errors() {
    return g_usb_errors.load();
}

static int fx3_boot_mem_write(libusb_device_handle* dev, uint32_t addr, const uint8_t* data, uint16_t len) {
    return libusb_control_transfer(dev,
                                   LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                   FX3_BOOT_VENDOR_REQUEST,
                                   addr & 0xFFFF,
                                   addr >> 16,
                                   const_cast<uint8_t*>(data),
                                   len,
                                   FX3_BOOT_TIMEOUT_MS);
}

static int fx3_boot_run(libusb_device_handle* dev, uint32_t entry) {
    return libusb_control_transfer(dev,
                                   LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
                                   FX3_BOOT_VENDOR_REQUEST,
                                   entry & 0xFFFF,
                                   entry >> 16,
                                   nullptr,
                                   0,
                                   FX3_BOOT_TIMEOUT_MS);
}

static int fx3_boot_upload_firmware(libusb_device_handle* dev, const char* path) {
    FILE* fw = fopen(path, "rb");
    if (!fw) {
        std::fprintf(stderr, "RX888 Android: failed to open firmware image %s\n", path ? path : "(null)");
        return LIBUSB_ERROR_OTHER;
    }

    char sign[2];
    if (fread(sign, 2, 1, fw) != 1 || sign[0] != 'C' || sign[1] != 'Y') {
        std::fprintf(stderr, "RX888 Android: firmware image has invalid signature\n");
        fclose(fw);
        return LIBUSB_ERROR_OTHER;
    }
    if (fseek(fw, 2, SEEK_CUR) != 0) {
        std::fprintf(stderr, "RX888 Android: firmware image has invalid metadata\n");
        fclose(fw);
        return LIBUSB_ERROR_OTHER;
    }

    uint32_t bufferSize = 0x10000;
    auto* buffer = static_cast<uint8_t*>(std::malloc(bufferSize));
    if (!buffer) {
        fclose(fw);
        return LIBUSB_ERROR_NO_MEM;
    }

    int ret = LIBUSB_SUCCESS;
    while (true) {
        uint32_t sizeWords = 0;
        uint32_t addr = 0;
        if (fread(&sizeWords, sizeof(sizeWords), 1, fw) != 1 ||
            fread(&addr, sizeof(addr), 1, fw) != 1) {
            std::fprintf(stderr, "RX888 Android: invalid firmware section header\n");
            ret = LIBUSB_ERROR_OTHER;
            break;
        }

        uint32_t size = sizeWords << 2;
        if (size == 0) {
            ret = fx3_boot_run(dev, addr);
            break;
        }

        if (size > bufferSize) {
            bufferSize = size;
            auto* resized = static_cast<uint8_t*>(std::realloc(buffer, bufferSize));
            if (!resized) {
                ret = LIBUSB_ERROR_NO_MEM;
                break;
            }
            buffer = resized;
        }

        if (fread(buffer, 1, size, fw) != size) {
            std::fprintf(stderr, "RX888 Android: failed to read firmware section data\n");
            ret = LIBUSB_ERROR_OTHER;
            break;
        }

        for (uint32_t offset = 0; offset < size; offset += FX3_BOOT_MAX_BLOCK_SIZE) {
            uint32_t left = size - offset;
            uint16_t chunk = (uint16_t)std::min<uint32_t>(left, FX3_BOOT_MAX_BLOCK_SIZE);
            ret = fx3_boot_mem_write(dev, addr + offset, &buffer[offset], chunk);
            if (ret < LIBUSB_SUCCESS) {
                std::fprintf(stderr, "RX888 Android: failed writing firmware at 0x%08x: %d\n", addr + offset, ret);
                break;
            }
        }
        if (ret < LIBUSB_SUCCESS) { break; }
    }

    std::free(buffer);
    fclose(fw);
    return ret;
}

bool rx888_android_upload_firmware() {
    if (g_android_fd < 0 || g_android_vid != 0x04b4 || g_android_pid != 0x00f3) {
        std::fprintf(stderr, "RX888 Android: bootloader USB fd is not available vid=%04x pid=%04x fd=%d\n",
                     g_android_vid, g_android_pid, g_android_fd);
        return false;
    }
    if (g_firmware_path.empty()) {
        std::fprintf(stderr, "RX888 Android: firmware path is not set\n");
        return false;
    }

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_context* bootCtx = nullptr;
    libusb_device_handle* bootDev = nullptr;
    int err = libusb_init(&bootCtx);
    if (err != LIBUSB_SUCCESS || !bootCtx) {
        std::fprintf(stderr, "RX888 Android: firmware libusb_init failed: %d\n", err);
        return false;
    }

    int fdDup = dup(g_android_fd);
    if (fdDup < 0) {
        std::fprintf(stderr, "RX888 Android: firmware dup USB fd failed\n");
        libusb_exit(bootCtx);
        return false;
    }

    err = libusb_wrap_sys_device(bootCtx, (intptr_t)fdDup, &bootDev);
    if (err != LIBUSB_SUCCESS || !bootDev) {
        std::fprintf(stderr, "RX888 Android: firmware libusb_wrap_sys_device failed: %d\n", err);
        ::close(fdDup);
        libusb_exit(bootCtx);
        return false;
    }

    std::fprintf(stderr, "RX888 Android: uploading FX3 firmware from %s\n", g_firmware_path.c_str());
    err = fx3_boot_upload_firmware(bootDev, g_firmware_path.c_str());
    libusb_close(bootDev);
    libusb_exit(bootCtx);

    if (err != LIBUSB_SUCCESS) {
        std::fprintf(stderr, "RX888 Android: firmware upload failed: %d\n", err);
        return false;
    }

    std::fprintf(stderr, "RX888 Android: firmware upload complete; wait for USB re-enumeration\n");
    return true;
}

extern "C" fx3class* CreateUsbHandler() {
    return new android_fx3handler();
}

android_fx3handler::android_fx3handler() = default;

android_fx3handler::~android_fx3handler() {
    StopStream();
    close();
}

bool android_fx3handler::Open() {
    if (g_android_fd < 0 || g_android_vid != 0x04b4 || g_android_pid != 0x00f1) {
        std::fprintf(stderr, "RX888 Android: runtime USB fd is not available vid=%04x pid=%04x fd=%d\n",
                     g_android_vid, g_android_pid, g_android_fd);
        return false;
    }

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    int err = libusb_init(&ctx);
    if (err != LIBUSB_SUCCESS || !ctx) {
        std::fprintf(stderr, "RX888 Android: libusb_init failed: %d\n", err);
        ctx = nullptr;
        return false;
    }

    int fdDup = dup(g_android_fd);
    if (fdDup < 0) {
        std::fprintf(stderr, "RX888 Android: dup USB fd failed\n");
        close();
        return false;
    }

    err = libusb_wrap_sys_device(ctx, (intptr_t)fdDup, &dev);
    if (err != LIBUSB_SUCCESS || !dev) {
        std::fprintf(stderr, "RX888 Android: libusb_wrap_sys_device failed: %d\n", err);
        ::close(fdDup);
        close();
        return false;
    }

    libusb_claim_interface(dev, 0);
    if (!findBulkInEndpoint()) {
        std::fprintf(stderr, "RX888 Android: no bulk IN endpoint found, using 0x%02x fallback\n", bulkInEndpoint);
    }

    Control(STOPFX3, (uint8_t)0);
    return true;
}

bool android_fx3handler::close() {
    if (dev) {
        libusb_release_interface(dev, 0);
        libusb_close(dev);
        dev = nullptr;
    }
    if (ctx) {
        libusb_exit(ctx);
        ctx = nullptr;
    }
    return true;
}

bool android_fx3handler::findBulkInEndpoint() {
    libusb_device* usbDev = libusb_get_device(dev);
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(usbDev, &cfg) != LIBUSB_SUCCESS || !cfg) {
        return false;
    }

    bool found = false;
    for (uint8_t i = 0; i < cfg->bNumInterfaces && !found; i++) {
        const libusb_interface& iface = cfg->interface[i];
        for (int j = 0; j < iface.num_altsetting && !found; j++) {
            const libusb_interface_descriptor& alt = iface.altsetting[j];
            for (uint8_t k = 0; k < alt.bNumEndpoints; k++) {
                const libusb_endpoint_descriptor& ep = alt.endpoint[k];
                bool isBulk = (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
                bool isIn = (ep.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;
                if (isBulk && isIn) {
                    bulkInEndpoint = ep.bEndpointAddress;
                    bulkPacketSize = ep.wMaxPacketSize;
                    libusb_ss_endpoint_companion_descriptor* ss = nullptr;
                    if (libusb_get_ss_endpoint_companion_descriptor(ctx, &ep, &ss) == LIBUSB_SUCCESS && ss) {
                        bulkMaxBurst = ss->bMaxBurst;
                        libusb_free_ss_endpoint_companion_descriptor(ss);
                    }
                    found = true;
                    break;
                }
            }
        }
    }

    libusb_free_config_descriptor(cfg);
    return found;
}

bool android_fx3handler::control(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t length, bool read) {
    if (!dev) { return false; }
    uint8_t bmRequest = (read ? LIBUSB_ENDPOINT_IN : LIBUSB_ENDPOINT_OUT) |
                        LIBUSB_REQUEST_TYPE_VENDOR |
                        LIBUSB_RECIPIENT_DEVICE;
    int err = libusb_control_transfer(dev, bmRequest, request, value, index, data, length, 1000);
    return err >= 0;
}

bool android_fx3handler::Control(FX3Command command, uint8_t data) {
    return control(command, 0, 0, &data, sizeof(data), false);
}

bool android_fx3handler::Control(FX3Command command, uint32_t data) {
    return control(command, 0, 0, reinterpret_cast<uint8_t*>(&data), sizeof(data), false);
}

bool android_fx3handler::Control(FX3Command command, uint64_t data) {
    return control(command, 0, 0, reinterpret_cast<uint8_t*>(&data), sizeof(data), false);
}

bool android_fx3handler::SetArgument(uint16_t index, uint16_t value) {
    uint8_t data = 0;
    return control(SETARGFX3, value, index, &data, sizeof(data), false);
}

bool android_fx3handler::GetHardwareInfo(uint32_t* data) {
    if (!data) { return false; }
    bool ok = control(TESTFX3, 0, 0, reinterpret_cast<uint8_t*>(data), sizeof(*data), true);
    uint8_t* rdata = reinterpret_cast<uint8_t*>(data);
    std::fprintf(stderr, "RX888 Android: TESTFX3 model=0x%02x firmware=0x%02x%02x vendorReq=%u\n",
                 rdata[0], rdata[1], rdata[2], rdata[3]);
    if (ok && rdata[0] == HF103) {
        std::fprintf(stderr, "RX888 Android: firmware reported HF103 on RX888 source; forcing RX888 MkII model\n");
        rdata[0] = RX888r2;
    }
    return ok;
}

bool android_fx3handler::ReadDebugTrace(uint8_t* pdata, uint8_t len) {
    return pdata && control(READINFODEBUG, 0, 0, pdata, len, true);
}

void android_fx3handler::StartStream(ringbuffer<int16_t>& input, int) {
    inputBuffer = &input;
    rx888_android_reset_usb_stats();
    uint32_t maxXfer = bulkPacketSize * (bulkMaxBurst + 1);
    uint32_t frameSize = transferSize;
    if (maxXfer > 0 && frameSize % maxXfer != 0) {
        frameSize = std::max<uint32_t>(maxXfer, (frameSize / maxXfer) * maxXfer);
    }
    asyncFrameSize = frameSize;
    input.setBlockSize(frameSize / sizeof(int16_t));
    input.Start();

    run = true;
    streamThread = std::thread(&android_fx3handler::streamLoop, this);
}

void android_fx3handler::StopStream() {
    run = false;
    if (inputBuffer) { inputBuffer->Stop(); }
    if (streamThread.joinable()) { streamThread.join(); }
    inputBuffer = nullptr;
}

static void LIBUSB_CALL rx888_android_transfer_callback(libusb_transfer* transfer) {
    auto* slot = static_cast<android_fx3handler::AsyncTransferSlot*>(transfer->user_data);
    if (slot && slot->owner) {
        slot->owner->onAsyncTransfer(slot, transfer);
    }
}

bool android_fx3handler::submitAsyncTransfer(AsyncTransferSlot* slot) {
    if (!run || !slot || !slot->transfer || !slot->buffer) { return false; }
    libusb_fill_bulk_transfer(slot->transfer,
                              dev,
                              bulkInEndpoint,
                              slot->buffer,
                              slot->frameSize,
                              rx888_android_transfer_callback,
                              slot,
                              1000);
    int err = libusb_submit_transfer(slot->transfer);
    if (err != LIBUSB_SUCCESS) {
        g_usb_errors++;
        slot->submitted = false;
        return false;
    }
    slot->submitted = true;
    activeAsyncTransfers++;
    return true;
}

void android_fx3handler::onAsyncTransfer(AsyncTransferSlot* slot, libusb_transfer* transfer) {
    slot->submitted = false;
    activeAsyncTransfers--;

    if (run && transfer->status == LIBUSB_TRANSFER_COMPLETED && transfer->actual_length == slot->frameSize) {
        int16_t* ptr = inputBuffer->getWritePtr();
        if (run && ptr) {
            std::memcpy(ptr, slot->buffer, slot->frameSize);
            g_usb_bytes += (uint64_t)slot->frameSize;
            g_usb_transfers++;
            inputBuffer->WriteDone();
        }
    }
    else if (transfer->status != LIBUSB_TRANSFER_CANCELLED) {
        g_usb_errors++;
    }

    if (run) { submitAsyncTransfer(slot); }
}

void android_fx3handler::streamLoop() {
    asyncSlots.clear();
    activeAsyncTransfers = 0;

    for (int i = 0; i < RX888_ANDROID_ASYNC_TRANSFERS; i++) {
        auto* slot = new AsyncTransferSlot();
        slot->owner = this;
        slot->frameSize = (int)asyncFrameSize;
        slot->buffer = static_cast<uint8_t*>(std::malloc(slot->frameSize));
        slot->transfer = libusb_alloc_transfer(0);
        if (!slot->buffer || !slot->transfer) {
            g_usb_errors++;
            if (slot->transfer) { libusb_free_transfer(slot->transfer); }
            if (slot->buffer) { std::free(slot->buffer); }
            delete slot;
            continue;
        }
        asyncSlots.push_back(slot);
        submitAsyncTransfer(slot);
    }

    while (run && activeAsyncTransfers.load() > 0) {
        timeval tv{0, 100000};
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }

    for (auto* slot : asyncSlots) {
        if (slot->submitted) {
            libusb_cancel_transfer(slot->transfer);
        }
    }

    while (activeAsyncTransfers.load() > 0) {
        timeval tv{0, 100000};
        libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
    }

    for (auto* slot : asyncSlots) {
        if (slot->transfer) { libusb_free_transfer(slot->transfer); }
        if (slot->buffer) { std::free(slot->buffer); }
        delete slot;
    }
    asyncSlots.clear();
}

bool android_fx3handler::Enumerate(unsigned char& idx, char* lbuf) {
    if (idx > 0 || g_android_fd < 0 || g_android_pid != 0x00f1) { return false; }
    std::strcpy(lbuf, "RX888 Android");
    return true;
}
