#ifndef __USB_INFO_H_
#define __USB_INFO_H_

#include <linux/types.h>
#include <linux/ioctl.h>

#define CMD_GET_STATUS	_IOR(0xFF, 123, unsigned char) 

#define DEV_NAME "/proc/usb_monitor"

#define MAX_EPOLL_EVENTS         1
#define KERNEL_DATA_LENG        128
#define MONITOR_DISABLE       0x00
#define MONITOR_ENABLE        0xff

size_t BUFFER_SIZE = 1024;

struct DataInfo{
    uint8_t kernel_time[8];       //8 Byte 
    uint8_t status;               //1 byte
    int8_t  name[128];            //128 byte
};

class UsbMonitorInfo {
public:
    struct DataInfo info;
};

#endif
