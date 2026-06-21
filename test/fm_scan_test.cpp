// Sweeps the FM broadcast band through the SoapySDR g39ddc driver and
// prints average power per frequency. Real broadcast stations should stand
// out clearly above the receiver noise floor -- this is a much stronger
// "is it actually working" check than raw sample throughput, since it
// proves the full tune -> stream -> CF32-convert path is carrying real RF
// content end to end, not just non-zero numbers.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <cstdio>
#include <complex>
#include <vector>
#include <cmath>

int main()
{
    SoapySDR::Kwargs args;
    args["driver"] = "g39ddc";
    SoapySDR::Device *dev = SoapySDR::Device::make(args);

    const double rate = 2.0e6;
    dev->setSampleRate(SOAPY_SDR_RX, 0, rate);
    dev->setGain(SOAPY_SDR_RX, 0, "ATT", 0.0);

    auto *stream = dev->setupStream(SOAPY_SDR_RX, "CF32", {0});
    dev->activateStream(stream);

    const size_t N = 65536;
    std::vector<std::complex<float>> buf(N);
    void *buffs[] = {buf.data()};

    printf("freq_MHz  avg_power_dB\n");
    for (double f = 88.0e6; f <= 108.0e6; f += 0.5e6) {
        dev->setFrequency(SOAPY_SDR_RX, 0, f);

        // discard a couple buffers while the front end / DDC settle after retune
        for (int discard = 0; discard < 3; discard++) {
            int flags; long long timeNs;
            dev->readStream(stream, buffs, N, flags, timeNs, 500000);
        }

        double power_sum = 0.0;
        size_t total = 0;
        for (int reps = 0; reps < 4; reps++) {
            int flags; long long timeNs;
            int ret = dev->readStream(stream, buffs, N, flags, timeNs, 500000);
            if (ret <= 0) continue;
            for (int i = 0; i < ret; i++) {
                float re = buf[i].real(), im = buf[i].imag();
                power_sum += re * re + im * im;
            }
            total += ret;
        }
        double avg_power = total ? power_sum / total : 0.0;
        double db = 10.0 * log10(avg_power + 1e-12);
        printf("%8.1f  %8.2f\n", f / 1e6, db);
    }

    dev->deactivateStream(stream);
    dev->closeStream(stream);
    SoapySDR::Device::unmake(dev);
    return 0;
}
