#ifndef _USB_DRIVER_H_
#define _USB_DRIVER_H_

bool get_usb_mounted(void);
bool get_usb_suspended(void);

#include <stdint.h>

// How far the host got with this device, for the Network Input addon's status reply.
struct UsbDebugCounters {
    uint32_t deviceDescriptors;
    uint32_t configDescriptors;
    uint32_t hidReportDescriptors;
    uint32_t strings;
    uint32_t mounts;
    uint32_t unmounts;
    uint32_t suspends;
    uint32_t events[8]; // TinyUSB device events by id: bus reset, unplugged, SOF, suspend, resume, setup, xfer
};
extern UsbDebugCounters usbDebug;

#endif // #ifndef _USB_DRIVER_H_