/* Minimal hardware smoke test: open the device, read its identity/version
 * info, close it. No audio/PulseAudio dependency, unlike example1.c. */
#include "g39ddcapi.h"
#include <stdio.h>
#include <dlfcn.h>
#include <inttypes.h>

int main(void)
{
    void *API = dlopen("libg39ddcapi.so", RTLD_LAZY);
    if (!API) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }

    G39DDC_OPEN_DEVICE    OpenDevice    = (G39DDC_OPEN_DEVICE)dlsym(API, "OpenDevice");
    G39DDC_CLOSE_DEVICE   CloseDevice   = (G39DDC_CLOSE_DEVICE)dlsym(API, "CloseDevice");
    G39DDC_GET_DEVICE_INFO GetDeviceInfo = (G39DDC_GET_DEVICE_INFO)dlsym(API, "GetDeviceInfo");

    if (!OpenDevice || !CloseDevice || !GetDeviceInfo) {
        printf("dlsym failed for one or more required symbols\n");
        return 1;
    }

    int32_t hDevice = OpenDevice(G39DDC_OPEN_FIRST);
    if (hDevice < 0) {
        printf("OpenDevice failed, returned %d\n", hDevice);
        return 1;
    }
    printf("OpenDevice OK, handle=%d\n", hDevice);

    G39DDC_DEVICE_INFO info;
    int status = GetDeviceInfo(hDevice, &info, sizeof(info));
    if (status == 0) {
        printf("GetDeviceInfo failed, status=%d\n", status);
        CloseDevice(hDevice);
        return 1;
    }

    printf("DevicePath:    %s\n", info.DevicePath);
    printf("SerialNumber:  %s\n", info.SerialNumber);
    printf("HWVersion:     0x%04x\n", info.HWVersion);
    printf("FWVersion:     0x%04x\n", info.FWVersion);
    printf("ChannelCount:  %u\n", info.ChannelCount);
    printf("FreqRange:     %" PRIu64 " - %" PRIu64 " Hz\n",
           info.FrontEndMinFrequency, info.FrontEndMaxFrequency);

    CloseDevice(hDevice);
    printf("CloseDevice OK\n");
    return 0;
}
