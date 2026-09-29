%global _buildhost reproducible
%global use_source_date_epoch_as_buildtime 1
%global clamp_mtime_to_source_date_epoch 1
%global _build_id_links none
%global debug_package %{nil}
%{!?source_date_epoch:%global source_date_epoch 0}

Name:           lto-ltfs
Version:        0.1.0
Release:        22%{?dist}
Summary:        Auditable LTFS 2.4 runtime for LTO Archiver
License:        BSD-3-Clause AND BSD-1-Clause AND LGPL-2.1-only
URL:            https://github.com/Halino/lto-ltfs
Source0:        %{name}-%{version}.tar.gz
Source1:        99-lto-ltfs.rules
Source2:        lto-ltfs.conf

BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  diffutils
BuildRequires:  findutils
BuildRequires:  fuse-devel >= 2.9.9
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  git-core
BuildRequires:  libicu
BuildRequires:  libicu-devel
BuildRequires:  libtool
BuildRequires:  libubsan
BuildRequires:  libuuid-devel
BuildRequires:  libxml2-devel
BuildRequires:  make
BuildRequires:  openssl-devel
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
BuildRequires:  zlib-devel

Requires:       /usr/bin/fusermount
Requires(post): /usr/bin/systemd-tmpfiles
Requires:       /usr/lib/udev/scsi_id
Requires:       fuse-libs >= 2.9.9
Requires:       libxml2
Requires:       libuuid
Requires:       libicu
Requires:       openssl-libs
Requires:       zlib
Requires(pre):  shadow-utils

%description
A small RHEL 9-native LTFS runtime with stable device identity, exclusive
operation locking, structured events, read-only inspection, and hermetic
qualification. No vendor diagnostic payload or private media data is included.

%prep
%autosetup -p1

%build
export SOURCE_DATE_EPOCH=%{source_date_epoch}
# Independent jobs use distinct temporary topdirs. Normalize that prefix in
# object debug metadata before linking so the final ELF Build IDs are stable.
export CFLAGS="${CFLAGS} -ffile-prefix-map=%{_topdir}=/usr/src/debug/lto-ltfs"
export CXXFLAGS="${CXXFLAGS} -ffile-prefix-map=%{_topdir}=/usr/src/debug/lto-ltfs"
./autogen.sh
%configure --enable-fast --enable-tests --disable-snmp --disable-lintape
%make_build

%check
make check

%install
%make_install
rm -f %{buildroot}%{_bindir}/ltfs_ordered_copy
find %{buildroot}%{_libdir} -type f \( -name '*.la' -o -name '*.a' \) -delete
/bin/sh ./packaging/rpm/prune-buildroot.sh "%{buildroot}"

install -Dpm0644 %{SOURCE1} \
    %{buildroot}%{_udevrulesdir}/99-lto-ltfs.rules
install -Dpm0644 %{SOURCE2} \
    %{buildroot}%{_tmpfilesdir}/lto-ltfs.conf

install -d -m0755 %{buildroot}%{_sysconfdir}/lto-ltfs
printf '%s\n' \
	'{"nst_path":"__UNPROVISIONED__","sg_path":"__UNPROVISIONED__","serial":"__UNPROVISIONED__","wwid":"__UNPROVISIONED__"}' \
	> %{buildroot}%{_sysconfdir}/lto-ltfs/device.json
chmod 0640 %{buildroot}%{_sysconfdir}/lto-ltfs/device.json

install -d -m0755 %{buildroot}%{_datadir}/lto-ltfs/messages
(cd messages && find . -mindepth 2 -maxdepth 2 -type f \
    \( -name 'root.txt' -o -name 'en.txt' -o -name 'en_US.txt' \) \
    -exec install -Dpm0644 '{}' \
    '%{buildroot}%{_datadir}/lto-ltfs/messages/{}' ';')

%pre
getent group lto-admin >/dev/null || /usr/sbin/groupadd -r lto-admin

%post
/usr/bin/systemd-tmpfiles --create %{_tmpfilesdir}/lto-ltfs.conf

%files
%license LICENSE NOTICES COPYING.LIB LGPL-NOTICE provenance/upstream.json
%doc provenance/downstream-overlays.json provenance/upstream-files.sha256
%doc provenance/license-inventory.json
%{_bindir}/ltfs
%{_bindir}/ltfsck
%attr(0750,root,lto-admin) %{_bindir}/mkltfs
%{_bindir}/ltfs-info
%{_libdir}/libltfs.so*
%{_libdir}/ltfs/*.so*
%{_libdir}/pkgconfig/ltfs.pc
%{_includedir}/ltfs/
%{_mandir}/man1/*
%{_mandir}/man8/*
%{_datadir}/lto-ltfs/messages/
%{_udevrulesdir}/99-lto-ltfs.rules
%{_tmpfilesdir}/lto-ltfs.conf
%config(noreplace) %{_sysconfdir}/ltfs.conf
%config(noreplace) %attr(0640,root,lto-admin) %{_sysconfdir}/ltfs.conf.local
%config(noreplace) %attr(0640,root,lto-admin) %{_sysconfdir}/lto-ltfs/device.json

%changelog
* Mon Sep 28 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-22
- Preserve IBM and third-party BSD terms while providing the LGPL 2.1-only
  text and conservative HPE attribution for the identified downstream changes.
- Bind the package license and source inventory to the new release identity;
  no tape-operation behavior is changed.

* Tue Sep 08 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-21
- Bound SG logical-block-protection mode pages to validated response lengths
  and align close-time LBP reset with the IBM-only setup policy.
- Use alignment-safe endian conversions without changing LTFS wire bytes.
- Enforce fatal UBSan qualification and add direct endian/SG regressions.
- Require the UBSan runtime in the offline build environment for the fatal-error
  qualification probe; it is not a runtime dependency of the packaged driver.

* Tue Sep 08 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-20
- Accept HP MAM Volume Identifier values from zero through 32 bytes without
  a false warning or copying the following attribute descriptor.
- Preserve exact-length checks for all other MAM fields and read-only guards.

* Tue Sep 08 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-19
- Enforce read-only policy before device open/load and preserve it across refresh.
- Reject MAM/index writes and repair in read-only sessions; skip periodic and
  unmount commits without changing normal writable finalization behavior.
- Rebuild all consumers of the widened internal write-protection field.

* Sun Sep 06 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-18
- Add an explicitly selected read-only capacity diagnostic using the shared
  strict Tape Capacity and Volume Statistics parsers.
- Preserve release17 drive-family selection, capacity units, VCI serialization
  and operation-option behavior; no physical qualification is claimed.

* Sun Sep 06 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-17
- Select the MiB-valued Tape Capacity page for HP LTO-6 without changing
  Volume Statistics units or other drive-family selection.
- Serialize the required VCI application signature NUL deterministically.
- Preserve validated nonempty operation options through initial parsing
  without false invalid-option diagnostics (G01).

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-16
- Expose a stable kernel FUSE source name instead of leaking the anchored
  generic-SCSI descriptor into mountinfo.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-15
- Anchor the configured generic-SCSI descriptor and pass that same endpoint to
  the Linux SG backend for mount and check operations.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-14
- Allow the lto-admin runtime to create operation lock files in the root-owned
  lock directory while keeping the directory closed to other users.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-13
- Permit the root-owned device selector to be group-readable without granting
  any group or world write access, so the confined ArchiveRunner can perform
  the same read-only identity probe as the root broker.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-12
- Open the Linux SG endpoint read-write so the kernel admits the allowlisted
  READ ATTRIBUTE command without granting CAP_SYS_RAWIO to the caller.
- Add a read-only identity-resolution diagnostic with explicit phase and errno.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-11
- Treat an HPE 0xffffffff used-capacity partition sentinel as unavailable
  optional telemetry instead of rejecting otherwise valid LTFS identity.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-10
- Return success from the Linux SG backend after successful resource cleanup
  and avoid an unnecessary append-only MODE SELECT when that mode is inactive.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-9
- Preserve command-qualified READ ILI, filemark, EOD, record, and cleaning
  outcomes and WRITE/WRITE FILEMARKS early-warning and cleaning outcomes while
  leaving unqualified sequential no-sense results fail-closed as ambiguous.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-8
- Preserve recognized READ filemark sense results through the SG retry layer so
  freshly formatted partition labels mount normally.

* Tue Aug 25 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-7
- Accept HPE pre-format volume statistics that report native maximum and used
  capacity while preserving remaining-capacity priority and strict validation.

* Mon Aug 24 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-6
- Use read-only MAM Medium Serial Number 0x0401 for physical continuity and
  classify otherwise complete pre-format LTFS media without barcode safely.

* Mon Aug 24 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-5
- Report bindable schema-2 pre-format media identity for label-first
  qualification.

* Mon Aug 24 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-4
- Use a flat udev SG alias that is available without a pre-created /dev parent.

* Mon Aug 24 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-3
- Leave the nested stable-device namespace exclusively to udev.

* Sun Aug 23 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-2
- Add fail-closed read-only pre-format media identity inspection.

* Fri Aug 21 2026 LTO Archiver Engineering <noreply@example.invalid> - 0.1.0-1
- Initial reproducible RHEL 9 package.
