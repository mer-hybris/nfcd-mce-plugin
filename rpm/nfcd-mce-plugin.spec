Name: nfcd-mce-plugin

Version: 1.0.1
Release: 0
Summary: nfcd plugin for mce-based device state tracking
License: BSD
URL: https://github.com/mer-hybris/nfcd-mce-plugin
Source: %{name}-%{version}.tar.bz2

%define nfcd_version 1.1.11
%define libmce_version 1.0.5

BuildRequires: pkgconfig
BuildRequires: pkgconfig(libglibutil)
BuildRequires: pkgconfig(libmce-glib) >= %{libmce_version}
BuildRequires: pkgconfig(nfcd-plugin) >= %{nfcd_version}

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

Requires: libmce-glib >= %{libmce_version}
Requires: nfcd >= %{nfcd_version}

%define plugin_dir %{_libdir}/nfcd/plugins

%description
%{summary}.

%prep
%setup -q

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} PLUGIN_DIR=%{plugin_dir} install

%post
systemctl reload-or-try-restart nfcd.service ||:

%postun
systemctl reload-or-try-restart nfcd.service ||:

%files
%defattr(-,root,root,-)
%dir %{plugin_dir}
%{plugin_dir}/*.so
%if %{license_support} == 0
%license LICENSE
%endif
