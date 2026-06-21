/* Streaming smoke test: open device, pick a DDC1 mode, tune, stream a few
 * buffers of real IQ via the callback, stop, close. Validates the actual
 * data path before writing the full SoapySDR wrapper around it. */
#include "g39ddcapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <inttypes.h>

static G39DDC_STOP_DDC1 StopDDC1;
static int32_t g_hDevice;
static volatile uint32_t g_buffers_received = 0;
static volatile uint32_t g_samples_received = 0;

static void DDC1Callback(uint32_t Channel, const void *Buffer,
                          uint32_t NumberOfSamples, uint32_t BitsPerSample,
                          uintptr_t UserData)
{
    (void)UserData;
    g_buffers_received++;
    g_samples_received += NumberOfSamples;
    if (g_buffers_received <= 3) {
        printf("[cb] ch=%u samples=%u bits=%u  first I/Q: ",
               Channel, NumberOfSamples, BitsPerSample);
        if (BitsPerSample == 16) {
            const int16_t *p = (const int16_t *)Buffer;
            printf("I=%d Q=%d\n", p[0], p[1]);
        } else if (BitsPerSample == 32) {
            const int32_t *p = (const int32_t *)Buffer;
            printf("I=%d Q=%d\n", p[0], p[1]);
        } else {
            printf("(unexpected BitsPerSample)\n");
        }
    }
}

int main(void)
{
    void *API = dlopen("libg39ddcapi.so", RTLD_LAZY);
    if (!API) { printf("dlopen failed: %s\n", dlerror()); return 1; }

    G39DDC_OPEN_DEVICE     OpenDevice     = (G39DDC_OPEN_DEVICE)dlsym(API, "OpenDevice");
    G39DDC_CLOSE_DEVICE    CloseDevice    = (G39DDC_CLOSE_DEVICE)dlsym(API, "CloseDevice");
    G39DDC_SET_POWER       SetPower       = (G39DDC_SET_POWER)dlsym(API, "SetPower");
    G39DDC_GET_DDC1_COUNT  GetDDC1Count   = (G39DDC_GET_DDC1_COUNT)dlsym(API, "GetDDC1Count");
    G39DDC_GET_DDC_INFO    GetDDCInfo     = (G39DDC_GET_DDC_INFO)dlsym(API, "GetDDCInfo");
    G39DDC_SET_DDC1        SetDDC1        = (G39DDC_SET_DDC1)dlsym(API, "SetDDC1");
    G39DDC_SET_CALLBACKS   SetCallbacks   = (G39DDC_SET_CALLBACKS)dlsym(API, "SetCallbacks");
    G39DDC_SET_FREQUENCY   SetFrequency   = (G39DDC_SET_FREQUENCY)dlsym(API, "SetFrequency");
    G39DDC_START_DDC1      StartDDC1      = (G39DDC_START_DDC1)dlsym(API, "StartDDC1");
    StopDDC1 = (G39DDC_STOP_DDC1)dlsym(API, "StopDDC1");

    if (!OpenDevice || !CloseDevice || !SetPower || !GetDDC1Count || !GetDDCInfo
        || !SetDDC1 || !SetCallbacks || !SetFrequency || !StartDDC1 || !StopDDC1) {
        printf("dlsym failed for one or more required symbols\n");
        return 1;
    }

    g_hDevice = OpenDevice(G39DDC_OPEN_FIRST);
    if (g_hDevice < 0) { printf("OpenDevice failed: %d\n", g_hDevice); return 1; }
    printf("OpenDevice OK, handle=%d\n", g_hDevice);

    if (!SetPower(g_hDevice, 1)) { printf("SetPower(1) failed\n"); return 1; }
    printf("SetPower(1) OK\n");
    sleep(1); /* let the front end stabilize */

    uint32_t channel = 0;
    uint32_t count = 0;
    if (!GetDDC1Count(g_hDevice, channel, &count) || count == 0) {
        printf("GetDDC1Count failed or zero, count=%u\n", count);
        SetPower(g_hDevice, 0);
        CloseDevice(g_hDevice);
        return 1;
    }
    printf("DDC1 mode count for channel %u: %u\n", channel, count);

    /* List all available DDC1 modes so we know the real rate/bandwidth menu */
    G39DDC_DDC_INFO best_info;
    uint32_t best_idx = 0;
    memset(&best_info, 0, sizeof(best_info));
    for (uint32_t i = 0; i < count; i++) {
        G39DDC_DDC_INFO info;
        if (GetDDCInfo(i, &info)) {
            printf("  DDC1[%u]: SampleRate=%u Bandwidth=%u BitsPerSample=%u\n",
                   i, info.SampleRate, info.Bandwidth, info.BitsPerSample);
            if (info.SampleRate > best_info.SampleRate) {
                best_info = info;
                best_idx = i;
            }
        } else {
            printf("  DDC1[%u]: GetDDCInfo failed\n", i);
        }
    }
    printf("Selecting DDC1[%u] (highest sample rate=%u)\n", best_idx, best_info.SampleRate);

    if (!SetDDC1(g_hDevice, channel, best_idx)) {
        printf("SetDDC1 failed\n");
        SetPower(g_hDevice, 0);
        CloseDevice(g_hDevice);
        return 1;
    }

    G39DDC_CALLBACKS callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.DDC1StreamCallback = DDC1Callback;
    if (!SetCallbacks(g_hDevice, &callbacks, 0)) {
        printf("SetCallbacks failed\n");
        SetPower(g_hDevice, 0);
        CloseDevice(g_hDevice);
        return 1;
    }

    uint64_t freq_hz = 100000000; /* 100 MHz, well within 8MHz-3.5GHz range */
    if (!SetFrequency(g_hDevice, channel, freq_hz)) {
        printf("SetFrequency failed\n");
        SetCallbacks(g_hDevice, NULL, 0);
        SetPower(g_hDevice, 0);
        CloseDevice(g_hDevice);
        return 1;
    }
    printf("SetFrequency(%" PRIu64 " Hz) OK\n", freq_hz);

    uint32_t samples_per_buffer = 4096;
    if (!StartDDC1(g_hDevice, channel, samples_per_buffer)) {
        printf("StartDDC1 failed\n");
        SetCallbacks(g_hDevice, NULL, 0);
        SetPower(g_hDevice, 0);
        CloseDevice(g_hDevice);
        return 1;
    }
    printf("StartDDC1 OK, streaming for 3 seconds...\n");

    sleep(3);

    StopDDC1(g_hDevice, channel);
    printf("StopDDC1 OK. buffers_received=%u samples_received=%u\n",
           g_buffers_received, g_samples_received);

    SetCallbacks(g_hDevice, NULL, 0);
    SetPower(g_hDevice, 0);
    CloseDevice(g_hDevice);
    printf("CloseDevice OK\n");

    return (g_buffers_received > 0) ? 0 : 1;
}
