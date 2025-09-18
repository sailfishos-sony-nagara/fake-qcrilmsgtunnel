Name: fake-qcrilmsgtunnel

Version: 0.2.1
Release: 1
Summary: Fake qcrilmsgtunnel service
License: BSD-3-Clause
URL: https://github.com/sailfishos-sony-nagara/fake-qcrilmsgtunnel
Source: %{name}-%{version}.tar.bz2

BuildRequires: cmake
BuildRequires: pkgconfig
BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(libglibutil)
BuildRequires: pkgconfig(libgbinder)

Requires(post): systemd
Requires(postun): systemd

%description
%{summary}

%prep
%setup -q -n %{name}-%{version}

%build
mkdir build-rpm || true
cd build-rpm
%cmake ..
make %{?_smp_mflags}

%install
rm -rf %{buildroot}
(cd build-rpm && make DESTDIR=%{buildroot} install)

install -d $RPM_BUILD_ROOT%{_unitdir}/graphical.target.wants/
install -m 644 -D %{name}.service %{buildroot}%{_unitdir}/%{name}.service
ln -s ../%{name}.service $RPM_BUILD_ROOT%{_unitdir}/graphical.target.wants/%{name}.service

%preun
systemctl daemon-reload || :

%post
systemctl daemon-reload || :

%files
%defattr(-,root,root,-)
%{_sbindir}/%{name}
%{_unitdir}/%{name}.service
%{_unitdir}/graphical.target.wants/%{name}.service
