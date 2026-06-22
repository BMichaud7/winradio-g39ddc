# WiNRADiO G39DDC — Linux kernel driver patches + SoapySDR driver

[![CI](https://github.com/BMichaud7/winradio-g39ddc/actions/workflows/ci.yml/badge.svg)](https://github.com/BMichaud7/winradio-g39ddc/actions/workflows/ci.yml)
[![Latest release](https://img.shields.io/github/v/release/BMichaud7/winradio-g39ddc)](https://github.com/BMichaud7/winradio-g39ddc/releases/latest)

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
  per-channel sample rate (DDC1 mode selection), frequency, and
  band-aware gain (step attenuator below 50MHz, switched preamp above).
  Builds as a standalone SoapySDR plugin module.
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
  - `calibrate_gain.c` — proves the per-channel `SetGain`/`SetAGC` API has
    zero effect on DDC1's raw IQ output (it only affects the demodulator/
    audio chain downstream of the IQ tap point).
  - `calibrate_preamp.c` — proves `SetPreamplifier` (device-wide, ~10dB)
    is the real VHF/UHF/SHF gain control, with a measured ~11dB power
    swing at 99.5MHz.
  - `gain_band_test.cpp` — exercises the SoapySDR driver's `setGain("ATT")`
    end-to-end at both an HF and a VHF frequency, confirming the
    band-aware dispatch below.

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

## Gain control: two different controls in two different signal paths (handled)

The vendor SDK's block diagram (per-channel chain: frequency shift → DDC1
→ frequency shift → DDC2 → noise blanker → demod filter → notch → AGC/
gain → demodulator → audio gain → audio filter) shows the per-channel
`SetGain`/`SetAGC` API sits *after* DDC2, in the demodulator/audio chain
only — it never touches DDC1's raw IQ output, which is the data this
driver actually streams. Confirmed empirically in `test/calibrate_gain.c`:
sweeping `SetGain` 0-60dB at a known strong VHF station produced no
measurable change in IQ power.

The two controls that *do* affect DDC1's raw output are both device-wide
(not per-channel) and live in different parts of the analog front-end
depending on band:

- `SetAttenuator` (0-18dB, 6dB steps) — wired into the **HF path only**
  (9kHz-50MHz on the tested unit).
- `SetPreamplifier` (switched ~10dB boolean) — wired into the
  **VHF/UHF/SHF path only** (50MHz-3.5GHz).

`SoapyG39DDC.cpp` exposes a single `"ATT"` SoapySDR gain element per
channel that dispatches to whichever control is actually wired into that
channel's current frequency, using a 50MHz HF/VHF boundary check against
the channel's last-tuned frequency:`getGainRange`, `setGain`, and
`getGain` all do this check. Below 50MHz it drives `SetAttenuator`
(0-18dB); at or above it drives `SetPreamplifier` (treating any
requested gain ≥5dB as "preamp on"). Verified on real hardware with
`test/gain_band_test.cpp`: ~11dB measured swing toggling `ATT` 0/10 at
99.5MHz (preamp path), and a real ~10dB monotonic drop sweeping `ATT`
0/6/12/18 at 7.2MHz (attenuator path) — neither was visible before this
fix, since `SetAttenuator` alone (without the preamp dispatch) showed
~0dB change at any VHF test frequency.

Because both underlying controls are device-wide, setting gain on one
channel also affects the other if they're both in the same band — same
as the vendor API, not a limitation introduced by this driver.

## Building

### Quick path: `install.sh`

If you already have the WiNRADiO G39DDC DevPack downloaded and extracted
somewhere (get it from WiNRADiO/RADIXON yourself — see licensing note
above), one command does everything below by hand:

```sh
sudo ./install.sh --sdk-dir /path/to/g39ddc-1.11
```

It applies the patches, builds+installs the kernel module, installs the
udev rule, and builds+installs the SoapySDR module, then runs
`SoapySDRUtil --find` to verify. Pass `--skip-kernel-module` if the
driver's already installed, or `--module-dir` to override the
auto-detected SoapySDR plugin path. `./install.sh --help` for details.

### RPM: one command, does the vendor download for you too

Every tagged release publishes prebuilt `x86_64` and `aarch64` RPMs as
real GitHub Release assets (permanent, public, no GitHub login needed —
not CI workflow artifacts, which require auth and expire after 90 days).
Grab the one matching `uname -m` from the
[latest release](https://github.com/BMichaud7/winradio-g39ddc/releases/latest)
and:

```sh
sudo dnf install ./g39ddc-soapy-1.0.0-1.el9.x86_64.rpm   # or the aarch64 one
```

That's the whole install. The RPM's `%post` script fetches the vendor
DevPack itself (from WiNRADiO's own public URL — never bundled in this
repo or the RPM) and builds+installs the kernel driver against whatever
kernel is running on the install target, in addition to the prebuilt
SoapySDR module and udev rule it ships directly.

This needs network access and kernel headers matching the running
kernel at install time; `%post` checks for the latter and fails with a
clear message (rather than silently skipping the driver) if they're
missing. It's a one-shot build against the kernel at install time, not
DKMS — a kernel upgrade later needs `rpm --reinstall` to rebuild. To
build the RPM yourself instead of using a release, see the comment at
the top of `rpm/g39ddc-soapy.spec`.

### By hand

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
#    g39ddcapi.h on your include path -- the vendor lib is dlopen()'d
#    at runtime, not linked, so it isn't needed at build time):
cd soapy
make VENDOR_HDR=/path/to/devpack/c_cpp_header
sudo make install

# 5. Verify:
SoapySDRUtil --find
SoapySDRUtil --probe="driver=g39ddc"
```

## CI

`.github/workflows/ci.yml` runs on every push/PR:

- **build-test**: fetches the vendor DevPack fresh (same as the RPM
  `%post` and install.sh do, from WiNRADiO's public URL — never
  cached/committed in this repo beyond a build-time GitHub Actions
  cache), regression-tests that `patches/` still apply and the kernel
  module still builds, builds the SoapySDR module, compile-checks every
  program in `test/` (no real hardware on CI runners, so they're built
  but not run), and loads the built module to confirm it registers the
  `g39ddc` SoapySDR factory without crashing.
- **rpm**: builds `rpm/g39ddc-soapy.spec` in a Rocky Linux 9 container
  (matching this project's actual deployment base image), as a matrix
  over `x86_64` (`ubuntu-latest`) and `aarch64` (`ubuntu-24.04-arm`,
  GitHub's native ARM64 hosted runner), and uploads each `.rpm` as a
  workflow artifact.
- **release** (tagged pushes only, e.g. `v1.0.0`): takes both RPMs from
  the matrix and publishes them as real GitHub Release assets — see
  the RPM section above.

## License

The code in this repo (everything except what you provide yourself from
WiNRADiO's DevPack) is MIT licensed — see `LICENSE`.
