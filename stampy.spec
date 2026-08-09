Name:           stampy
Version:        1.0.0
Release:        1%{?dist}
Summary:        Photo stamping and geolocation utility

License:        GPL-3.0-or-later
URL:            https://github.com/cobaltty/stampy
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  pkgconfig
BuildRequires:  pkgconf-pkg-config
BuildRequires:  gtk4-devel
BuildRequires:  libadwaita-devel
BuildRequires:  libgexiv2-devel
BuildRequires:  json-glib-devel

%description
A GTK4 application to batch stamp photos with metadata and geolocation.

%prep
%autosetup -n stampy-1.0.0

%build
%meson
%meson_build

%install
%meson_install

%files
%{_bindir}/stampy
%{_datadir}/applications/com.cobaltty.stampy.desktop
%{_datadir}/glib-2.0/schemas/com.cobaltty.stampy.gschema.xml
%{_datadir}/icons/hicolor/*/apps/com.cobaltty.stampy*.svg
%{_datadir}/metainfo/com.cobaltty.stampy.metainfo.xml

%changelog
* Sun Aug 02 2026 Riqi Ramadhan <riqi@local> - 1.0.0-1
- Initial RPM release