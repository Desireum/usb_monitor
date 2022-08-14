/**
    Application for usb device plugging detection
    @file UsbMonitorApp.cpp
    @author Wang Taorui
    @version 2022/8/14
*/
#include <cinttypes>
#include <numeric>
#include <set>
#include <string>
#include <string.h>
#include <tuple>
#include <vector>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include "RingBuffer.h"
#include "UsbInfo.h"

using namespace std;

pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  fifo_nonzero;
static volatile int64_t isEmpty = 0;
static volatile int64_t fifo_size = 0;

class UsbMonitorDevice {
public:
    UsbMonitorDevice(char* name){
        mDev_name = name;
    }

    ~UsbMonitorDevice(){
        epoll_ctl(mEpollfd, EPOLL_CTL_DEL, mFd, &mEpev);
        close(mEpollfd);
        close(mFd);
    }

    int InitSetup(){
        //open "/proc/usb_monitor"
        mFd = open(mDev_name, O_RDWR); 
        if (mFd == -1) {
        printf("open %s fail, Check!!!\n",mDev_name);
            return errno;
        }
        //epoll
        mEpollfd = epoll_create(MAX_EPOLL_EVENTS);
        if (mEpollfd == -1) {
            printf("epoll_create failed errno = %d ", errno);	
            return errno;
        }
        printf("epoll_create ok epollfd= %d \n", mEpollfd);

        //add fd for epoll
        memset(&mEpev, 0, sizeof(mEpev));
        mEpev.data.fd = mFd;
        mEpev.events = EPOLLIN;
        if (epoll_ctl(mEpollfd, EPOLL_CTL_ADD, mFd, &mEpev) < 0) {
            printf("epoll_ctl failed, errno = %d \n", errno);
            return errno;
        }
        return 0;
    }

    int getFd() { return mFd; };
    int getepollfd() { return mEpollfd; };
    char* getBuffer() { return mBuf; };

    UsbMonitorInfo& GetFristDataInfo(){
        return mRingBuffer.Get(0);
    }
    void PPopFrontDatainfo(){
        mRingBuffer.PopFront();
    }

    void PopBackDatainfo(){
        mRingBuffer.PopBack();
    }

    UsbMonitorInfo& GetBackDataInfo(){
        return mRingBuffer.Back();
    }

    void AppendDatainfo(UsbMonitorInfo& info){
        mRingBuffer.Append(info);
    }

    size_t GetFifoSize(){
        return mRingBuffer.GetSize();
    }

    void FifoReset(size_t capacity) {
        mRingBuffer.Reset(capacity);
    }

    bool FifoIsEmpty() {
        return mRingBuffer.IsEmpty();
    }

private:
    int mFd; //"/proc/usb_monitor"
    int mEpollfd;
    struct epoll_event mEpev;
    char *mDev_name;
    char mBuf[50];
    RingBuffer<UsbMonitorInfo> mRingBuffer;
};

/**
 * Monitor USB device plugging and unplugging status, and output and record that status
 *
 * @param arg: device name; /proc/usb_monitor;
 */
static void * DoUsbMonitor(void *arg){
        int ret;
        //static int index = 0;
        ssize_t leng = 0, i = 0;
        struct epoll_event epev;
        UsbMonitorInfo deviceinfo;
        UsbMonitorDevice* device = (UsbMonitorDevice*)arg;
        char* buf = device->getBuffer();
        device->FifoReset(BUFFER_SIZE);

        while(1){
            printf("usb_monitor epoll_wait... \n");
            ret = epoll_wait(device->getepollfd(), &epev, MAX_EPOLL_EVENTS, -1);
            if (ret == -1 && errno != EINTR) {
                printf("usb_monitor epoll_wait failed; errno=%d\n", errno);
                return (void*)(-1);
            }

            // Max 128 byte;
            // Set this parameter in UsbInfo.h;
            leng  = read(device->getfd(), buf, kernel_data_leng);

            if (leng > 0){
                printf("The length of device information is %d\n",leng);

                // Record kernel time;
                // size : 8 Bytes;
                for (i = 0; i < 8; i++){
                    deviceinfo.info.kernel_time[i] = buf[i];
                    printf("kernel_time[%d] = 0x%x \n", i, deviceinfo.info.kernel_time[i]);
                }

                // Record USB plugging status;
                deviceinfo.info.status = buf[8];

                // Record USB device name;
                memcpy(deviceinfo.info.name, buf+9,leng-9);

                //Output usb device plugging information
                if(deviceinfo.info.status==1){
                    printf("Device name: %s ====== PLUG IN \n",deviceinfo.info.name);
                }else{
                    printf("Device name: %s ====== PLUG OUT \n",deviceinfo.info.name);
                }
                printf("\n");

                // Get lock;
                ret = pthread_mutex_lock(&data_mutex);
                if (ret != 0) {
                    printf("Error on pthread_mutex_lock(), ret = %d\n", ret);
                    return (void *)(-1);
                }

                // Save infomation;
                device->AppendDatainfo(deviceinfo);
                fifo_size = device->GetFifoSize();

                // Unlock;
                ret = pthread_mutex_unlock(&data_mutex);
                if (ret != 0) {
                    printf("Error on pthread_mutex_unlock(), ret = %d\n", ret);
                    return (void *)(-1);
                }

                if (isEmpty){
                    for (i = 0; i < isEmpty; i++){
                        pthread_cond_signal(&fifo_nonzero);
                    }
                }
                printf("Current BufferSize = %ld \n", fifo_size);
            }
        }
}


int main(){

    UsbMonitorDevice* monitorDevice = new UsbMonitorDevice((char*)DEV_NAME);

    if ( monitorDevice->InitSetup() != 0){
        printf("SuspendMonitorDevice::InitSetup fail \n");
        return -1;
    }
    printf("SuspendMonitorDevice::InitSetup OK \n");

    DoUsbMonitor((void*)monitorDevice);

    return 0;
}
