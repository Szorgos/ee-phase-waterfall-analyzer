# Aqulify – Fedora / COPR csomag. A verziót a .copr/Makefile a CMakeLists.txt-ből írja be.
Name:           aqulify
Version:        0.0.0
Release:        1%{?dist}
Summary:        Headphone EQ, convolver and limiter for PipeWire with a live 3D waterfall

License:        GPL-3.0-or-later AND MIT AND LGPL-3.0-or-later AND LGPL-2.1-or-later AND BSD-3-Clause
URL:            https://github.com/Szorgos/ee-phase-waterfall-analyzer
Source0:        %{name}-%{version}.tar.gz
# Az RNNoise betanított modellje (a kód a third_party/rnnoise mappában van)
Source1:        rnnoise_0.2+ds.orig.tar.xz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(fftw3f)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(webkit2gtk-4.1)
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       pipewire
Requires:       hicolor-icon-theme
# three.js r128 (MIT) a 3D vízeséshez
Provides:       bundled(js-three) = 0.128.0
# Az EasyEffects-szel azonos DSP könyvtárak (third_party/)
Provides:       bundled(lsp-common-lib) = 1.0.49
Provides:       bundled(lsp-dsp-lib) = 1.1.1
Provides:       bundled(lsp-lltl-lib) = 1.0.33
Provides:       bundled(lsp-runtime-lib) = 1.0.36
Provides:       bundled(lsp-dsp-units) = 1.0.38
Provides:       bundled(calf)
Provides:       bundled(libbs2b) = 3.1.0
Provides:       bundled(speexdsp)
Provides:       bundled(zita-convolver) = 4.0.0
Provides:       bundled(rnnoise) = 0.2

%description
Aqulify creates a virtual "Aqulify" audio output on PipeWire. Everything played
through it passes a low-latency DSP chain (parametric EQ with AutoEq import,
crossfeed, convolver, bass enhancer, true peak limiter), and a live 3D acoustic
waterfall, phase and group delay view shows what the processing does.

%prep
%autosetup -n %{name}-%{version}
mkdir -p rnnoise-model
tar -xJf %{SOURCE1} -C rnnoise-model --strip-components=2 rnnoise-0.2/src/rnnoise_data.c rnnoise-0.2/src/rnnoise_data.h

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON -DBUILD_LINUX_GUI=ON \
       -DAQULIFY_RNNOISE_MODEL_DIR=$PWD/rnnoise-model
%cmake_build

%install
%cmake_install
# A fejlesztői tesztprogramok nem kerülnek a csomagba
rm -f %{buildroot}%{_bindir}/test_*

%check
for t in test_ringbuffer test_core test_dsp; do
    %{__cmake_builddir}/$t
done
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.szorgos.Aqulify.desktop
appstream-util validate-relax --nonet %{buildroot}%{_metainfodir}/io.github.szorgos.Aqulify.metainfo.xml

%post
%systemd_user_post aqulify.service

%preun
%systemd_user_preun aqulify.service

%files
%license LICENSE
%doc README.md RELEASE_NOTES.md
%{_bindir}/aqulify-engine
%{_bindir}/aqulify-gui
%{_datadir}/aqulify/
%{_datadir}/applications/io.github.szorgos.Aqulify.desktop
%{_metainfodir}/io.github.szorgos.Aqulify.metainfo.xml
%{_datadir}/icons/hicolor/256x256/apps/aqulify.png
%{_userunitdir}/aqulify.service
%{_userpresetdir}/80-aqulify.preset

%changelog
* Tue Oct 20 2026 Szorgos - 1.5.3-1
- Első Fedora/COPR csomag
