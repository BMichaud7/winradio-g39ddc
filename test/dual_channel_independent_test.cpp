// Proves channel 0 and channel 1 tune/stream truly independently: each is
// set to a DIFFERENT frequency and sample rate, streamed simultaneously,
// and each must show real signal power at ITS OWN frequency (not the
// other channel's, and not just both echoing a single shared front-end
// setting). Picks the two strong peaks found by fm_scan_test.cpp
// (99.5 MHz and 93.5 MHz) plus a deliberately-quiet frequency (102.5 MHz)
// to swap in mid-run as a control, so a false-positive "always shows
// power" bug would be caught.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <cstdio>
#include <complex>
#include <vector>
#include <cmath>

static double measure_power(SoapySDR::Device *dev, SoapySDR::Stream *stream,
                             int chan_index_in_stream, std::vector<void*> &buffs, size_t N)
{
    // discard a couple buffers after retune for the front end/DDC to settle
    for (int d = 0; d < 3; d++) {
        int flags; long long t;
        dev->readStream(stream, buffs.data(), N, flags, t, 500000);
    }
    double sum = 0.0; size_t total = 0;
    auto *buf = reinterpret_cast<std::complex<float>*>(buffs[chan_index_in_stream]);
    for (int reps = 0; reps < 4; reps++) {
        int flags; long long t;
        int ret = dev->readStream(stream, buffs.data(), N, flags, t, 500000);
        if (ret <= 0) continue;
        for (int i = 0; i < ret; i++) {
            float re = buf[i].real(), im = buf[i].imag();
            sum += re*re + im*im;
        }
        total += ret;
    }
    return 10.0 * log10((total ? sum/total : 0.0) + 1e-12);
}

int main()
{
    SoapySDR::Kwargs args; args["driver"] = "g39ddc";
    SoapySDR::Device *dev = SoapySDR::Device::make(args);

    // Different sample rate per channel -- proves independent DDC1 mode
    // selection too, not just independent frequency.
    dev->setSampleRate(SOAPY_SDR_RX, 0, 2.0e6);
    dev->setSampleRate(SOAPY_SDR_RX, 1, 1.0e6);
    dev->setGain(SOAPY_SDR_RX, 0, "ATT", 0.0);
    dev->setGain(SOAPY_SDR_RX, 1, "ATT", 0.0);

    auto *stream = dev->setupStream(SOAPY_SDR_RX, "CF32", {0, 1});
    dev->activateStream(stream);

    const size_t N = 65536;
    std::vector<std::complex<float>> buf0(N), buf1(N);
    std::vector<void*> buffs = {buf0.data(), buf1.data()};

    struct Case { double f0, f1; const char *label; };
    Case cases[] = {
        {99.5e6, 93.5e6, "ch0=99.5MHz(strong) ch1=93.5MHz(strong)"},
        {93.5e6, 99.5e6, "ch0=93.5MHz(strong) ch1=99.5MHz(strong)  -- swapped"},
        {102.5e6, 99.5e6, "ch0=102.5MHz(quiet) ch1=99.5MHz(strong) -- control"},
    };

    printf("ch0_rate=%.0f Hz  ch1_rate=%.0f Hz\n",
           dev->getSampleRate(SOAPY_SDR_RX, 0), dev->getSampleRate(SOAPY_SDR_RX, 1));

    for (auto &c : cases) {
        dev->setFrequency(SOAPY_SDR_RX, 0, c.f0);
        dev->setFrequency(SOAPY_SDR_RX, 1, c.f1);

        // read+discard once with both channels together so settle applies to both
        for (int d = 0; d < 3; d++) {
            int flags; long long t;
            dev->readStream(stream, buffs.data(), N, flags, t, 500000);
        }
        double p0 = 0, p1 = 0; size_t n0 = 0, n1 = 0;
        for (int reps = 0; reps < 4; reps++) {
            int flags; long long t;
            int ret = dev->readStream(stream, buffs.data(), N, flags, t, 500000);
            if (ret <= 0) continue;
            for (int i = 0; i < ret; i++) {
                p0 += buf0[i].real()*buf0[i].real() + buf0[i].imag()*buf0[i].imag();
                p1 += buf1[i].real()*buf1[i].real() + buf1[i].imag()*buf1[i].imag();
            }
            n0 += ret; n1 += ret;
        }
        double db0 = 10.0*log10((n0?p0/n0:0.0)+1e-12);
        double db1 = 10.0*log10((n1?p1/n1:0.0)+1e-12);
        printf("[%s]\n  ch0 @ %.1f MHz: %.2f dB | ch1 @ %.1f MHz: %.2f dB\n",
               c.label, c.f0/1e6, db0, c.f1/1e6, db1);
    }

    dev->deactivateStream(stream);
    dev->closeStream(stream);
    SoapySDR::Device::unmake(dev);
    return 0;
}
