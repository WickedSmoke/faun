Summary: High-level C library for playing sound & music
Name: faun
Version: 0.2.0
Release: %autorelease
License: GPL-2.0-or-later
URL: https://codeberg.org/wickedsmoke/faun
Source: https://codeberg.org/wickedsmoke/faun/archive/v%{version}.tar.gz
BuildRequires: gcc, make, libvorbis-devel, pulseaudio-libs-devel

%global debug_package %{nil}

%description
Faun is a high-level C API for playback of sound & music in games & demos.
It is a modestly sized library designed to use pre-packaged audio and is not
intended for synthesizer or audio manipulation applications.

%package devel
Summary: Development files for Faun
Requires: %{name}%{?_isa} = %{version}-%{release}

%description devel
This package contains the header files and libraries needed to build
programs that use Faun.

%prep
%setup -q -n %{name}

%build
make

%install
make DESTDIR="$RPM_BUILD_ROOT/usr" install

%clean
rm -rf $RPM_BUILD_ROOT

%files
%license COPYING
%defattr(-,root,root)
%{_libdir}/libfaun.so.0
%{_libdir}/libfaun.so.%{version}

%files devel
%defattr(-,root,root)
%dir %{_includedir}
%{_libdir}/libfaun.so
%{_includedir}/faun.h

%changelog
* Thu Jun 15 2023 Karl Robillard <wickedsmoke@users.sf.net>
  - Initial package release.
