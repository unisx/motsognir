#
# spec file for package motsognir
#
# Copyright (c) 2013 Mateusz Viste
#

Name: motsognir
Version: 1.0
Release: 1%{?dist}
Summary: A robust, reliable and easy to install gopher server
Url: http://sourceforge.net/projects/motsognir/
Group: Productivity/Networking/Other

License: GPL-3.0+
Source0: motsognir-1.0.tar.gz

BuildRequires: gcc

%description
Motsognir is a robust, reliable and easy to install open-source gopher server for Unix-like systems.

The Motsognir gopher server is meant to be used for small projects (like home servers), but should scale well on bigger architectures as well. All the configuration is done via a single configuration file, which has very reasonable defaults. That makes Motsognir easily maintainable, and allows the administrator to have a full knowledge of what features are allowed/enabled on the server, and what's not. Motsognir supports server-side CGI applications and PHP scripts, is plainly compatible with UTF-8 filesystems, and is entirely written in ANSI C without external dependencies.

%prep
%setup

%build
make

%check

%install
install -D motsognir %buildroot/%{_bindir}/motsognir
install -D motsognir.conf %buildroot/%{_sysconfdir}/motsognir.conf

%files
%attr(644, root, root) %doc license.txt changes.txt manual.pdf
%attr(644, root, root) %config %{_sysconfdir}/motsognir.conf
%attr(755, root, root) %{_bindir}/motsognir

%changelog
* Sat Sep 24 2013 Mateusz Viste <mateusz@viste-family.net> 1.0
 - v1.0 released
