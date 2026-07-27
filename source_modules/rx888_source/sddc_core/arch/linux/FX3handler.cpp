#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>

#include "FX3handler.h"
#include "usb_device.h"
#include "ezusb.h"
#include "firmware.h"

#define firmware_data ((const char *)FIRMWARE)
#define firmware_size sizeof(FIRMWARE)

fx3class *CreateUsbHandler()
{
    return new fx3handler();
}

fx3handler::fx3handler()
{
    devidx = 0;
    usb_device_infos = nullptr;
    dev = nullptr;
    stream = nullptr;
    inputbuffer = nullptr;
    run = false;
}

fx3handler::~fx3handler()
{
    Close();
}

bool fx3handler::Open()
{
    dev = usb_device_open(devidx, firmware_data, firmware_size);
    DbgPrintf("Open DevIdx=%d dev=%p\n", devidx, dev);

    usleep(5000);
    Control(STOPFX3, (uint8_t)0);

    return dev != nullptr;
}

bool fx3handler::Close(void)
{
    DbgPrintf("Close dev=%p\n", dev);

    if (dev) {
        usb_device_close(dev);
        dev = nullptr;
    }

    return true;
}

bool fx3handler::Control(FX3Command command, uint8_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::Control(FX3Command command, uint32_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::Control(FX3Command command, uint64_t data)
{
    return usb_device_control(this->dev, command, 0, 0, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::SetArgument(uint16_t index, uint16_t value)
{
    uint8_t data = 0;
    return usb_device_control(this->dev, SETARGFX3, value, index, (uint8_t *)&data, sizeof(data), 0) == 0;
}

bool fx3handler::GetHardwareInfo(uint32_t *data)
{
    bool ok = usb_device_control(this->dev, TESTFX3, 0, 0, (uint8_t *)data, sizeof(*data), 1) == 0;
    if (!ok || data == nullptr) {
        return ok;
    }

    uint8_t *rdata = (uint8_t *)data;
    const char *product = nullptr;
    const char *serial = nullptr;
    if (usb_device_infos != nullptr) {
        product = (const char *)usb_device_infos[devidx].product;
        serial = (const char *)usb_device_infos[devidx].serial_number;
    }
    (void)product;

    DbgPrintf("TESTFX3 raw model=0x%02x firmware=0x%02x%02x vendorReq=%u product=%s serial=%s\n",
              rdata[0],
              rdata[1],
              rdata[2],
              rdata[3],
              product ? product : "(unknown)",
              serial ? serial : "(unknown)");

    bool isThisRx888MkII =
        serial != nullptr &&
        (strcmp(serial, "0009071C02CC153B") == 0 ||
         strcmp(serial, "0000000004BE") == 0);

    if (rdata[0] == HF103 && isThisRx888MkII) {
        DbgPrintf("TESTFX3 diagnostic override: firmware reported HF103 for known RX888 MkII serial %s; forcing RX888 mkII\n",
                  serial);
        rdata[0] = RX888r2;
    }

    struct I2CDiag {
        const char *name;
        uint16_t address;
        uint16_t reg;
    };
    const I2CDiag probes[] = {
        {"R828D",  0x74, 0},
        {"R820T",  0x34, 0},
        {"Si5351", 0xC0, 183},
    };
    for (const auto &probe : probes) {
        uint8_t value = 0;
        int ret = usb_device_control(this->dev, I2CRFX3, probe.address, probe.reg, &value, sizeof(value), 1);
        if (ret == 0) {
            fprintf(stderr, "SDDC diagnostic I2C: %s addr=0x%02x reg=0x%02x value=0x%02x\n",
                    probe.name, probe.address, probe.reg, value);
        }
        else {
            fprintf(stderr, "SDDC diagnostic I2C: %s addr=0x%02x reg=0x%02x read failed\n",
                    probe.name, probe.address, probe.reg);
        }
    }

    return true;
}

void fx3handler::StartStream(ringbuffer<int16_t> &input, int numofblock)
{
    inputbuffer = &input;
    stream = streaming_open_async(this->dev, transferSize, concurrentTransfers, PacketRead, this);
    input.setBlockSize(streaming_framesize(stream) / sizeof(int16_t));

    DbgPrintf("StartStream blocksize=%d\n", input.getBlockSize());

    // Start background thread to poll the events
    run = true;
    if (stream)
    {
        streaming_start(stream);
    }

    poll_thread = std::thread(
        [this]()
        {
            while (run)
            {
                usb_device_handle_events(this->dev);
            }
        });
}

void fx3handler::StopStream()
{
    if (!stream) {
        run = false;
        if (poll_thread.joinable()) {
            poll_thread.join();
        }
        return;
    }

    // streaming_stop() needs libusb events to retire cancelled transfers.
    // Keep the polling thread alive until after the transfers are cancelled
    // and their completion callbacks have run; otherwise shutdown can wait
    // forever until the device is unplugged.
    streaming_stop(stream);

    run = false;
    if (poll_thread.joinable()) {
        poll_thread.join();
    }

    streaming_close(stream);
    stream = nullptr;
}

void fx3handler::PacketRead(uint32_t data_size, uint8_t *data, void *context)
{
    fx3handler *handler = (fx3handler *)context;

    auto *ptr = handler->inputbuffer->getWritePtr();
    assert(data_size == handler->inputbuffer->getBlockSize() * sizeof(int16_t));
    memcpy(ptr, data, data_size);
    handler->inputbuffer->WriteDone();
}

bool fx3handler::ReadDebugTrace(uint8_t *pdata, uint8_t len)
{
    return true;
}

bool fx3handler::Enumerate(unsigned char &idx, char *lbuf)
{
    if (idx >= usb_device_count_devices()) return false;

    if (usb_device_infos == nullptr) {
        usb_device_get_device_list(&usb_device_infos);
    }

    auto dev = &usb_device_infos[idx];

    strcpy (lbuf, (const char*)dev->product);
    while (strlen(lbuf) < 18) strcat(lbuf, " ");
    strcat(lbuf, "sn:");
    strcat(lbuf, (const char*)dev->serial_number);
    devidx = idx;

    return true;
}
