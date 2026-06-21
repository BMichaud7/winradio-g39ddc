# WiNRADiO G39DDC — Linux kernel driver patches + SoapySDR driver

A SoapySDR driver and modern-kernel compatibility patches for the
[WiNRADiO G39DDC EXCELSIOR](https://www.winradio.com/) receiver family
(tested against a real G39DDCe, USB vendor:product `14e0:1109`).

This repo contains **only original code I wrote**. It does **not** bundle
WiNRADiO/RADIXON's proprietary DevPack (kernel driver source, C/C++ API
header, prebuilt `libg39ddcapi.so`) — that SDK is vendor-copyrighted with
no redistribution license, so you need to get it yourself from WiNRADiO
and apply the patches in `patches/` on top of it.

## What's here

- `patches/` — unified diffs against WiNRADiO's `g39ddc-1.11` DevPack
  driver source, updating it to build against modern (6.x) Linux kernels.
  Fixes: `access_ok()` 3-arg→2-arg signature, `.ioctl`→`.unlocked_ioctl`/
  `.compat_ioctl`, `from_timer()`→`container_of()`, `struct timespec` /
  `current_kernel_time()` / `timespec_compare()` →`timespec64` equivalents,
  and disables the unused/unportable PCI code path (this hardware is USB).
- `soapy/SoapyG39DDC.cpp` — a `SoapySDR::Device` driver wrapping the
  vendor's `libg39ddcapi.so` via `dlopen`. Supports both RX channels,
  per-channel sample rate (DDC1 mode selection), frequency, and step
  attenuator gain. Builds as a standalone SoapySDR plugin module.
- `udev/99-g39ddc.rules` — lets a normal user open `/dev/g39ddc*` and the
  raw USB device without root.
- `test/` — standalone validation programs used to confirm this actually
  works against real hardware (not just compiles):
  - `smoketest.c` / `streamtest.c` — minimal C programs against the
    vendor API directly (device info, then live DDC1 IQ streaming).
  - `fm_scan_test.cpp` — sweeps the FM broadcast band through the
    SoapySDR driver and prints per-frequency power, to prove real RF
    content is flowing end-to-end (not just non-zero sample throughput).
  - `dual_channel_independent_test.cpp` — tunes both RX channels to
    different frequencies/sample rates simultaneously and measures each
    independently.
  - `calibrate_ddc1freq.c` — empirically determines the
    `SetFrontEndFrequency`/`SetDDC1Frequency` offset convention by parking
    the front end and sweeping the DDC1 offset against known real signal
    peaks.

## Shared front-end window (handled)

The G39DDC has **one shared analog front-end downconverter** across both
RX channels (`FrontEndWindowWidth` ≈ 16 MHz, `FrontEndFrequencyStep` =
10 MHz on the unit this was tested against — read via `GetDeviceInfo`).
The vendor's convenience `SetFrequency(channel, freq)` call repositions
this shared front-end on every call, so naively calling it per-channel
lets whichever channel retunes *last* silently steal the shared front-end
out from under the other channel, even though that channel's own
`SetFrequency` call still reports success.

`SoapyG39DDC.cpp` tracks the front-end's actual position itself and only
moves it via `SetFrontEndFrequency` when a requested frequency genuinely
doesn't fit the current window; within the window it tunes via the
per-channel `SetDDC1Frequency` offset instead, which doesn't disturb the
other channel at all (`test/calibrate_ddc1freq.c` is the standalone
calibration program that empirically confirmed the offset convention:
`actual_freq = front_end_hz + ddc1_offset`). Verified on real hardware
with `test/dual_channel_independent_test.cpp`: both channels tuned to
different frequencies and sample rates, streaming simultaneously, each
independently showing the correct signal power at its own frequency.

If the two channels' desired frequencies are farther apart than the
window width, only one can be served correctly at a time — that's a
genuine hardware limit, not something software can work around; the
driver repositions the front end for the requesting channel and logs a
warning that the other channel is now degraded, same as the vendor API
would behave.

## Building

```sh
# 1. Get the WiNRADiO G39DDC Linux DevPack from WiNRADiO/RADIXON yourself.
# 2. Apply the patches:
cd g39ddc-1.11
patch -p1 < /path/to/this/repo/patches/g39ddc-c.patch
patch -p1 < /path/to/this/repo/patches/g39ddc-h.patch
patch -p1 < /path/to/this/repo/patches/g39ddc-makefile.patch
cd driver && make && sudo make install

# 3. Install the udev rule:
sudo cp udev/99-g39ddc.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger -s g39ddc

# 4. Build and install the SoapySDR module (needs the vendor's
#    g39ddcapi.h and libg39ddcapi.so on your include/library path):
cd soapy
make VENDOR_HDR=/path/to/devpack/c_cpp_header VENDOR_LIB=/path/to/devpack/lib/x86_64
sudo make install

# 5. Verify:
SoapySDRUtil --find
SoapySDRUtil --probe="driver=g39ddc"
```

## License

The code in this repo (everything except what you provide yourself from
WiNRADiO's DevPack) is MIT licensed — see `LICENSE`.
