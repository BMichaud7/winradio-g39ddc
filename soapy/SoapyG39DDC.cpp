// SoapySDR driver for the WiNRADiO G39DDC EXCELSIOR receiver family
// (verified against a G39DDCe over USB, vendor id 0x14E0 product id 0x1109).
//
// Wraps the vendor's libg39ddcapi.so (loaded at runtime via dlopen, since
// there's no devel package / .so symlink / pkg-config file shipped — only
// libg39ddcapi.so.<ver>) rather than talking to the kernel driver directly.
// The vendor API already handles all of the USB/PCIe protocol detail; this
// driver only needs to bridge its push-style streaming (one callback per
// hardware DMA buffer, fired from the vendor's own thread) into SoapySDR's
// pull-style readStream(), and map SoapySDR's tuning/gain/rate API onto the
// vendor calls validated against real hardware (see streamtest.c in the
// devpack's examples/c/ for the standalone version of this validation).
//
// Hardware facts this driver relies on (confirmed empirically, not just
// from docs, against a real G39DDCe):
//   - SetFrequency(handle, channel, freqHz) sets the front-end + both DDC
//     stages + demodulator offset for you; no manual DDC-offset math needed.
//   - DDC1 (the raw-IQ wideband tap, as opposed to DDC2/demod which are
//     narrowband/already-processed) exposes a fixed MENU of sample rates
//     (24 on the test unit, 25 kSPS-5 MSPS) via GetDDC1Count/GetDDCInfo --
//     you select one by index via SetDDC1(), you cannot set an arbitrary
//     rate.
//   - Each DDC1 mode's BitsPerSample is fixed by the hardware (16-bit above
//     ~1.6 MSPS, 32-bit below, on the test unit) -- not independently
//     selectable for RX streaming (only for DDC1 *playback*/TX, which this
//     driver does not implement). This driver converts to CF32 in the
//     callback so SoapySDR callers never see this wrinkle.
//   - SetAttenuator/GetDDCInfo are device-wide / index-only calls in the
//     vendor API -- they do NOT take a channel argument (unlike almost
//     everything else here), confirmed against the real header.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Logger.hpp>

#include <dlfcn.h>
#include <cstring>
#include <cstdint>
#include <complex>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <algorithm>
#include <stdexcept>

#include "g39ddcapi.h"

#define G39_VENDOR_LIB "libg39ddcapi.so"

// Bound the per-channel ring buffer so a stalled SoapySDR consumer can't
// grow memory without limit; old samples are dropped (with SOAPY_SDR_OVERFLOW
// reported on the next readStream) rather than blocking the vendor's
// realtime callback thread, which must never be made to wait.
static constexpr size_t RING_CAPACITY_SAMPLES = 1 << 20; // ~1M IQ samples/channel

struct G39ChannelState
{
    bool      streaming   = false;
    uint32_t  ddc1_index  = 0;
    uint32_t  sample_rate = 0;
    uint32_t  bandwidth   = 0;
    double    frequency_hz = 100e6;
    uint8_t   attenuator_step = 0; // 0..3 -> 0/6/12/18 dB

    std::mutex                          mtx;
    std::condition_variable             cv;
    std::deque<std::complex<float>>     ring;
    bool                                overflowed = false;
};

class SoapyG39DDC : public SoapySDR::Device
{
public:
    explicit SoapyG39DDC(const SoapySDR::Kwargs &args)
    {
        api_ = dlopen(G39_VENDOR_LIB, RTLD_NOW);
        if (!api_)
            throw std::runtime_error(std::string("dlopen ") + G39_VENDOR_LIB +
                                      " failed: " + dlerror());
        bindSymbols();

        std::string serial = args.count("serial") ? args.at("serial") : std::string();
        const char *open_arg = serial.empty() ? G39DDC_OPEN_FIRST : serial.c_str();
        hDevice_ = OpenDevice_(open_arg);
        if (hDevice_ < 0)
            throw std::runtime_error("G39DDC OpenDevice failed");

        G39DDC_DEVICE_INFO info;
        memset(&info, 0, sizeof(info));
        if (!GetDeviceInfo_(hDevice_, &info, sizeof(info)))
        {
            CloseDevice_(hDevice_);
            throw std::runtime_error("G39DDC GetDeviceInfo failed");
        }
        serial_       = std::string(info.SerialNumber);
        hwVersion_    = info.HWVersion;
        fwVersion_    = info.FWVersion;
        numChannels_  = info.ChannelCount;
        freqMinHz_    = static_cast<double>(info.FrontEndMinFrequency);
        freqMaxHz_    = static_cast<double>(info.FrontEndMaxFrequency);

        if (!SetPower_(hDevice_, 1))
        {
            CloseDevice_(hDevice_);
            throw std::runtime_error("G39DDC SetPower(1) failed");
        }
        powered_ = true;

        channels_.resize(numChannels_);
        ddc1Menu_.resize(numChannels_);
        for (uint32_t ch = 0; ch < numChannels_; ch++)
        {
            channels_[ch] = std::make_unique<G39ChannelState>();
            refreshDdc1Menu(ch);
        }

        G39DDC_CALLBACKS cb;
        memset(&cb, 0, sizeof(cb));
        cb.DDC1StreamCallback = &SoapyG39DDC::ddc1Trampoline;
        if (!SetCallbacks_(hDevice_, &cb, reinterpret_cast<uintptr_t>(this)))
        {
            SetPower_(hDevice_, 0);
            CloseDevice_(hDevice_);
            throw std::runtime_error("G39DDC SetCallbacks failed");
        }

        SoapySDR_logf(SOAPY_SDR_INFO,
                      "G39DDC opened: serial=%s hw=0x%04x fw=0x%04x channels=%u "
                      "freq=%.0f-%.0f Hz",
                      serial_.c_str(), hwVersion_, fwVersion_, numChannels_,
                      freqMinHz_, freqMaxHz_);
    }

    ~SoapyG39DDC() override
    {
        for (uint32_t ch = 0; ch < numChannels_; ch++)
        {
            if (channels_[ch]->streaming)
                StopDDC1_(hDevice_, ch);
        }
        SetCallbacks_(hDevice_, nullptr, 0);
        if (powered_) SetPower_(hDevice_, 0);
        CloseDevice_(hDevice_);
        dlclose(api_);
    }

    // ---- identification --------------------------------------------------
    std::string getDriverKey(void) const override { return "G39DDC"; }
    std::string getHardwareKey(void) const override { return "WiNRADiO G39DDC"; }

    SoapySDR::Kwargs getHardwareInfo(void) const override
    {
        SoapySDR::Kwargs args;
        args["serial"]   = serial_;
        args["hwVersion"] = std::to_string(hwVersion_);
        args["fwVersion"] = std::to_string(fwVersion_);
        return args;
    }

    size_t getNumChannels(const int direction) const override
    {
        return (direction == SOAPY_SDR_RX) ? numChannels_ : 0;
    }

    // ---- frequency ---------------------------------------------------------
    void setFrequency(const int direction, const size_t channel,
                       const double frequency, const SoapySDR::Kwargs &) override
    {
        if (direction != SOAPY_SDR_RX) return;
        checkChannel(channel);
        if (!SetFrequency_(hDevice_, static_cast<uint32_t>(channel),
                            static_cast<uint64_t>(frequency)))
            throw std::runtime_error("G39DDC SetFrequency failed");
        channels_[channel]->frequency_hz = frequency;
        // Retuning doesn't stop the stream, so the vendor callback keeps
        // pushing into the ring buffer the whole time the retune is in
        // flight -- without clearing it here, readStream() can keep
        // returning pre-retune samples for a while after setFrequency()
        // returns, especially under continuous dual-channel streaming
        // where the backlog can be large. Confirmed via a two-channel
        // different-frequency test: channel 1's reported power kept
        // reflecting its OLD frequency after a second retune until this
        // flush was added.
        {
            auto &st = *channels_[channel];
            std::lock_guard<std::mutex> lk(st.mtx);
            st.ring.clear();
        }
    }

    double getFrequency(const int direction, const size_t channel) const override
    {
        if (direction != SOAPY_SDR_RX) return 0.0;
        checkChannel(channel);
        uint64_t freq = 0;
        if (GetFrequency_(hDevice_, static_cast<uint32_t>(channel), &freq))
            return static_cast<double>(freq);
        return channels_[channel]->frequency_hz;
    }

    SoapySDR::RangeList getFrequencyRange(const int direction, const size_t) const override
    {
        SoapySDR::RangeList r;
        if (direction == SOAPY_SDR_RX) r.push_back(SoapySDR::Range(freqMinHz_, freqMaxHz_));
        return r;
    }

    // ---- sample rate (DDC1 mode selection) ----------------------------------
    void setSampleRate(const int direction, const size_t channel, const double rate) override
    {
        if (direction != SOAPY_SDR_RX) return;
        checkChannel(channel);
        refreshDdc1Menu(channel); // menu is queried fresh in case it's channel-specific

        const auto &menu = ddc1Menu_[channel];
        if (menu.empty())
            throw std::runtime_error("G39DDC: no DDC1 modes available for this channel");

        size_t best = 0;
        double bestDelta = std::abs(static_cast<double>(menu[0].SampleRate) - rate);
        for (size_t i = 1; i < menu.size(); i++)
        {
            double delta = std::abs(static_cast<double>(menu[i].SampleRate) - rate);
            if (delta < bestDelta) { bestDelta = delta; best = i; }
        }

        bool was_streaming = channels_[channel]->streaming;
        if (was_streaming) stopChannel(channel);

        if (!SetDDC1_(hDevice_, static_cast<uint32_t>(channel),
                       static_cast<uint32_t>(best)))
            throw std::runtime_error("G39DDC SetDDC1 failed");

        channels_[channel]->ddc1_index  = static_cast<uint32_t>(best);
        channels_[channel]->sample_rate = menu[best].SampleRate;
        channels_[channel]->bandwidth   = menu[best].Bandwidth;

        SoapySDR_logf(SOAPY_SDR_DEBUG,
                      "G39DDC ch%zu: selected DDC1[%zu] rate=%u bw=%u bits=%u (requested %.0f)",
                      channel, best, menu[best].SampleRate, menu[best].Bandwidth,
                      menu[best].BitsPerSample, rate);

        if (was_streaming) startChannel(channel);
    }

    double getSampleRate(const int direction, const size_t channel) const override
    {
        if (direction != SOAPY_SDR_RX) return 0.0;
        checkChannel(channel);
        return channels_[channel]->sample_rate;
    }

    std::vector<double> listSampleRates(const int direction, const size_t channel) const override
    {
        std::vector<double> rates;
        if (direction != SOAPY_SDR_RX) return rates;
        checkChannel(channel);
        const_cast<SoapyG39DDC *>(this)->refreshDdc1Menu(channel);
        for (const auto &m : ddc1Menu_[channel]) rates.push_back(m.SampleRate);
        return rates;
    }

    SoapySDR::RangeList getSampleRateRange(const int direction, const size_t channel) const override
    {
        SoapySDR::RangeList r;
        if (direction != SOAPY_SDR_RX) return r;
        checkChannel(channel);
        const_cast<SoapyG39DDC *>(this)->refreshDdc1Menu(channel);
        for (const auto &m : ddc1Menu_[channel])
            r.push_back(SoapySDR::Range(m.SampleRate, m.SampleRate));
        return r;
    }

    // Bandwidth is implied by the selected DDC1 mode on this hardware (RX
    // raw-IQ tap has no independently adjustable filter) -- report it,
    // don't pretend it's separately settable.
    double getBandwidth(const int direction, const size_t channel) const override
    {
        if (direction != SOAPY_SDR_RX) return 0.0;
        checkChannel(channel);
        return channels_[channel]->bandwidth;
    }

    std::vector<double> listBandwidths(const int direction, const size_t channel) const override
    {
        std::vector<double> bws;
        if (direction != SOAPY_SDR_RX) return bws;
        checkChannel(channel);
        const_cast<SoapyG39DDC *>(this)->refreshDdc1Menu(channel);
        for (const auto &m : ddc1Menu_[channel]) bws.push_back(m.Bandwidth);
        return bws;
    }

    // ---- gain (front-end step attenuator, 0/6/12/18 dB) ---------------------
    std::vector<std::string> listGains(const int direction, const size_t) const override
    {
        std::vector<std::string> g;
        if (direction == SOAPY_SDR_RX) g.push_back("ATT");
        return g;
    }

    SoapySDR::Range getGainRange(const int direction, const size_t, const std::string &) const override
    {
        return (direction == SOAPY_SDR_RX) ? SoapySDR::Range(0.0, 18.0, 6.0) : SoapySDR::Range(0, 0);
    }

    void setGain(const int direction, const size_t channel, const std::string &name,
                 const double value) override
    {
        if (direction != SOAPY_SDR_RX || name != "ATT") return;
        checkChannel(channel);
        double clamped = std::min(18.0, std::max(0.0, value));
        int step = static_cast<int>(std::round(clamped / 6.0));
        if (!SetAttenuator_(hDevice_, static_cast<uint32_t>(step)))
            throw std::runtime_error("G39DDC SetAttenuator failed");
        channels_[channel]->attenuator_step = static_cast<uint8_t>(step);
    }

    double getGain(const int direction, const size_t channel, const std::string &name) const override
    {
        if (direction != SOAPY_SDR_RX || name != "ATT") return 0.0;
        checkChannel(channel);
        return channels_[channel]->attenuator_step * 6.0;
    }

    // ---- streaming -----------------------------------------------------------
    std::vector<std::string> getStreamFormats(const int direction, const size_t) const override
    {
        std::vector<std::string> f;
        if (direction == SOAPY_SDR_RX) f.push_back("CF32");
        return f;
    }

    std::string getNativeStreamFormat(const int direction, const size_t, double &fullScale) const override
    {
        fullScale = 1.0;
        return (direction == SOAPY_SDR_RX) ? "CF32" : "";
    }

    SoapySDR::Stream *setupStream(const int direction, const std::string &format,
                                   const std::vector<size_t> &channels,
                                   const SoapySDR::Kwargs &) override
    {
        if (direction != SOAPY_SDR_RX)
            throw std::runtime_error("G39DDC: TX not supported");
        if (format != "CF32")
            throw std::runtime_error("G39DDC: only CF32 is supported, got " + format);

        auto chans = channels.empty() ? std::vector<size_t>{0} : channels;
        for (size_t ch : chans) checkChannel(ch);

        auto *handle = new std::vector<size_t>(chans);
        return reinterpret_cast<SoapySDR::Stream *>(handle);
    }

    void closeStream(SoapySDR::Stream *stream) override
    {
        auto *chans = reinterpret_cast<std::vector<size_t> *>(stream);
        for (size_t ch : *chans) stopChannel(ch);
        delete chans;
    }

    int activateStream(SoapySDR::Stream *stream, const int, const long long,
                        const size_t) override
    {
        auto *chans = reinterpret_cast<std::vector<size_t> *>(stream);
        for (size_t ch : *chans) startChannel(ch);
        return 0;
    }

    int deactivateStream(SoapySDR::Stream *stream, const int, const long long) override
    {
        auto *chans = reinterpret_cast<std::vector<size_t> *>(stream);
        for (size_t ch : *chans) stopChannel(ch);
        return 0;
    }

    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t numElems,
                    int &flags, long long &, const long timeoutUs) override
    {
        auto *chans = reinterpret_cast<std::vector<size_t> *>(stream);
        if (chans->empty()) return SOAPY_SDR_NOT_SUPPORTED;
        // Single-channel-per-readStream-call is all client code below
        // actually needs for this driver's intended use (recon/acquisition
        // pulls one channel at a time); for >1 requested channel, every
        // channel is drained the same way using its own buffer slot.
        flags = 0;
        size_t got = 0;
        for (size_t i = 0; i < chans->size(); i++)
        {
            size_t ch = (*chans)[i];
            auto &st = *channels_[ch];
            std::unique_lock<std::mutex> lk(st.mtx);
            if (st.ring.empty())
            {
                st.cv.wait_for(lk, std::chrono::microseconds(timeoutUs),
                               [&] { return !st.ring.empty(); });
            }
            if (st.ring.empty())
            {
                return chans->size() == 1 ? SOAPY_SDR_TIMEOUT : static_cast<int>(got);
            }
            if (st.overflowed)
            {
                flags |= SOAPY_SDR_OVERFLOW;
                st.overflowed = false;
            }
            size_t n = std::min(numElems, st.ring.size());
            auto *out = static_cast<std::complex<float> *>(buffs[i]);
            for (size_t s = 0; s < n; s++)
            {
                out[s] = st.ring.front();
                st.ring.pop_front();
            }
            got = n;
        }
        return static_cast<int>(got);
    }

private:
    void checkChannel(size_t channel) const
    {
        if (channel >= numChannels_)
            throw std::runtime_error("G39DDC: invalid channel " + std::to_string(channel));
    }

    void refreshDdc1Menu(size_t channel)
    {
        uint32_t count = 0;
        if (!GetDDC1Count_(hDevice_, static_cast<uint32_t>(channel), &count) || count == 0)
        {
            ddc1Menu_[channel].clear();
            return;
        }
        std::vector<G39DDC_DDC_INFO> menu(count);
        for (uint32_t i = 0; i < count; i++)
        {
            memset(&menu[i], 0, sizeof(menu[i]));
            GetDDCInfo_(i, &menu[i]);
        }
        ddc1Menu_[channel] = std::move(menu);
    }

    void startChannel(size_t channel)
    {
        auto &st = *channels_[channel];
        if (st.streaming) return;
        constexpr uint32_t SAMPLES_PER_BUFFER = 4096;
        if (!StartDDC1_(hDevice_, static_cast<uint32_t>(channel), SAMPLES_PER_BUFFER))
            throw std::runtime_error("G39DDC StartDDC1 failed");
        st.streaming = true;
    }

    void stopChannel(size_t channel)
    {
        auto &st = *channels_[channel];
        if (!st.streaming) return;
        StopDDC1_(hDevice_, static_cast<uint32_t>(channel));
        st.streaming = false;
        std::lock_guard<std::mutex> lk(st.mtx);
        st.ring.clear();
    }

    // Called on the vendor's own realtime thread -- must never block.
    void onDdc1Data(uint32_t channel, const void *buffer, uint32_t numSamples,
                     uint32_t bitsPerSample)
    {
        if (channel >= numChannels_) return;
        auto &st = *channels_[channel];

        std::lock_guard<std::mutex> lk(st.mtx);
        if (bitsPerSample == 16)
        {
            const int16_t *p = static_cast<const int16_t *>(buffer);
            for (uint32_t i = 0; i < numSamples; i++)
                pushSample(st, p[2 * i] / 32768.0f, p[2 * i + 1] / 32768.0f);
        }
        else if (bitsPerSample == 32)
        {
            const int32_t *p = static_cast<const int32_t *>(buffer);
            for (uint32_t i = 0; i < numSamples; i++)
                pushSample(st, p[2 * i] / 2147483648.0f, p[2 * i + 1] / 2147483648.0f);
        }
        st.cv.notify_one();
    }

    static void pushSample(G39ChannelState &st, float i, float q)
    {
        if (st.ring.size() >= RING_CAPACITY_SAMPLES)
        {
            st.ring.pop_front();
            st.overflowed = true;
        }
        st.ring.emplace_back(i, q);
    }

    static void ddc1Trampoline(uint32_t Channel, const void *Buffer, uint32_t NumberOfSamples,
                                uint32_t BitsPerSample, uintptr_t UserData)
    {
        reinterpret_cast<SoapyG39DDC *>(UserData)->onDdc1Data(Channel, Buffer, NumberOfSamples,
                                                                BitsPerSample);
    }

    void bindSymbols()
    {
        OpenDevice_     = reinterpret_cast<G39DDC_OPEN_DEVICE>(dlsym(api_, "OpenDevice"));
        CloseDevice_    = reinterpret_cast<G39DDC_CLOSE_DEVICE>(dlsym(api_, "CloseDevice"));
        GetDeviceInfo_  = reinterpret_cast<G39DDC_GET_DEVICE_INFO>(dlsym(api_, "GetDeviceInfo"));
        SetPower_       = reinterpret_cast<G39DDC_SET_POWER>(dlsym(api_, "SetPower"));
        SetFrequency_   = reinterpret_cast<G39DDC_SET_FREQUENCY>(dlsym(api_, "SetFrequency"));
        GetFrequency_   = reinterpret_cast<G39DDC_GET_FREQUENCY>(dlsym(api_, "GetFrequency"));
        GetDDC1Count_   = reinterpret_cast<G39DDC_GET_DDC1_COUNT>(dlsym(api_, "GetDDC1Count"));
        GetDDCInfo_     = reinterpret_cast<G39DDC_GET_DDC_INFO>(dlsym(api_, "GetDDCInfo"));
        SetDDC1_        = reinterpret_cast<G39DDC_SET_DDC1>(dlsym(api_, "SetDDC1"));
        SetCallbacks_   = reinterpret_cast<G39DDC_SET_CALLBACKS>(dlsym(api_, "SetCallbacks"));
        StartDDC1_      = reinterpret_cast<G39DDC_START_DDC1>(dlsym(api_, "StartDDC1"));
        StopDDC1_       = reinterpret_cast<G39DDC_STOP_DDC1>(dlsym(api_, "StopDDC1"));
        SetAttenuator_  = reinterpret_cast<G39DDC_SET_ATTENUATOR>(dlsym(api_, "SetAttenuator"));

        if (!OpenDevice_ || !CloseDevice_ || !GetDeviceInfo_ || !SetPower_ ||
            !SetFrequency_ || !GetFrequency_ || !GetDDC1Count_ || !GetDDCInfo_ ||
            !SetDDC1_ || !SetCallbacks_ || !StartDDC1_ || !StopDDC1_ || !SetAttenuator_)
            throw std::runtime_error("G39DDC: missing one or more required symbols in " G39_VENDOR_LIB);
    }

    void *api_ = nullptr;
    G39DDC_OPEN_DEVICE     OpenDevice_     = nullptr;
    G39DDC_CLOSE_DEVICE    CloseDevice_    = nullptr;
    G39DDC_GET_DEVICE_INFO GetDeviceInfo_  = nullptr;
    G39DDC_SET_POWER       SetPower_       = nullptr;
    G39DDC_SET_FREQUENCY   SetFrequency_   = nullptr;
    G39DDC_GET_FREQUENCY   GetFrequency_   = nullptr;
    G39DDC_GET_DDC1_COUNT  GetDDC1Count_   = nullptr;
    G39DDC_GET_DDC_INFO    GetDDCInfo_     = nullptr;
    G39DDC_SET_DDC1        SetDDC1_        = nullptr;
    G39DDC_SET_CALLBACKS   SetCallbacks_   = nullptr;
    G39DDC_START_DDC1      StartDDC1_      = nullptr;
    G39DDC_STOP_DDC1       StopDDC1_       = nullptr;
    G39DDC_SET_ATTENUATOR  SetAttenuator_  = nullptr;

    int32_t  hDevice_     = -1;
    bool     powered_     = false;
    std::string serial_;
    uint16_t hwVersion_   = 0;
    uint16_t fwVersion_   = 0;
    uint32_t numChannels_ = 0;
    double   freqMinHz_   = 0.0;
    double   freqMaxHz_   = 0.0;

    std::vector<std::unique_ptr<G39ChannelState>> channels_;
    std::vector<std::vector<G39DDC_DDC_INFO>> ddc1Menu_;
};

// ---- plugin registration --------------------------------------------------

static SoapySDR::KwargsList findG39DDC(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList results;

    void *api = dlopen(G39_VENDOR_LIB, RTLD_NOW);
    if (!api) return results; // vendor lib not installed -- nothing to enumerate

    auto OpenDevice    = reinterpret_cast<G39DDC_OPEN_DEVICE>(dlsym(api, "OpenDevice"));
    auto CloseDevice   = reinterpret_cast<G39DDC_CLOSE_DEVICE>(dlsym(api, "CloseDevice"));
    auto GetDeviceInfo = reinterpret_cast<G39DDC_GET_DEVICE_INFO>(dlsym(api, "GetDeviceInfo"));
    if (!OpenDevice || !CloseDevice || !GetDeviceInfo) { dlclose(api); return results; }

    int32_t h = OpenDevice(G39DDC_OPEN_FIRST);
    if (h >= 0)
    {
        G39DDC_DEVICE_INFO info;
        memset(&info, 0, sizeof(info));
        if (GetDeviceInfo(h, &info, sizeof(info)))
        {
            SoapySDR::Kwargs dev;
            dev["driver"] = "g39ddc";
            dev["label"]  = std::string("WiNRADiO G39DDC [") + info.SerialNumber + "]";
            dev["serial"] = info.SerialNumber;
            if (!args.count("serial") || args.at("serial") == info.SerialNumber)
                results.push_back(dev);
        }
        CloseDevice(h);
    }
    dlclose(api);
    return results;
}

static SoapySDR::Device *makeG39DDC(const SoapySDR::Kwargs &args)
{
    return new SoapyG39DDC(args);
}

static SoapySDR::Registry registerG39DDC("g39ddc", &findG39DDC, &makeG39DDC, SOAPY_SDR_ABI_VERSION);
