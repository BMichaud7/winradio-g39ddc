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

## Known hardware limitation: shared front-end window

The G39DDC has **one shared analog front-end downconverter** across both
RX channels (`FrontEndWindowWidth` ≈ 16 MHz, `FrontEndFrequencyStep` =
10 MHz on the unit this was tested against — read via `GetDeviceInfo`).
The vendor's convenience `SetFrequency(channel, freq)` call repositions
this shared front-end on every call, so when both channels are streaming
simultaneously, whichever channel calls `SetFrequency` *last* effectively
"steals" the shared front-end, leaving the other channel's reception
incorrect even though its own `SetFrequency` call reports success.

`SoapyG39DDC.cpp` currently calls the vendor's convenience function
per-channel and does **not** yet implement front-end-window-aware
tuning (i.e. using `SetFrontEndFrequency` + per-channel
`SetDDC1Frequency` offsets to keep both channels correctly tuned within
a shared window, or repositioning the window without clobbering the
other channel's offset when both targets still fit within it). This is a
real architectural fix, not yet implemented — true independent
dual-channel reception at frequencies far enough apart currently does
not work correctly. PRs welcome.

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
