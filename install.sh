#!/usr/bin/env bash
# Builds and installs the WiNRADiO G39DDC kernel driver (patched for modern
# kernels), udev rule, and SoapySDR plugin module, given a copy of the
# WiNRADiO/RADIXON g39ddc-1.11 DevPack that you've already downloaded
# yourself -- this script cannot fetch it for you (no redistribution
# license; see README.md).
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: sudo ./install.sh --sdk-dir /path/to/g39ddc-1.11 [options]

Options:
  --sdk-dir DIR          Path to the extracted WiNRADiO g39ddc-1.11 DevPack
                         (must contain driver/ and c_cpp_header/). Required.
  --skip-kernel-module   Only build/install the SoapySDR module + udev rule;
                         skip the kernel driver (e.g. it's already installed,
                         or you're building this on a different machine than
                         the one with the hardware attached).
  --module-dir DIR       Override the SoapySDR plugin install directory
                         (default: auto-detected /usr/local/.../modules0.8
                         path from `SoapySDRUtil --info`).
  -h, --help             Show this help.
EOF
}

sdk_dir=""
skip_kernel=0
module_dir=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-dir) sdk_dir="$2"; shift 2 ;;
        --skip-kernel-module) skip_kernel=1; shift ;;
        --module-dir) module_dir="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

if [[ -z "$sdk_dir" ]]; then
    echo "error: --sdk-dir is required" >&2
    usage
    exit 1
fi
sdk_dir=$(readlink -f "$sdk_dir")
repo_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

for sub in driver c_cpp_header; do
    if [[ ! -d "$sdk_dir/$sub" ]]; then
        echo "error: $sdk_dir/$sub not found -- doesn't look like a g39ddc-1.11 DevPack" >&2
        exit 1
    fi
done

if [[ $EUID -ne 0 ]]; then
    echo "error: must run as root (kernel module install, udev rules, ldconfig)" >&2
    exit 1
fi

echo "==> Applying kernel-driver compatibility patches to $sdk_dir"
for p in g39ddc-c.patch g39ddc-h.patch g39ddc-makefile.patch; do
    patch_file="$repo_dir/patches/$p"
    if (cd "$sdk_dir" && patch -p1 --dry-run --silent < "$patch_file" >/dev/null 2>&1); then
        (cd "$sdk_dir" && patch -p1 < "$patch_file")
        echo "    applied $p"
    elif (cd "$sdk_dir" && patch -p1 --dry-run --silent -R < "$patch_file" >/dev/null 2>&1); then
        echo "    $p already applied, skipping"
    else
        echo "error: $p does not apply cleanly to $sdk_dir -- DevPack version mismatch?" >&2
        exit 1
    fi
done

if [[ $skip_kernel -eq 0 ]]; then
    echo "==> Building and installing kernel module"
    make -C "$sdk_dir/driver"
    make -C "$sdk_dir/driver" install
    depmod -a
else
    echo "==> Skipping kernel module (--skip-kernel-module)"
fi

echo "==> Installing udev rule"
install -D -m 644 "$repo_dir/udev/99-g39ddc.rules" /etc/udev/rules.d/99-g39ddc.rules
udevadm control --reload-rules
udevadm trigger -s g39ddc || true

if [[ -z "$module_dir" ]]; then
    module_dir=$(SoapySDRUtil --info 2>/dev/null \
        | grep 'Search path:' \
        | grep -oE '/[^ ]+' \
        | grep -m1 '^/usr/local/' || true)
    module_dir=${module_dir:-/usr/local/lib64/SoapySDR/modules0.8}
fi

echo "==> Building and installing SoapySDR module into $module_dir"
make -C "$repo_dir/soapy" \
    VENDOR_HDR="$sdk_dir/c_cpp_header" \
    MODULE_DIR="$module_dir" \
    install

echo "==> Done. Verifying:"
SoapySDRUtil --find || true
