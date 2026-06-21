#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <complex>
#include <vector>
#include <cstdio>
#include <cmath>

static double measurePower(SoapySDR::Device *dev, SoapySDR::Stream *stream)
{
    std::vector<std::complex<float>> buf(4096);
    void *buffs[] = {buf.data()};
    // drain a bit first to clear stale samples after a retune
    for (int i = 0; i < 5; i++) {
        int flags; long long timeNs;
        dev->readStream(stream, buffs, buf.size(), flags, timeNs, 200000);
    }
    double acc = 0; long n = 0;
    for (int i = 0; i < 20; i++) {
        int flags; long long timeNs;
        int ret = dev->readStream(stream, buffs, buf.size(), flags, timeNs, 200000);
        if (ret <= 0) continue;
        for (int s = 0; s < ret; s++) {
            double re = buf[s].real(), im = buf[s].imag();
            acc += re*re + im*im;
            n++;
        }
    }
    return n ? 10.0 * std::log10(acc / n + 1e-12) : -999.0;
}

int main()
{
    SoapySDR::Kwargs args;
    args["driver"] = "g39ddc";
    SoapySDR::Device *dev = SoapySDR::Device::make(args);

    dev->setFrequency(SOAPY_SDR_RX, 0, 99.5e6);
    dev->setSampleRate(SOAPY_SDR_RX, 0, 2e6);
    dev->setBandwidth(SOAPY_SDR_RX, 0, 2e6);
    SoapySDR::Stream *stream = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    dev->activateStream(stream);

    printf("=== VHF 99.5MHz (preamp path) ===\n");
    for (double g : {0.0, 10.0}) {
        dev->setGain(SOAPY_SDR_RX, 0, "ATT", g);
        double readback = dev->getGain(SOAPY_SDR_RX, 0, "ATT");
        double p = measurePower(dev, stream);
        printf("  set=%.0f readback=%.0f measured=%.2f dB\n", g, readback, p);
    }

    dev->deactivateStream(stream);
    dev->closeStream(stream);

    // Now retune to an HF frequency to exercise the attenuator path.
    dev->setFrequency(SOAPY_SDR_RX, 0, 7.2e6); // 41m broadcast band, likely active
    stream = dev->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32);
    dev->activateStream(stream);

    printf("=== HF 7.2MHz (attenuator path) ===\n");
    SoapySDR::Range range = dev->getGainRange(SOAPY_SDR_RX, 0, "ATT");
    printf("  ATT range: %.1f to %.1f step %.1f\n", range.minimum(), range.maximum(), range.step());
    for (double g : {0.0, 6.0, 12.0, 18.0}) {
        dev->setGain(SOAPY_SDR_RX, 0, "ATT", g);
        double readback = dev->getGain(SOAPY_SDR_RX, 0, "ATT");
        double p = measurePower(dev, stream);
        printf("  set=%.0f readback=%.0f measured=%.2f dB\n", g, readback, p);
    }

    dev->deactivateStream(stream);
    dev->closeStream(stream);
    SoapySDR::Device::unmake(dev);
    return 0;
}
