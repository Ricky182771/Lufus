Name:           rufus-linux
Version:        0.1.0
Release:        1%{?dist}
Summary:        Bootable USB creator for Linux, inspired by Rufus

License:        GPL-3.0-or-later
URL:            https://github.com/rufuslinux/rufus-linux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  qt6-qtsvg-devel
BuildRequires:  qt6-qttools-devel
BuildRequires:  systemd-devel
BuildRequires:  libblkid-devel
BuildRequires:  util-linux-devel
BuildRequires:  libcurl-devel

%description
Rufus Linux is a utility that helps format and create bootable USB flash drives,
such as USB keys/pendrives, memory sticks, etc. It provides a 1:1 functional
Linux equivalent to the Windows application Rufus.

%prep
%setup -q

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%{_bindir}/rufus-linux
%{_datadir}/applications/rufus-linux.desktop
%{_datadir}/icons/hicolor/scalable/apps/rufus-linux.svg
%{_datadir}/polkit-1/actions/org.rufuslinux.pkexec.policy

%changelog
* Fri May 08 2026 Rufus Linux Team <team@rufuslinux.org> - 0.1.0-1
- Initial release
