# SPEC file for pg_fastload on PostgreSQL 18
# Copyright (C) 2009-2026 NTT, Inc.

%define sname                   pg_fastload
%define pgmajorversion  18

%define _prefix                 /usr/pgsql-%{pgmajorversion}
%define _libdir                 %{_prefix}/lib
%define _bcdir			%{_libdir}/bitcode/pg_fastload

Summary:        High speed data load utility for PostgreSQL
Name:           %{sname}
Version:        1.0.0
Release:        1%{?dist}
License:        BSD-3-Clause
Group:          Applications/Databases
# The release tarball is git archive output, published next to the RPMs:
# https://github.com/yuansong/pg_fastload/releases/download/v%{version}/pg_fastload-%{version}.tar.gz
Source0:        %{sname}-%{version}.tar.gz
URL:            https://github.com/yuansong/pg_fastload
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-%(%{__id_u} -n)

BuildRequires:  postgresql18-devel, postgresql18
Requires:       postgresql18


%description
pg_fastload provides high-speed data loading capability to PostgreSQL users.

When we load huge amount of data to a database, it is common situation that data set to be loaded is valid and consistent. For example, dedicated tools are used to prepare such data, providing data validation in advance. In such cases, we'd like to bypass any overheads within database system to load data as quickly as possible. pg_fastload is developed to help such situations. Therefore, it is not pg_fastload's goal to provide detailed data validation. Rather, pg_fastload asumes that loaded data set is validated by separate means. If you're not in such situation, you should use COPY command in PostgreSQL.


%package llvmjit
Requires: postgresql18-server, postgresql18-llvmjit
Requires: pg_fastload = %{version}
Summary:  Just-in-time compilation support for pg_fastload

%description llvmjit
Just-in-time compilation support for pg_fastload

%prep
rm -rf %{_libdir}/pgxs/src/backend/

%setup -n %{sname}-%{version}

%build
USE_PGXS=1 make %{?_smp_mflags} MAJORVERSION=%{pgmajorversion}

%install
%define pg_contribdir %{_datadir}/contrib
%define pg_extensiondir %{_datadir}/extension

rm -rf %{buildroot}

install -d %{buildroot}%{_bindir}
install -d %{buildroot}%{_libdir}
install -d %{buildroot}%{pg_contribdir}
install -d %{buildroot}%{pg_extensiondir}
install -d %{buildroot}%{_bcdir}

install -m 755 bin/pg_fastload                 %{buildroot}%{_bindir}/pg_fastload
install -m 755 bin/postgresql                  %{buildroot}%{_bindir}/postgresql
install -m 755 lib/pg_fastload.so              %{buildroot}%{_libdir}/pg_fastload.so
install -m 644 lib/pg_fastload.bc %{buildroot}%{_bcdir}/pg_fastload.bc

install -m 644 lib/pg_fastload.control         %{buildroot}%{pg_extensiondir}/pg_fastload.control
install -m 644 lib/pg_fastload--%{version}.sql        %{buildroot}%{pg_extensiondir}/pg_fastload--%{version}.sql

# sample_*.ctl files are needed for rpm users.
# %{sname}-%{version} is the same path with "%setup -n"'s argument.
install -m 644 docs/sample_bin.ctl              %{buildroot}%{pg_contribdir}/sample_bin.ctl
install -m 644 docs/sample_csv.ctl              %{buildroot}%{pg_contribdir}/sample_csv.ctl

%files
%defattr(755,root,root,755)
%{_bindir}/pg_fastload
%{_bindir}/postgresql
%{_libdir}/pg_fastload.so
%defattr(644,root,root,755)
#%doc README.pg_fastload
%{pg_contribdir}/sample_bin.ctl
%{pg_contribdir}/sample_csv.ctl
%{pg_extensiondir}/pg_fastload.control
%{pg_extensiondir}/pg_fastload--%{version}.sql

%files llvmjit
%defattr(0755,root,root)
%{_bcdir}
%defattr(0644,root,root)
%{_bcdir}/pg_fastload.bc

%clean
rm -rf %{buildroot}
rm -rf %{_libdir}/pgxs/src/backend/

%changelog
* Tue Nov 17 2026 - pg_fastload maintainers <yuansong@users.noreply.github.com> - 1.0.0-1
- First public release of pg_fastload
- Parallel loading with PARALLEL_WORKERS 1..64 and parallel export
- Supports PostgreSQL 15, 16, 17 and 18
