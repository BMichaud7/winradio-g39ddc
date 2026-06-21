/* Empirically determine the SetFrontEndFrequency / SetDDC1Frequency offset
 * convention: park the front end at a fixed center, then sweep the DDC1
 * offset and measure power to find where the known FM peaks (93.5, 99.5
 * MHz) actually land relative to the requested offset. */
#include "g39ddcapi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <math.h>

static volatile double power_acc = 0.0;
static volatile uint64_t n_acc = 0;

static void DDC1Callback(uint32_t Channel, const void *Buffer,
                          uint32_t NumberOfSamples, uint32_t BitsPerSample,
                          uintptr_t UserData)
{
    (void)Channel; (void)UserData;
    if (BitsPerSample == 16) {
        const int16_t *p = (const int16_t *)Buffer;
        for (uint32_t i = 0; i < NumberOfSamples; i++) {
            double re = p[2*i] / 32768.0, im = p[2*i+1] / 32768.0;
            power_acc += re*re + im*im;
            n_acc++;
        }
    } else if (BitsPerSample == 32) {
        const int32_t *p = (const int32_t *)Buffer;
        for (uint32_t i = 0; i < NumberOfSamples; i++) {
            double re = p[2*i] / 2147483648.0, im = p[2*i+1] / 2147483648.0;
            power_acc += re*re + im*im;
            n_acc++;
        }
    }
}

int main(void)
{
    void *API = dlopen("libg39ddcapi.so", RTLD_LAZY);
    G39DDC_OPEN_DEVICE OpenDevice = (G39DDC_OPEN_DEVICE)dlsym(API, "OpenDevice");
    G39DDC_CLOSE_DEVICE CloseDevice = (G39DDC_CLOSE_DEVICE)dlsym(API, "CloseDevice");
    G39DDC_SET_POWER SetPower = (G39DDC_SET_POWER)dlsym(API, "SetPower");
    G39DDC_GET_DDC1_COUNT GetDDC1Count = (G39DDC_GET_DDC1_COUNT)dlsym(API, "GetDDC1Count");
    G39DDC_GET_DDC_INFO GetDDCInfo = (G39DDC_GET_DDC_INFO)dlsym(API, "GetDDCInfo");
    G39DDC_SET_DDC1 SetDDC1 = (G39DDC_SET_DDC1)dlsym(API, "SetDDC1");
    G39DDC_SET_CALLBACKS SetCallbacks = (G39DDC_SET_CALLBACKS)dlsym(API, "SetCallbacks");
    G39DDC_SET_FRONT_END_FREQUENCY SetFrontEndFrequency = (G39DDC_SET_FRONT_END_FREQUENCY)dlsym(API, "SetFrontEndFrequency");
    G39DDC_GET_FRONT_END_FREQUENCY GetFrontEndFrequency = (G39DDC_GET_FRONT_END_FREQUENCY)dlsym(API, "GetFrontEndFrequency");
    G39DDC_SET_DDC1_FREQUENCY SetDDC1Frequency = (G39DDC_SET_DDC1_FREQUENCY)dlsym(API, "SetDDC1Frequency");
    G39DDC_GET_DDC1_FREQUENCY GetDDC1Frequency = (G39DDC_GET_DDC1_FREQUENCY)dlsym(API, "GetDDC1Frequency");
    G39DDC_START_DDC1 StartDDC1 = (G39DDC_START_DDC1)dlsym(API, "StartDDC1");
    G39DDC_STOP_DDC1 StopDDC1 = (G39DDC_STOP_DDC1)dlsym(API, "StopDDC1");

    if (!SetFrontEndFrequency || !GetFrontEndFrequency || !SetDDC1Frequency || !GetDDC1Frequency) {
        printf("missing symbols\n"); return 1;
    }

    int32_t h = OpenDevice(G39DDC_OPEN_FIRST);
    if (h < 0) { printf("OpenDevice failed\n"); return 1; }
    SetPower(h, 1);
    sleep(1);

    uint32_t ch = 0, count = 0;
    GetDDC1Count(h, ch, &count);
    /* pick a moderate-rate DDC1 mode for fast settle */
    uint32_t best_idx = 0; uint32_t best_rate_diff = 0xFFFFFFFF;
    for (uint32_t i = 0; i < count; i++) {
        G39DDC_DDC_INFO info;
        if (GetDDCInfo(i, &info)) {
            uint32_t diff = info.SampleRate > 2000000 ? info.SampleRate - 2000000 : 2000000 - info.SampleRate;
            if (diff < best_rate_diff) { best_rate_diff = diff; best_idx = i; }
        }
    }
    SetDDC1(h, ch, best_idx);

    G39DDC_CALLBACKS cb; memset(&cb, 0, sizeof(cb));
    cb.DDC1StreamCallback = DDC1Callback;
    SetCallbacks(h, &cb, 0);

    uint64_t fe_request = 100000000; /* 100 MHz */
    if (!SetFrontEndFrequency(h, fe_request)) { printf("SetFrontEndFrequency failed\n"); return 1; }
    uint64_t fe_actual = 0;
    GetFrontEndFrequency(h, &fe_actual);
    printf("Requested front end %" PRIu64 " Hz -> actual %" PRIu64 " Hz\n", fe_request, fe_actual);

    StartDDC1(h, ch, 4096);
    sleep(1);

    /* Sweep DDC1 offset from -8MHz to +8MHz in 0.5MHz steps, looking for
     * peaks. Known real peaks (from earlier full-band scan) are at
     * absolute 93.5 MHz and 99.5 MHz. If fe_actual==100e6, the expected
     * offsets (if offset = target - front_end) are -6.5e6 and -0.5e6. */
    printf("offset_Hz  avg_power_dB\n");
    for (int32_t off = -8000000; off <= 8000000; off += 500000) {
        SetDDC1Frequency(h, ch, off);
        usleep(300000);
        power_acc = 0.0; n_acc = 0;
        usleep(200000);
        double avg = n_acc ? power_acc / n_acc : 0.0;
        printf("%9d  %8.2f\n", off, 10.0*log10(avg + 1e-12));
    }

    StopDDC1(h, ch);
    SetCallbacks(h, NULL, 0);
    SetPower(h, 0);
    CloseDevice(h);
    return 0;
}
