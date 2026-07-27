#include <sddc.h>
#include <stdlib.h>
#include <stdint.h>
#include <libusb.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "usb_interface.h"
#include "fx3_boot.h"

struct sddc_dev {
    // USB handles
    struct libusb_device_handle* openDev;

    // Device info
    sddc_devinfo_t info;

    // Device state
    bool running;
    uint32_t samplerate;
    uint64_t tunerFreq;
    sddc_gpio_t gpioState;
    sddc_port_t port;
};

struct libusb_context* ctx = NULL;
char* sddc_firmware_path = NULL;
bool sddc_is_init = false;

static int sddc_gpio_put(sddc_dev_t* dev, sddc_gpio_t gpios, bool value);

static sddc_error_t sddc_init() {
    // If already initialized, do nothing
    if (sddc_is_init) { return SDDC_SUCCESS; }

    // If the firmware isn't already found, find it
    if (!sddc_firmware_path) {
        // TODO: Find the firmware
    }

    // Init libusb
#ifdef __ANDROID__
    libusb_set_option(NULL, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
#endif
    int err = libusb_init(&ctx);
    if (err != LIBUSB_SUCCESS || !ctx) {
        fprintf(stderr, "Failed to initialize libusb for SDDC: %d\n", err);
        ctx = NULL;
        return SDDC_ERROR_USB_ERROR;
    }

    sddc_is_init = true;
    return SDDC_SUCCESS;
}

static sddc_error_t sddc_wrap_fd(int fd, libusb_device_handle** openDev) {
    *openDev = NULL;

    sddc_error_t initErr = sddc_init();
    if (initErr != SDDC_SUCCESS) { return initErr; }

    int fdDup = dup(fd);
    if (fdDup < 0) {
        fprintf(stderr, "Failed to duplicate Android USB fd for SDDC\n");
        return SDDC_ERROR_USB_ERROR;
    }

    int err = libusb_wrap_sys_device(ctx, (intptr_t)fdDup, openDev);
    if (err != LIBUSB_SUCCESS || !*openDev) {
        fprintf(stderr, "Failed to wrap Android USB fd for SDDC: %d\n", err);
        close(fdDup);
        return SDDC_ERROR_USB_ERROR;
    }

    return SDDC_SUCCESS;
}

const char* sddc_model_to_string(sddc_model_t model) {
    switch (model) {
    case SDDC_MODEL_BBRF103:    return "BBRF103";
    case SDDC_MODEL_HF103:      return "HF103";
    case SDDC_MODEL_RX888:      return "RX888";
    case SDDC_MODEL_RX888_MK2:  return "RX888 MK2";
    case SDDC_MODEL_RX999:      return "RX999";
    case SDDC_MODEL_RXLUCY:     return "RXLUCY";
    case SDDC_MODEL_RX888_MK3:  return "RX888 MK3";
    default:                    return "Unknown";
    }
}

const char* sddc_error_to_string(sddc_error_t error) {
    switch (error) {
    case SDDC_ERROR_NOT_IMPLEMENTED:        return "Not Implemented";
    case SDDC_ERROR_FIRMWARE_UPLOAD_FAILED: return "Firmware Upload Failed";
    case SDDC_ERROR_NOT_FOUND:              return "Not Found";
    case SDDC_ERROR_USB_ERROR:              return "USB Error";
    case SDDC_ERROR_TIMEOUT:                return "Timeout";
    case SDDC_SUCCESS:                      return "Success";
    default:                                return "Unknown";
    }
}

sddc_error_t sddc_set_firmware_path(const char* path) {
    // Free the old path if it exists
    if (sddc_firmware_path) { free(sddc_firmware_path); }

    // Allocate the new path
    sddc_firmware_path = malloc(strlen(path) + 1);

    // Copy the new path
    strcpy(sddc_firmware_path, path);

    // TODO: Check if the file path exists
    return SDDC_SUCCESS;
}

int sddc_get_device_list(sddc_devinfo_t** dev_list) {
    *dev_list = NULL;

    // Initialize libsddc in case it isn't already
    sddc_error_t initErr = sddc_init();
    if (initErr != SDDC_SUCCESS) { return initErr; }

    // Get a list of USB devices
    libusb_device** devices;
    int devCount = libusb_get_device_list(ctx, &devices);
    if (devCount < 0 || !devices) { return SDDC_ERROR_USB_ERROR; }

    // Initialize all uninitialized devices
    bool uninit = false;
    for (int i = 0; i < devCount; i++) {
        // Get the device from the list
        libusb_device* dev = devices[i];

        // Get the device descriptor. Fail silently on error as it might not even be a SDDC device.
        struct libusb_device_descriptor desc;
        int err = libusb_get_device_descriptor(dev, &desc);
        if (err != LIBUSB_SUCCESS) { continue; }

        // If it's not an uninitialized device, go to next device
        if (desc.idVendor != SDDC_UNINIT_VID || desc.idProduct != SDDC_UNINIT_PID) { continue; }

        // Initialize the device
        printf("Found uninitialized device, initializing...\n");
        // TODO: Check that the firmware path is valid
        sddc_error_t serr = sddc_init_device(dev, sddc_firmware_path);
        if (serr != SDDC_SUCCESS) { continue; }

        // Set the flag to wait the devices to start up
        uninit = true;
    }

    // If some uninitialized devices were found
    if (uninit) {
        // Free the device list
        libusb_free_device_list(devices, 1);

        // Wait for the devices to show back up
#ifdef _WIN32
        Sleep(SDDC_INIT_SEARCH_DELAY_MS);
#else
        usleep(SDDC_INIT_SEARCH_DELAY_MS * 1000);
#endif

        // Attempt to list devices again
        devCount = libusb_get_device_list(ctx, &devices);
        if (devCount < 0 || !devices) { return SDDC_ERROR_USB_ERROR; }
    }

    // Allocate the device list
    *dev_list = malloc(devCount * sizeof(sddc_devinfo_t));

    // Check each device
    int found = 0;
    libusb_device_handle* openDev;
    for (int i = 0; i < devCount; i++) {
        // Get the device from the list
        libusb_device* dev = devices[i];

        // Get the device descriptor. Fail silently on error as it might not even be a SDDC device.
        struct libusb_device_descriptor desc;
        int err = libusb_get_device_descriptor(dev, &desc);
        if (err != LIBUSB_SUCCESS) { continue; }

        // If the device is not an SDDC device, go to next device
        if (desc.idVendor != SDDC_VID || desc.idProduct != SDDC_PID) { continue; }

        // Open the device
        err = libusb_open(dev, &openDev);
        if (err != LIBUSB_SUCCESS) {
            fprintf(stderr, "Failed to open device: %d\n", err);
            continue; 
        }

        // Create entry
        sddc_devinfo_t* info = &((*dev_list)[found]);

        // Get the serial number
        err = libusb_get_string_descriptor_ascii(openDev, desc.iSerialNumber, info->serial, SDDC_SERIAL_MAX_LEN-1);
        if (err < LIBUSB_SUCCESS) {
            printf("Failed to get descriptor: %d\n", err);
            libusb_close(openDev);
            continue;
        }

        // Get the hardware info
        sddc_hwinfo_t hwinfo;
        err = sddc_fx3_get_info(openDev, &hwinfo, 0);
        if (err < LIBUSB_SUCCESS) {
            printf("Failed to get device info: %d\n", err);
            libusb_close(openDev);
            continue;
        }

        // Save the hardware info
        info->model = (sddc_model_t)hwinfo.model;
        info->firmwareMajor = hwinfo.firmwareConfigH;
        info->firmwareMinor = hwinfo.firmwareConfigL;

        // Close the device
        libusb_close(openDev);

        // Increment device counter
        found++;
    }

    // Free the libusb device list
    libusb_free_device_list(devices, 1);

    // Return the number of devices found
    return found;
}

void sddc_free_device_list(sddc_devinfo_t* dev_list) {
    // Free the device list if it exists
    if (dev_list) { free(dev_list); };
}

int sddc_get_device_list_fd(int fd, int vid, int pid, sddc_devinfo_t** dev_list) {
    *dev_list = NULL;

    if (fd < 0 || vid != SDDC_VID) { return SDDC_ERROR_NOT_FOUND; }

    libusb_device_handle* openDev;
    sddc_error_t err = sddc_wrap_fd(fd, &openDev);
    if (err != SDDC_SUCCESS) { return err; }

    if (pid == SDDC_UNINIT_PID) {
        fprintf(stderr, "Found uninitialized SDDC device, uploading firmware...\n");
        int fwErr = sddc_fx3_boot_upload_firmware(openDev, sddc_firmware_path);
        libusb_close(openDev);
        if (fwErr != LIBUSB_SUCCESS) {
            fprintf(stderr, "Failed to upload firmware to uninitialized device: %d\n", fwErr);
            return SDDC_ERROR_FIRMWARE_UPLOAD_FAILED;
        }
        return 0;
    }

    if (pid != SDDC_PID) {
        libusb_close(openDev);
        return SDDC_ERROR_NOT_FOUND;
    }

    *dev_list = malloc(sizeof(sddc_devinfo_t));
    if (!*dev_list) {
        libusb_close(openDev);
        return SDDC_ERROR_USB_ERROR;
    }

    sddc_devinfo_t* info = *dev_list;
    memset(info, 0, sizeof(sddc_devinfo_t));

    libusb_device* dev = libusb_get_device(openDev);
    struct libusb_device_descriptor desc;
    int usbErr = libusb_get_device_descriptor(dev, &desc);
    if (usbErr == LIBUSB_SUCCESS && desc.iSerialNumber) {
        usbErr = libusb_get_string_descriptor_ascii(openDev, desc.iSerialNumber, (unsigned char*)info->serial, SDDC_SERIAL_MAX_LEN - 1);
        if (usbErr < LIBUSB_SUCCESS) {
            fprintf(stderr, "Failed to get SDDC serial descriptor: %d\n", usbErr);
            info->serial[0] = '\0';
        }
    }

    if (!info->serial[0]) {
        strcpy(info->serial, "android-fd");
    }

    sddc_hwinfo_t hwinfo;
    usbErr = sddc_fx3_get_info(openDev, &hwinfo, 0);
    if (usbErr < LIBUSB_SUCCESS) {
        fprintf(stderr, "Failed to get SDDC device info: %d\n", usbErr);
        libusb_close(openDev);
        free(*dev_list);
        *dev_list = NULL;
        return SDDC_ERROR_USB_ERROR;
    }

    info->model = (sddc_model_t)hwinfo.model;
    info->firmwareMajor = hwinfo.firmwareConfigH;
    info->firmwareMinor = hwinfo.firmwareConfigL;

    libusb_close(openDev);
    return 1;
}

sddc_error_t sddc_open(const char* serial, sddc_dev_t** dev) {
    *dev = NULL;

    // Initialize libsddc in case it isn't already
    sddc_error_t initErr = sddc_init();
    if (initErr != SDDC_SUCCESS) { return initErr; }

    // Get a list of USB devices
    libusb_device** devices;
    int devCount = libusb_get_device_list(ctx, &devices);
    if (devCount < 0 || !devices) { return SDDC_ERROR_USB_ERROR; }

    // Initialize all uninitialized devices
    bool uninit = false;
    for (int i = 0; i < devCount; i++) {
        // Get the device from the list
        libusb_device* dev = devices[i];

        // Get the device descriptor. Fail silently on error as it might not even be a SDDC device.
        struct libusb_device_descriptor desc;
        int err = libusb_get_device_descriptor(dev, &desc);
        if (err != LIBUSB_SUCCESS) { continue; }

        // If it's not an uninitialized device, go to next device
        if (desc.idVendor != SDDC_UNINIT_VID || desc.idProduct != SDDC_UNINIT_PID) { continue; }

        // Initialize the device
        printf("Found uninitialized device, initializing...\n");
        // TODO: Check that the firmware path is valid
        sddc_error_t serr = sddc_init_device(dev, sddc_firmware_path);
        if (serr != SDDC_SUCCESS) { continue; }

        // Set the flag to wait the devices to start up
        uninit = true;
    }

    // If some uninitialized devices were found
    if (uninit) {
        // Free the device list
        libusb_free_device_list(devices, 1);

        // Wait for the devices to show back up
#ifdef _WIN32
        Sleep(SDDC_INIT_SEARCH_DELAY_MS);
#else
        usleep(SDDC_INIT_SEARCH_DELAY_MS * 1000);
#endif

        // Attempt to list devices again
        devCount = libusb_get_device_list(ctx, &devices);
        if (devCount < 0 || !devices) { return SDDC_ERROR_USB_ERROR; }
    }

    // Search through all USB device
    bool found = false;
    libusb_device_handle* openDev;
    for (int i = 0; i < devCount; i++) {
        // Get the device from the list
        libusb_device* dev = devices[i];

        // Get the device descriptor. Fail silently on error as it might not even be a SDDC device.
        struct libusb_device_descriptor desc;
        int err = libusb_get_device_descriptor(dev, &desc);
        if (err != LIBUSB_SUCCESS) { continue; }

        // If the device is not an SDDC device, go to next device
        if (desc.idVendor != SDDC_VID || desc.idProduct != SDDC_PID) { continue; }

        // Open the device
        err = libusb_open(dev, &openDev);
        if (err != LIBUSB_SUCCESS) {
            fprintf(stderr, "Failed to open device: %d\n", err);
            continue; 
        }

        // Get the serial number
        char dserial[SDDC_SERIAL_MAX_LEN];
        err = libusb_get_string_descriptor_ascii(openDev, desc.iSerialNumber, dserial, SDDC_SERIAL_MAX_LEN-1);
        if (err < LIBUSB_SUCCESS) {
            printf("Failed to get descriptor: %d\n", err);
            libusb_close(openDev);
            continue;
        }

        // Compare the serial number and give up if not a match
        if (strcmp(dserial, serial)) { continue; }

        // Get the device info
        // TODO

        // Set the found flag and stop searching
        found = true;
        break;
    }

    // Free the libusb device list
    libusb_free_device_list(devices, true);

    // If the device was not found, give up
    if (!found) { return SDDC_ERROR_NOT_FOUND; }

    // Claim the interface
    libusb_claim_interface(openDev, 0);

    // Allocate the device object
    *dev = malloc(sizeof(sddc_dev_t));

    // Initialize the device object
    (*dev)->openDev = openDev;
    //(*dev)->info = ; //TODO
    (*dev)->running = false;
    (*dev)->samplerate = 128e6;
    (*dev)->tunerFreq = 100e6;
    (*dev)->gpioState = SDDC_GPIO_SHUTDOWN | SDDC_GPIO_SEL0; // ADC shutdown and HF port selected
    (*dev)->port = SDDC_PORT_VHF;
    
    // Stop everything in case the device is partially started
    printf("Stopping...\n");
    sddc_stop(*dev);

    // TODO: Setup all of the other state
    sddc_gpio_put(*dev, SDDC_GPIO_SEL0, false);
    sddc_gpio_put(*dev, SDDC_GPIO_SEL1, true);
    sddc_gpio_put(*dev, SDDC_GPIO_VHF_EN, true);
    sddc_tuner_start((*dev)->openDev, 16e6);
    sddc_tuner_tune((*dev)->openDev, 100e6);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_R82XX_ATT, 15);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_R83XX_VGA, 9);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_AD8340_VGA, 5);

    return SDDC_SUCCESS;
}

sddc_error_t sddc_open_fd(int fd, const char* serial, sddc_dev_t** dev) {
    *dev = NULL;

    libusb_device_handle* openDev;
    sddc_error_t err = sddc_wrap_fd(fd, &openDev);
    if (err != SDDC_SUCCESS) { return err; }

    libusb_device* usbDev = libusb_get_device(openDev);
    struct libusb_device_descriptor desc;
    int usbErr = libusb_get_device_descriptor(usbDev, &desc);
    if (usbErr != LIBUSB_SUCCESS || desc.idVendor != SDDC_VID || desc.idProduct != SDDC_PID) {
        libusb_close(openDev);
        return SDDC_ERROR_NOT_FOUND;
    }

    if (serial && serial[0]) {
        char dserial[SDDC_SERIAL_MAX_LEN];
        usbErr = libusb_get_string_descriptor_ascii(openDev, desc.iSerialNumber, (unsigned char*)dserial, SDDC_SERIAL_MAX_LEN - 1);
        if (usbErr >= LIBUSB_SUCCESS) {
            dserial[usbErr] = '\0';
            if (strcmp(dserial, serial)) {
                libusb_close(openDev);
                return SDDC_ERROR_NOT_FOUND;
            }
        }
    }

    libusb_claim_interface(openDev, 0);

    *dev = malloc(sizeof(sddc_dev_t));
    if (!*dev) {
        libusb_close(openDev);
        return SDDC_ERROR_USB_ERROR;
    }

    (*dev)->openDev = openDev;
    (*dev)->running = false;
    (*dev)->samplerate = 128e6;
    (*dev)->tunerFreq = 100e6;
    (*dev)->gpioState = SDDC_GPIO_SHUTDOWN | SDDC_GPIO_SEL0;
    (*dev)->port = SDDC_PORT_VHF;

    sddc_stop(*dev);
    sddc_gpio_put(*dev, SDDC_GPIO_SEL0, false);
    sddc_gpio_put(*dev, SDDC_GPIO_SEL1, true);
    sddc_gpio_put(*dev, SDDC_GPIO_VHF_EN, true);
    sddc_tuner_start((*dev)->openDev, 16e6);
    sddc_tuner_tune((*dev)->openDev, 100e6);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_R82XX_ATT, 15);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_R83XX_VGA, 9);
    sddc_fx3_set_param((*dev)->openDev, SDDC_PARAM_AD8340_VGA, 5);

    return SDDC_SUCCESS;
}

void sddc_close(sddc_dev_t* dev) {
    // Stop everything
    sddc_stop(dev);

    // Release the interface
    libusb_release_interface(dev->openDev, 0);

    // Close the USB device
    libusb_close(dev->openDev);

    // Free the device struct
    free(dev);
}

sddc_range_t sddc_get_samplerate_range(sddc_dev_t* dev) {
    // All devices have the same samplerate range
    sddc_range_t range = { 8e6, 128e6, 0 };
    return range;
}

int sddc_gpio_set(sddc_dev_t* dev, sddc_gpio_t gpios) {
    // Update the state
    dev->gpioState = gpios;

    // Push to the device
    return sddc_fx3_gpio(dev->openDev, gpios);
}

static int sddc_gpio_put(sddc_dev_t* dev, sddc_gpio_t gpios, bool value) {
    // Update the state of the given GPIOs only
    return sddc_gpio_set(dev, (dev->gpioState & (~gpios)) | (value ? gpios : 0));
}

sddc_error_t sddc_set_samplerate(sddc_dev_t* dev, uint32_t samplerate) {
    // Update the state
    dev->samplerate = samplerate;

    // If running, send the new sampling rate to the device
    if (dev->running) {
        int err = sddc_adc_set_samplerate(dev->openDev, samplerate);
        if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
    }

    // Return successfully
    return SDDC_SUCCESS;
}

sddc_error_t sddc_set_dithering(sddc_dev_t* dev, bool enabled) {
    // Update the GPIOs according to the desired state
    int err = sddc_gpio_put(dev, SDDC_GPIO_DITHER, enabled);
    return (err < LIBUSB_SUCCESS) ? SDDC_ERROR_USB_ERROR : SDDC_SUCCESS;
}

sddc_error_t sddc_set_randomizer(sddc_dev_t* dev, bool enabled) {
    // Update the GPIOs according to the desired state
    int err = sddc_gpio_put(dev, SDDC_GPIO_RANDOM, enabled);
    return (err < LIBUSB_SUCCESS) ? SDDC_ERROR_USB_ERROR : SDDC_SUCCESS;
}

sddc_error_t sddc_set_port(sddc_dev_t* dev, sddc_port_t port) {
    int err;
    switch (port) {
    case SDDC_PORT_VHF:
        sddc_fx3_set_param(dev->openDev, SDDC_PARAM_DAT31_ATT, 63);
        err = sddc_gpio_put(dev, SDDC_GPIO_VHF_EN, true);
        if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
        err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_AD8340_VGA, 0x80 | 3);
        if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
        err = sddc_tuner_start(dev->openDev, 16000000);
        break;
    case SDDC_PORT_HF:
        sddc_tuner_stop(dev->openDev);
        err = sddc_gpio_put(dev, SDDC_GPIO_VHF_EN, false);
        break;
    default:
        return SDDC_ERROR_NOT_IMPLEMENTED;
    }
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
    dev->port = port;
    return SDDC_SUCCESS;
}

sddc_error_t sddc_set_rf_attenuator(sddc_dev_t* dev, uint16_t value) {
    int err;
    if (dev->port == SDDC_PORT_HF) {
        if (value > 63) { value = 63; }
        err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_DAT31_ATT, 63 - value);
    }
    else {
        if (value > 28) { value = 28; }
        err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_R82XX_ATT, value);
    }
    return (err < LIBUSB_SUCCESS) ? SDDC_ERROR_USB_ERROR : SDDC_SUCCESS;
}

sddc_error_t sddc_set_if_gain(sddc_dev_t* dev, uint16_t value) {
    int err;
    if (dev->port == SDDC_PORT_HF) {
        if (value > 126) { value = 126; }
        uint16_t gain = (value > 18) ? (0x80 | (value - 18 + 3)) : (value + 1);
        err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_AD8340_VGA, gain);
    }
    else {
        if (value > 15) { value = 15; }
        err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_R83XX_VGA, value);
    }
    return (err < LIBUSB_SUCCESS) ? SDDC_ERROR_USB_ERROR : SDDC_SUCCESS;
}

sddc_error_t sddc_set_vhf_attenuator(sddc_dev_t* dev, uint16_t value) {
    int err = sddc_fx3_set_param(dev->openDev, SDDC_PARAM_VHF_ATT, value);
    return (err < LIBUSB_SUCCESS) ? SDDC_ERROR_USB_ERROR : SDDC_SUCCESS;
}

sddc_error_t sddc_set_tuner_frequency(sddc_dev_t* dev, uint64_t frequency) {
    // Update the state
    dev->tunerFreq = frequency;

    // If running, send the new frequency to the device
    if (dev->running) {
        int err = sddc_tuner_tune(dev->openDev, frequency);
        if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
    }

    // Return successfully
    return SDDC_SUCCESS;
}

sddc_error_t sddc_start(sddc_dev_t* dev) {
    // De-assert the shutdown pin
    int err = sddc_gpio_put(dev, SDDC_GPIO_SHUTDOWN, false);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }

    // Start the tuner (TODO: Check if in VHF mode)

    // Start the ADC
    err = sddc_adc_set_samplerate(dev->openDev, dev->samplerate);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }

    // Start the FX3
    err = sddc_fx3_start(dev->openDev);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }

    // Update the state
    dev->running = true;

    // Return successfully
    return SDDC_SUCCESS;
}

sddc_error_t sddc_stop(sddc_dev_t* dev) {
    // Stop the FX3
    int err = sddc_fx3_stop(dev->openDev);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
    
    // Stop the tuner
    err = sddc_tuner_stop(dev->openDev);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }

    // Stop the ADC
    err = sddc_adc_set_samplerate(dev->openDev, 0);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }

    // Set the GPIOs for standby mode
    err = sddc_gpio_put(dev, SDDC_GPIO_SHUTDOWN, true);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }    

    // Update the state
    dev->running = false;

    // Return successfully
    return SDDC_SUCCESS;
}

sddc_error_t sddc_rx(sddc_dev_t* dev, int16_t* samples, int count) {
    // Read samples from the device
    int bytesRead = 0;
    int err = libusb_bulk_transfer(dev->openDev, LIBUSB_ENDPOINT_IN | 1, samples, count * sizeof(int16_t), &bytesRead, SDDC_TIMEOUT_MS);
    if (err < LIBUSB_SUCCESS) { return SDDC_ERROR_USB_ERROR; }
    return SDDC_SUCCESS;
}
