Name:           gmp-droid
Summary:        Gecko Media Plugin for droidmedia codec support
Version:        0.1
Release:        1
License:        MPL2.0
URL:            https://github.com/sailfishos/%{name}
Source0:        %{name}-%{version}.tar.gz
Source1:        gmp-generate-info.sh
BuildRequires:  meson
BuildRequires:  ninja
BuildRequires:  oneshot
BuildRequires:  pkgconfig(libandroid-properties)
BuildRequires:  droidmedia-devel
Requires:       droidmedia
Requires:  oneshot
%{_oneshot_requires_post}

%description
Gecko Media Plugin for droidmedia codec support in Gecko based browsers

%prep
%setup -q

%build
%meson
%meson_build

%install
%meson_install

# create config oneshot
install -D -m 0755 %{SOURCE1} %{buildroot}/%{_oneshotdir}/gmp-generate-info.sh
echo "%{_libdir}/%{name}/0.1/generate-info 1>%{_libdir}/%{name}/0.1/droid.info 2>/dev/null" >> %{buildroot}/%{_oneshotdir}/gmp-generate-info.sh
mkdir -p $RPM_BUILD_ROOT/%{_sharedstatedir}/environment/nemo/
echo "MOZ_GMP_PATH=\"%{_libdir}/%{name}/0.1/\"" > %{buildroot}/%{_sharedstatedir}/environment/nemo/70-browser-gmp.conf

%post
# Query device codec support and write out the droid.info file. On imager this should postpone until first boot.
%{_bindir}/add-oneshot gmp-generate-info.sh

%files
%defattr(-,root,root,-)
%license LICENSE
%dir %{_libdir}/%{name}
%dir %{_libdir}/%{name}/0.1
%{_libdir}/%{name}/0.1/libdroid.so
%ghost %{_libdir}/%{name}/0.1/droid.info
%{_libdir}/%{name}/0.1/generate-info
%{_oneshotdir}/gmp-generate-info.sh
%{_sharedstatedir}/environment/nemo/70-browser-gmp.conf
