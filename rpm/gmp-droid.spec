%define gecko_ver 52.9.1

Name:           gmp-droid
Summary:        Gecko Media Plugin for droidmedia codec support
Version:        0.1
Release:        1
License:        MPL2.0
URL:            https://github.com/sailfishos/%{name}
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  meson
BuildRequires:  pkgconfig(libandroid-properties)
BuildRequires:  droidmedia-devel
Requires:       droidmedia

%description
Gecko Media Plugin for droidmedia codec support in Gecko based browsers

%prep
%setup -q

%build
%meson -Dgecko_ver=%{gecko_ver}

%install
%meson_install

%post
# Query device codec support and write out the droid.info file
%{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1/generate-info 1>%{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1/droid.info 2>/dev/null

%files
%license LICENSE
%dir %{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}
%dir %{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1
%{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1/libdroid.so
%ghost %{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1/droid.info
%{_libdir}/xulrunner-qt5-%{gecko_ver}/%{name}/0.1/generate-info
