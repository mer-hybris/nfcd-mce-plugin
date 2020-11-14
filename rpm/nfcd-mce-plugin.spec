Name: nfcd-mce-plugin
Version: 1.0.1
Release: 0
Summary: nfcd plugin for mce-based device state tracking
License: BSD
URL: https://github.com/mer-hybris/nfcd-mce-plugin
Source: %{name}-%{version}.tar.bz2

BuildRequires: pkgconfig(libglibutil)
BuildRequires: pkgconfig(libmce-glib)
BuildRequires: pkgconfig(nfcd-plugin)
Requires: nfcd

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
