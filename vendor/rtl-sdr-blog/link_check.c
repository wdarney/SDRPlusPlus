#include "rtl-sdr.h"

/* Resolve the driver and USB dependency without opening attached hardware. */
int main(void)
{
    int (*volatile open_device)(rtlsdr_dev_t **, uint32_t) = rtlsdr_open;
    int (*volatile close_device)(rtlsdr_dev_t *) = rtlsdr_close;
    int (*volatile tune)(rtlsdr_dev_t *, uint32_t) = rtlsdr_set_center_freq;
#ifdef __ANDROID__
    int (*volatile open_fd)(rtlsdr_dev_t **, int) = rtlsdr_open_sys_dev;
    if (!open_fd) return 1;
#endif
    return !open_device || !close_device || !tune;
}
