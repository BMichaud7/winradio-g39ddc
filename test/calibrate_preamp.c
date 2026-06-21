#include "g39ddcapi.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
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
            power_acc += re*re + im*im; n_acc++;
        }
    }
}

int main(void)
{
    void *API = dlopen("libg39ddcapi.so", RTLD_LAZY);
    G39DDC_OPEN_DEVICE OpenDevice = (G39DDC_OPEN_DEVICE)dlsym(API, "OpenDevice");
    G39DDC_SET_POWER SetPower = (G39DDC_SET_POWER)dlsym(API, "SetPower");
    G39DDC_GET_DDC1_COUNT GetDDC1Count = (G39DDC_GET_DDC1_COUNT)dlsym(API, "GetDDC1Count");
    G39DDC_GET_DDC_INFO GetDDCInfo = (G39DDC_GET_DDC_INFO)dlsym(API, "GetDDCInfo");
    G39DDC_SET_DDC1 SetDDC1 = (G39DDC_SET_DDC1)dlsym(API, "SetDDC1");
    G39DDC_SET_CALLBACKS SetCallbacks = (G39DDC_SET_CALLBACKS)dlsym(API, "SetCallbacks");
    G39DDC_SET_FREQUENCY SetFrequency = (G39DDC_SET_FREQUENCY)dlsym(API, "SetFrequency");
    G39DDC_START_DDC1 StartDDC1 = (G39DDC_START_DDC1)dlsym(API, "StartDDC1");
    G39DDC_STOP_DDC1 StopDDC1 = (G39DDC_STOP_DDC1)dlsym(API, "StopDDC1");
    G39DDC_SET_PREAMPLIFIER SetPreamplifier = (G39DDC_SET_PREAMPLIFIER)dlsym(API, "SetPreamplifier");
    G39DDC_GET_PREAMPLIFIER GetPreamplifier = (G39DDC_GET_PREAMPLIFIER)dlsym(API, "GetPreamplifier");

    int32_t h = OpenDevice(G39DDC_OPEN_FIRST);
    SetPower(h, 1);
    sleep(1);

    uint32_t ch = 0, count = 0;
    GetDDC1Count(h, ch, &count);
    uint32_t best_idx = 0, best_diff = 0xFFFFFFFF;
    for (uint32_t i = 0; i < count; i++) {
        G39DDC_DDC_INFO info;
        if (GetDDCInfo(i, &info)) {
            uint32_t diff = info.SampleRate > 2000000 ? info.SampleRate-2000000 : 2000000-info.SampleRate;
            if (diff < best_diff) { best_diff = diff; best_idx = i; }
        }
    }
    SetDDC1(h, ch, best_idx);

    G39DDC_CALLBACKS cb; memset(&cb, 0, sizeof(cb));
    cb.DDC1StreamCallback = DDC1Callback;
    SetCallbacks(h, &cb, 0);
    SetFrequency(h, ch, 99500000);
    StartDDC1(h, ch, 4096);
    sleep(1);

    printf("preamp  measured_dB  (set_ok, readback)\n");
    for (int p = 0; p < 2; p++) {
        int ok = SetPreamplifier(h, p);
        int readback = -1;
        GetPreamplifier(h, &readback);
        usleep(500000);
        power_acc = 0; n_acc = 0;
        usleep(300000);
        double avg = n_acc ? power_acc/n_acc : 0;
        printf("%6d  %10.2f   (ok=%d, readback=%d)\n", p, 10*log10(avg+1e-12), ok, readback);
    }

    StopDDC1(h, ch);
    SetCallbacks(h, NULL, 0);
    SetPower(h, 0);
    return 0;
}
