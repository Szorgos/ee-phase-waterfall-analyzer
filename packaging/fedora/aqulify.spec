# Aqulify – Fedora / COPR csomag. A verziót a .copr/Makefile a CMakeLists.txt-ből írja be.
Name:           aqulify
Version:        0.0.0
Release:        1%{?dist}
Summary:        Headphone EQ, convolver and limiter for PipeWire with a live 3D waterfall

License:        GPL-3.0-or-later AND MIT
URL:            https://github.com/Szorgos/ee-phase-waterfall-analyzer
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(webkit2gtk-4.1)
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

Requires:       pipewire
Requires:       hicolor-icon-theme
# three.js r128 (MIT) a 3D vízeséshez
Provides:       bundled(js-three) = 0.128.0

%description
Aqulify creates a virtual "Aqulify" audio output on PipeWire. Everything played
through it passes a low-latency DSP chain (parametric EQ with AutoEq import,
crossfeed, convolver, bass enhancer, true peak limiter), and a live 3D acoustic
waterfall, phase and group delay view shows what the processing does.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_PIPEWIRE_PLUGIN=ON -DBUILD_LINUX_GUI=ON
%cmake_build

%install
%cmake_install
# A fejlesztői tesztprogramok nem kerülnek a csomagba
rm -f %{buildroot}%{_bindir}/test_*

%check
for t in test_math test_pipeline test_ringbuffer test_core test_dsp; do
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
* Tue Oct 20 2026 Sass Gergő <gergoeagle@gmail.com> - 1.5.0-1
- Első Fedora/COPR csomag
