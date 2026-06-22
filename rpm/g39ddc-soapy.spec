# Packages the SoapySDR plugin module (our own code, soapy/SoapyG39DDC.cpp)
# AND drives a full, working install of WiNRADiO/RADIXON's proprietary
# kernel driver -- without ever bundling or redistributing any of their
# code ourselves:
#
#   - The SoapySDR module is built once, at RPM-build time, against only
#     the vendor's public C API header (Source0, fetched from WiNRADiO's
#     own download URL -- never bundled in this repo). It loads the
#     vendor's libg39ddcapi.so via dlopen() at runtime, so the resulting
#     binary embeds none of WiNRADiO's code.
#   - The kernel module can't be prebuilt and shipped this way (it must
#     match the exact kernel running on the *install target*, and a
#     compiled .ko would be a derivative binary of the vendor's
#     proprietary driver source). Instead, %post fetches the same public
#     vendor tarball fresh on the target machine, applies the patches/
#     (our own diffs, shipped in this package), and builds+installs the
#     kernel module against the running kernel -- exactly what install.sh
#     does by hand, just triggered automatically by `rpm -i`/`dnf install`.
#
# Build with: rpmbuild --define "_repodir /path/to/this/repo/checkout" -ba g39ddc-soapy.spec
# (CI passes _repodir automatically; see .github/workflows/ci.yml)
#
# Note: this does a one-shot kernel module build against whatever kernel
# is running at install time -- it is not a DKMS package, so a kernel
# upgrade later requires re-running `rpm --reinstall` (or install.sh) to
# rebuild against the new kernel.

%if "%{?_repodir}" == ""
%global _repodir %{getenv:GITHUB_WORKSPACE}
%endif

%global debug_package %{nil}

Name:           g39ddc-soapy
Version:        1.0.0
Release:        1%{?dist}
Summary:        Kernel driver + SoapySDR module for the WiNRADiO G39DDC EXCELSIOR

License:        MIT
URL:            https://github.com/BMichaud7/winradio-g39ddc
Source0:        https://linradio.com/software/g39ddc-1.11.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  pkgconfig(SoapySDR)

Requires:       gcc
Requires:       make
Requires:       patch
Requires:       curl
Requires:       tar
Requires:       kmod

%description
Installs everything needed to get a WiNRADiO G39DDC EXCELSIOR DDC
receiver working with SoapySDR:

 - The SoapySDR plugin module (this package's own code -- contains no
   WiNRADiO/RADIXON code; it only links against their public API header
   at build time and dlopen()s their library at runtime).
 - WiNRADiO's kernel driver, patched for modern (6.x) kernels: %post
   downloads the vendor's DevPack fresh from their own public download
   URL (never bundled in this RPM), applies the patches shipped here,
   and builds+installs it against the kernel running on this machine.
 - The udev rule that lets a normal user open the device without root.

Requires network access at install time to fetch the vendor DevPack, and
kernel headers matching the running kernel (`/lib/modules/$(uname -r)/build`)
to build the kernel module -- %post checks for these and fails with a
clear message if they're missing, rather than silently skipping the
driver.

%prep
%setup -q -n g39ddc-1.11

%build
g++ -std=c++17 -fPIC -Wall -Wextra -O2 \
    -I c_cpp_header \
    "%{_repodir}/soapy/SoapyG39DDC.cpp" \
    -o libG39DDCSupport.so -shared -lSoapySDR -ldl

%install
install -D -m 755 libG39DDCSupport.so \
    %{buildroot}%{_libdir}/SoapySDR/modules0.8/libG39DDCSupport.so
install -D -m 644 "%{_repodir}/udev/99-g39ddc.rules" \
    %{buildroot}%{_prefix}/lib/udev/rules.d/99-g39ddc.rules
install -D -m 644 "%{_repodir}/patches/g39ddc-c.patch" \
    %{buildroot}%{_datadir}/g39ddc-soapy/patches/g39ddc-c.patch
install -D -m 644 "%{_repodir}/patches/g39ddc-h.patch" \
    %{buildroot}%{_datadir}/g39ddc-soapy/patches/g39ddc-h.patch
install -D -m 644 "%{_repodir}/patches/g39ddc-makefile.patch" \
    %{buildroot}%{_datadir}/g39ddc-soapy/patches/g39ddc-makefile.patch
cp "%{_repodir}/README.md" README.md
cp "%{_repodir}/LICENSE" LICENSE

%files
%license LICENSE
%doc README.md
%{_libdir}/SoapySDR/modules0.8/libG39DDCSupport.so
%{_prefix}/lib/udev/rules.d/99-g39ddc.rules
%{_datadir}/g39ddc-soapy/patches/g39ddc-c.patch
%{_datadir}/g39ddc-soapy/patches/g39ddc-h.patch
%{_datadir}/g39ddc-soapy/patches/g39ddc-makefile.patch

%post
set -e
kernel_build_dir="/lib/modules/$(uname -r)/build"
if [ ! -d "$kernel_build_dir" ]; then
    echo "g39ddc-soapy: $kernel_build_dir not found -- install kernel headers for" >&2
    echo "  the running kernel (e.g. 'dnf install kernel-devel-\$(uname -r)') and" >&2
    echo "  then run: rpm --reinstall g39ddc-soapy" >&2
    echo "g39ddc-soapy: SoapySDR module is installed, but the kernel driver is NOT." >&2
    exit 1
fi

workdir=$(mktemp -d)
trap 'rm -rf "$workdir"' EXIT
echo "g39ddc-soapy: fetching WiNRADiO DevPack to build the kernel driver for $(uname -r)..."
curl -fsSL -o "$workdir/g39ddc-1.11.tar.gz" https://linradio.com/software/g39ddc-1.11.tar.gz
tar xzf "$workdir/g39ddc-1.11.tar.gz" -C "$workdir"
cd "$workdir/g39ddc-1.11"
patch -p1 < %{_datadir}/g39ddc-soapy/patches/g39ddc-c.patch
patch -p1 < %{_datadir}/g39ddc-soapy/patches/g39ddc-h.patch
patch -p1 < %{_datadir}/g39ddc-soapy/patches/g39ddc-makefile.patch
make -C driver
make -C driver install
udevadm control --reload-rules || true
udevadm trigger -s g39ddc || true
ldconfig
echo "g39ddc-soapy: kernel driver + SoapySDR module installed. Plug in the device and run: SoapySDRUtil --find"

%postun
if [ "$1" -eq 0 ]; then
    /sbin/modprobe -r g39ddc 2>/dev/null || true
    rm -f /lib/modules/*/extra/WiNRADiO/g39ddc.ko 2>/dev/null || true
    /sbin/depmod -a || true
fi

%changelog
* Sat Jun 20 2026 Brendan Michaud <brendanmichaud7@gmail.com> - 1.0.0-1
- Initial packaging: SoapySDR module built at RPM-build time; kernel
  driver fetched from the vendor and built fresh against the running
  kernel in %post, so a single rpm/dnf install does the whole job.
