![](https://img.shields.io/github/issues/lineartapefilesystem/ltfs.svg)
![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-centos8.yml/badge.svg)

# Linear Tape File System (LTFS)

Linear Tape File System (LTFS) is a filesystem to mount a LTFS formatted tape in a tape drive. Once LTFS mounts a LTFS formatted tape as filesystem, user can access to the tape via filesystem API.

Objective of this project is being the reference implementation of the LTFS format Specifications in [SNIA](https://www.snia.org/tech_activities/standards/curr_standards/ltfs).

At this time, the target of this project to meet is the LTFS format specifications 2.4. (https://www.snia.org/sites/default/files/technical_work/LTFS/LTFS_Format_2.4.0_TechPosition.pdf).

## LTFS Format Specifications

LTFS Format Specification is specified data placement, shape of index and names of extended attributes for LTFS. This specification is defined in [SNIA](https://www.snia.org/tech_activities/standards/curr_standards/ltfs) first and then it is forwarded to [ISO](https://www.iso.org/home.html) as ISO/IEC 20919 from version 2.2.

The table below show status of the LTFS format Specification

  | Version | Status of SNIA                                                                                               | Status of ISO             |
  |:-------:|:------------------------------------------------------------------------------------------------------------:|:-------------------------:|
  | 2.2     | [Published](http://snia.org/sites/default/files/LTFS_Format_2.2.0_Technical_Position.pdf)                    | Published as `20919:2016` |
  | 2.3.1   | [Published](https://www.snia.org/sites/default/files/technical_work/LTFS/LTFS_Format_2.3.1_TechPosition.PDF) | -                         |
  | 2.4     | [Published](https://www.snia.org/sites/default/files/technical_work/LTFS/LTFS_Format_2.4.0_TechPosition.pdf) | -                         |

## How to use the LTFS (Quick start)

This section is for person who already have a machine the LTFS is installed.

Instruction how to use the LTFS is on [Wiki](https://github.com/LinearTapeFileSystem/ltfs/wiki). Please take a look!

## Getting Started from GitHub project

These instructions will get you a copy of the project up and running on your local machine for development and testing purposes.

## Prerequisites

- Linux
  * automake 1.13.4 or later
  * autoconf 2.69 or later
  * libtool 2.4.2 or later
  * fuse 2.6.0 or later
  * uuid 1.36 or later (Linux)
  * libxml-2.0 2.6.16 or later
  * net-snmp 5.3 or later
  * icu4c 4.8 or later

- OSX (macOS)

  Following packages on homebrew

  * automake
  * autoconf
  * libtool
  * osxfuse (brew cask install osxfuse)
  * ossp-uuid
  * libxml2
  * icu4c
  * gnu-sed

- FreeBSD:
  * FreeBSD 10.2 or 11.0 or later (for sa(4) driver changes)
  * automake
  * autoconf
  * libtool
  * fusefs-libs
  * net-snmp
  * e2fsprogs-libuuid
  * libxml2
  * icu

- NetBSD:
  * NetBSD 7.0 or higher (for FUSE support)
  * automake
  * autoconf
  * libtool
  * libfuse
  * net-snmp
  * libuuid
  * libxml2
  * icu

## Supported Tape Drives

  | Vendor  | Drive Type              | Minimum F/W Level |
  |:-------:|:-----------------------:|:-----------------:|
  | IBM     | LTO5                    | B170              |
  | IBM     | LTO6                    | None              |
  | IBM     | LTO7                    | None              |
  | IBM     | LTO8                    | HB81              |
  | IBM     | LTO9                    | None              |
  | IBM     | TS1140                  | 3694              |
  | IBM     | TS1150                  | None              |
  | IBM     | TS1155                  | None              |
  | IBM     | TS1160                  | None              |
  | HP      | LTO5                    | T.B.D.            |
  | HP      | LTO6                    | T.B.D.            |
  | HP      | LTO7                    | T.B.D.            |
  | HP      | LTO8                    | T.B.D.            |
  | HP      | LTO9                    | T.B.D.            |
  | Quantum | LTO5 (Only Half Height) | T.B.D.            |
  | Quantum | LTO6 (Only Half Height) | T.B.D.            |
  | Quantum | LTO7 (Only Half Height) | T.B.D.            |
  | Quantum | LTO8 (Only Half Height) | T.B.D.            |
  | Quantum | LTO9 (Only Half Height) | T.B.D.            |

## Installing

LTFS Format Specification is specified data placement, shape of index and names of extended attributes for LTFS. This specification is defined in [SNIA](https://www.snia.org/tech_activities/standards/curr_standards/ltfs) first and then it is forwarded to [ISO](https://www.iso.org/home.html) as ISO/IEC 20919 from version 2.2.

The table below show status of the LTFS format Specification

  | Version | Status of SNIA                                                                                                        | Status of ISO                                                        |
  |:-------:|:---------------------------------------------------------------------------------------------------------------------:|:--------------------------------------------------------------------:|
  | 2.2     | [Published](http://snia.org/sites/default/files/LTFS_Format_2.2.0_Technical_Position.pdf)                             | [Published as `20919:2016`](https://www.iso.org/standard/69458.html) |
  | 2.3.1   | [Published](https://www.snia.org/sites/default/files/technical_work/LTFS/LTFS_Format_2.3.1_TechPosition.PDF)          | -                                                                    |
  | 2.4     | [Published](https://www.snia.org/sites/default/files/technical_work/LTFS/LTFS_Format_2.4.0_TechPosition.pdf)          | -                                                                    |
  | 2.5.1   | [Published](https://www.snia.org/sites/default/files/technical-work/ltfs/release/SNIA-LTFS-Format-2-5-1-Standard.pdf) | [Published as `20919:2021`](https://www.iso.org/standard/80598.html) |

# How to use the LTFS (Quick start)

This section is for a person who already has a machine with the LTFS installed. Instructions on how to use the LTFS is also available on [Wiki](https://github.com/LinearTapeFileSystem/ltfs/wiki).

## Step1: List tape drives

`# ltfs -o device_list`

The output is as follows. You have 3 drives in this example and you can use "Device Name" field, like `/dev/sg43` in this case, as the argument of ltfs command to mount the tape drive.

```
50c4 LTFS14000I LTFS starting, LTFS version 2.4.0.0 (10022), log level 2.
50c4 LTFS14058I LTFS Format Specification version 2.4.0.
50c4 LTFS14104I Launched by "/home/piste/ltfsoss/bin/ltfs -o device_list".
50c4 LTFS14105I This binary is built for Linux (x86_64).
50c4 LTFS14106I GCC version is 4.8.5 20150623 (Red Hat 4.8.5-11).
50c4 LTFS17087I Kernel version: Linux version 3.10.0-514.10.2.el7.x86_64 (mockbuild@x86-039.build.eng.bos.redhat.com) (gcc version 4.8.5 20150623 (Red Hat 4.8.5-11) (GCC) ) #1 SMP Mon Feb 20 02:37:52 EST 2017 i386.
50c4 LTFS17089I Distribution: NAME="Red Hat Enterprise Linux Server".
50c4 LTFS17089I Distribution: Red Hat Enterprise Linux Server release 7.3 (Maipo).
50c4 LTFS17089I Distribution: Red Hat Enterprise Linux Server release 7.3 (Maipo).
50c4 LTFS17085I Plugin: Loading "sg" tape backend.
Tape Device list:.
Device Name = /dev/sg43, Vender ID = IBM    , Product ID = ULTRIUM-TD5    , Serial Number = 9A700L0077, Product Name = [ULTRIUM-TD5] .
Device Name = /dev/sg38, Vender ID = IBM    , Product ID = ULT3580-TD6    , Serial Number = 00013B0119, Product Name = [ULT3580-TD6] .
Device Name = /dev/sg37, Vender ID = IBM    , Product ID = ULT3580-TD7    , Serial Number = 00078D00C2, Product Name = [ULT3580-TD7] .
```

## Step2: Format a tape

As described in the LTFS format specifications, LTFS uses the partition feature of the tape drive. This means you can't use a tape just after you purchase a tape. You need format the tape before using it on LTFS.

To format a tape, you can use `mkltfs` command like

`# mkltfs -d 9A700L0077`

In this case, `mkltfs` tries to format a tape in the tape drive `9A700L0077`. You can use the device name `/dev/sg43` instead.

## Step3: Mount a tape through a tape drive

After you prepared a formatted tape, you can mount it through a tape drive like

`# ltfs -o devname=9A700L0077 /ltfs`

In this command, the ltfs command will try to mount the tape in the tape drive `9A700L0077` to `/ltfs` directory. Of course, you can use a device name `/dev/sg43` instead.

If the mount process is successfully done, you can access to the LTFS tape through `/ltfs` directory.

You must not touch any `st` devices while ltfs is mounting a tape.

## Step4: Unmount the tape drive

You can use following command when you want to unmount the tape. The ltfs command try to write the current meta-data to the tape and close the tape cleanly.

`# umount /ltfs`

One thing you need to pay attention to here is, that the unmount command continues to work in the background after it returns. It just initiates a trigger to notify the the ltfs command of the unmount request. Actual unmount is completed when the ltfs command is finished.

## The `ltfs_ordered_copy` utility

The [`ltfs_ordered_copy`](https://github.com/LinearTapeFileSystem/ltfs/wiki/ltfs_ordered_copy) is a program to copy files from source to destination with LTFS  order  optimization.

It is written in python and it can work with both python2 and python3 (Python 2.7 or later is strongly recommended). You need to install the `pyxattr` module for both python2 and python3.

# Building the LTFS from this GitHub project

These instructions will get a copy of the project up and running on your local machine for development and testing purposes.

## Prerequisites for build

Please refer [this page](https://github.com/LinearTapeFileSystem/ltfs/wiki/Build-Environments).

## Build and install on Linux

```
./autogen.sh
./configure
make
make install
```

`./configure --help` shows various options for build and install.

In some systems, you might need `sudo ldconfig -v` after `make install` to load the shared libraries correctly.

### Verified RHEL 9 RPM

The [GitHub-built release verification guide](docs/public-release-verification.md)
describes separately approved signed RPM/SRPM assets, source and attestation
checks, and licensing and no-tape limits. This checkout alone does not prove
that a public Release exists.

The downstream `lto-ltfs` package is built only from a clean Git commit. The
driver uses the digest-pinned UBI 9 packaging container twice, requires every
RPM and SRPM byte to match, verifies both packages, and performs an isolated
install/version/fake-backend/`rpm -V`/remove gate without accessing a device:

```
scripts/build-rpm.sh \
  --rpm-bundle /srv/lto-ltfs/rhel9-rpms \
  --rpm-lock /srv/lto-ltfs/RPM-BUNDLE.sha256 \
  --output /tmp/lto-ltfs-rpm-a
(cd /tmp/lto-ltfs-rpm-a && sha256sum -c SHA256SUMS)
scripts/verify-rpm.py /tmp/lto-ltfs-rpm-a/lto-ltfs-0.1.1-22.el9.x86_64.rpm
scripts/verify-rpm.py --srpm \
  /tmp/lto-ltfs-rpm-a/lto-ltfs-0.1.1-22.el9.src.rpm \
  --source-manifest /tmp/lto-ltfs-rpm-a/SOURCE-MANIFEST.json
```

The RPM bundle is an administrator-supplied, offline input with separate
`build/` and `runtime/` RPM sets. Its lock lists every file as
`SHA256  build/name.rpm` or `SHA256  runtime/name.rpm`; extra, missing, nested,
or mismatched files are rejected. The runtime set must provide the executable
`/usr/bin/fusermount` required by FUSE 2; `fusermount3` is not a substitute,
and the isolated install gate verifies that the exact helper is executable.
Podman uses the locally present digest-pinned
UBI 9 image with `--pull=never`, `--network=none`, and `--no-cache` for two
independent builds. Missing bundles, locks, images, or real container gates fail
closed. `--source-only` and fake-Podman unit tests do not qualify a release RPM.

`SOURCE-MANIFEST.json` is generated outside the container from the clean Git
archive. SRPM verification requires that external whole-archive trust anchor
and never executes code from the source package. Signed RPM envelope metadata
is compared separately. Never install with `--nodeps` or create compatibility
SONAME symlinks.

Install the verified RPM with the normal RHEL package manager. Installation
creates the host group `lto-admin`, installs a tape-class SCSI-generic udev rule
for flat `/dev/lto-archiver-scsi-*` aliases, and keeps `mkltfs`
restricted to mode 0750 and group `lto-admin`. It does not open, format, repair,
load, unload, or probe a tape. The udev rule invokes the system `scsi_id` helper
only for a tape-class SG device `add` event when `ID_SERIAL` is absent, then
creates the escaped `/dev/lto-archiver-scsi-*` link directly below the
udev-owned `/dev` root, so no persistent parent directory is required across a
reboot. Systemd-tmpfiles creates only `/run/lock/lto-ltfs`, mode 0770 and
owned by `root:lto-admin`, so the confined runtime can create its operation
lock files without making the directory writable by other users. RPM scripts
never create device paths, trigger udev, or run that helper. For an already
attached drive, quiesce LTFS and every tape user before an administrator
explicitly retriggers the add event; never retrigger while a mount or device
descriptor is active.

The package installs `/etc/lto-ltfs/device.json` with the runtime's exact four
keys, but every value is `__UNPROVISIONED__`.  That syntactically valid JSON is
deliberately rejected by `ltfs`, `ltfsck`, and normal `ltfs-info` inspection.
It contains no deploy-specific identifier.

An administrator first performs explicit metadata-only LTFS discovery through
the stable aliases. This command resolves the paired device identity from
sysfs and does not send a SCSI command or mount media; the earlier udev add
event may have used the read-only `scsi_id` inquiry described above:

```sh
sudo sh -c '
umask 077
ltfs-info --discover --json \
  --device /dev/lto-archiver-scsi-APPROVED-SG-ALIAS \
  --tape-device /dev/tape/by-id/APPROVED-NST-ALIAS \
  > /run/lto-ltfs-device.review.json
'
sudo cat /run/lto-ltfs-device.review.json
```

After comparing that root-owned review file with the approved host inventory,
root publishes it through a fresh mode-0640 `root:lto-admin` file in the
root-owned target directory, syncs it, and renames it over the configuration
without following a pre-existing destination symlink.  Group access is
read-only so the confined non-root ArchiveRunner can run the same identity
probe as the broker without gaining any ability to alter the selector:

```sh
sudo sh -c '
set -eu
staged=$(mktemp /etc/lto-ltfs/.device.json.XXXXXX)
trap '\''rm -f -- "$staged"'\'' EXIT HUP INT TERM
install -o root -g lto-admin -m 0640 /run/lto-ltfs-device.review.json "$staged"
sync -f "$staged"
mv -T -- "$staged" /etc/lto-ltfs/device.json
sync -f /etc/lto-ltfs
trap - EXIT HUP INT TERM
rm -f -- /run/lto-ltfs-device.review.json
'
```

The provisioned document has exactly `nst_path`, `sg_path`, `serial`, and
`wwid`, all non-empty strings.  The paths must be leaf aliases below
`/dev/tape/by-id/` and the flat `/dev/lto-archiver-scsi-` prefix.  Normal
`ltfs-info --json --mode unmounted`, `ltfsck`,
and `ltfs` load this root-owned, non-group/world-writable file and verify both
paths resolve to the configured serial, WWID, SCSI tuple, and device types.
Before an explicitly authorized format, `ltfs-info --json --mode pre-format`
uses the same provisioned paths, device-identity guard, lock, and read-only SCSI
opcode allowlist. Every successful record requires the physical MAM serial;
the application barcode remains mandatory except for the specific pre-format
classification described below. The wire key `mam_volume_serial` is the
compatibility name for the read-only MAM
Medium Serial Number `0x0401`; the application Volume Identifier `0x0008` is
not a physical-continuity identity and neither attribute is written. If a
recognized, otherwise complete LTFS cartridge has no application barcode,
pre-format reports `media_state=unidentified` and sets the barcode and all LTFS
fields to JSON null. A complete LTFS identity emits `media_state=ltfs`.
Non-LTFS or absent application markers may also produce the closed partial
record. Every successful pre-format record requires a nonempty `0x0401` value;
normal `unmounted` inspection remains strict and any other partial LTFS evidence
fails closed. The exact ten-key output contract is described by
`schemas/ltfs-info-v2.schema.json`. The private RHEL packaging candidate is
`lto-ltfs-0.1.1-22.el9`; it has not been built, signed, published, or deployed.
This distinct candidate corrects the full upstream CI fixture and public runtime
dependency closure. The earlier `v0.1.0` source tag and candidate remain fixed;
new public source transfer, signing, qualification and publication are pending.
The public driver project is [Halino/lto-ltfs-driver](https://github.com/Halino/lto-ltfs-driver).
The paired application is maintained separately on its
[Linux branch](https://github.com/Halino/lto-archiver/tree/linux).
Release22 changes license/notice packaging only. The signed release21 remains
unchanged. Release21 corrects
[SG response bounds and unaligned endian access](docs/qualification/sg-lbp-response-bounds.md).
Release20 corrected the
[variable-length MAM Volume Identifier](docs/qualification/mam-volume-identifier.md).
Release19 added
[native read-only session enforcement](docs/qualification/read-only-session.md).
Release18 introduced the explicitly selected
[read-only capacity diagnostic](docs/qualification/read-only-capacity.md).
Release20's read-only MAM check passed on the HP LTO-6 qualification drive.
The release21 candidate is not yet physically qualified, and preparation does not authorize
tape writes, formatting, mounts, ejects or backup resume.
For HP LTO-6, the Linux SG backend retains release17's selection of the
MiB-valued Tape Capacity page, as it already does for LTO-5. Other drive-family
selection and Volume Statistics units are unchanged. On the Volume Statistics
path, the backend accepts the standard remaining
capacity parameter `0x0204`; when that parameter is absent, it derives
remaining capacity from native maximum `0x0202` minus used capacity `0x0203`.
Both paths validate the complete response before returning capacity: page31
requires the four unique MiB-valued capacity fields, while page17 validates
one- or two-partition records. Command-aware sequential sense handling keeps
recognized READ ILI, filemark, end-of-data, record-not-found, and cleaning
outcomes, plus WRITE/WRITE FILEMARKS early-warning, progressive-early-warning,
and cleaning outcomes, from being replaced by ambiguity errors. Unqualified
READ no-sense and ILI on non-READ commands remain fail-closed as ambiguous.
Legacy nested SG aliases are rejected.
Passing device paths directly to normal `ltfs-info` is rejected; discovery is
never a silent admission bypass. A broker-provided `/proc/self/fd/N`
generic-SCSI anchor is accepted only after it is duplicated and proven to be
the exact configured SG device; LTFS continues through its private duplicate
while the paired non-rewinding device remains in the serial, WWID, and SCSI
tuple identity fence.
The configuration is host-only and must never be committed or copied into a
WebUI container.

Package installation and hermetic fake-backend checks are not physical-tape
qualification. Device access, mounts, and media operations require the later
separately authorized qualification plan.

Release17 introduced the required NUL in the VCI application signature and
preserved validated nonempty operation options during initial parsing without
false invalid-option diagnostics (G01); release18 retains both behaviors.
These changes have no physical-tape validation on this candidate. Earlier-driver
capacity or sampled-readback results do not qualify release18 or establish
cross-implementation interoperability. Release17's historical evidence is not
rewritten or promoted into physical qualification by this candidate.
Capacity counters are diagnostics, not a universal writable-payload guarantee;
this release establishes no universal reserve. Any capacity claim requires its
own workload, finalization, and readback evidence.

The checked export inventory in `qualification/ltfs-surface-v1.json` uses
schema 2 and distinguishes two probes. `source-call` means the named C or
Python test function contains a direct call to that exact exported symbol;
`abi-export` means only that the symbol is present in the built shared library.
An ABI-only entry has no test evidence and is not functional coverage. The
surface verifier reports both counts separately and rejects evidence that names
an existing test which does not call the symbol. It also enforces closed risk
classes for the known format, unformat, eject, reset-capacity, tape-attribute,
encryption-key, and xattr mutators.

The current manifest deliberately leaves high-level destructive exports such
as `ltfs_format_tape`, `ltfs_unformat_tape`, and `ltfs_eject_tape` ABI-only.
Hardware-free tests directly call lower tape-command boundaries, but that does
not substitute for the separately authorized physical qualification.

Run the closed hardware-free gate with:

```sh
scripts/run-complete-qualification.sh \
  --source-root "$PWD" --build-root "$PWD" \
  --output "$PWD/complete-qualification.json"
```

The gate records ABI enumeration and direct source-call evidence as separate
counts, then runs the unit, fake/file-backend destructive, ASan/UBSan, and
fresh gcov-instrumented phases. It stops at the first failure. The file-backend
phase is synthetic and the command never opens or names a tape device.

#### Parameter settings of the sg driver

LTFS uses the sg driver by default. You can improve reliability to change parameters of the sg driver below.

```
def_reserved_size=1048576
```

In RHEL7, you can put following file as `/etc/modprobe.d/sg.conf`.

```
options sg def_reserved_size=1048576
```

You can check current configuration of sg driver to see the file `/proc/scsi/sg/debug` like

```
$ cat /proc/scsi/sg/debug
max_active_device=44  def_reserved_size=32768
 >>> device=sg25 1:0:10:0   em=0 sg_tablesize=1024 excl=0 open_cnt=1
   FD(1): timeout=60000ms bufflen=524288 (res)sgat=16 low_dma=0
   cmd_q=1 f_packid=0 k_orphan=0 closed=0
     No requests active
 >>> device=sg26 1:0:10:1   em=0 sg_tablesize=1024 excl=0 open_cnt=1
   FD(1): timeout=60000ms bufflen=524288 (res)sgat=16 low_dma=0
   cmd_q=1 f_packid=0 k_orphan=0 closed=0
     No requests active
```

##### Performance improvement of the sg device

You can improve performance to change parameters of the sg driver below. But this option may cause I/O error reported as #144 in some HBAs.

```
allow_dio=1
```

In RHEL7, you can put following file as `/etc/modprobe.d/sg.conf`.

```
options sg allow_dio=1
```

At this time, we know following HBA's works correctly.

- QLogic 8Gb FC HBAs

And following HBA's doesn't work correctly.

- ATTO ExpressSAS H680
- Emulex FC HBAs (Some drivers work but some drivers dont work, see this [section](#note-for-the-lpfc-driver-emulex-fibre-hbas))

##### Note for the lpfc driver (Emulex Fibre HBAs)

In the lpfc driver (for Emulex Fibre HBAs), the table size of the scatter-gather is 64 by default. This configuration may cause I/O errors intermittently when `allow_dio=1` is set and scatter-gather table cannot be reserved. To avoid this error, you need to change the parameter `lpfc_sg_seg_cnt` to 256 or greater like below.

```
options lpfc lpfc_sg_seg_cnt=256
```

In some versions of the lpfc driver (for Emulex Fibre HBAs), the table size of the scatter-gather cannot be changed correctly. You can check the value is changed or not in `sg_tablesize` value in `/proc/scsi/sg/debug`. If you don't have a correct value (256 or greater) in `sg_tablesize`, removing `allow_dio=1` configuration of the sg driver is strongly recommended.

##### Note for buggy HBAs

LTFS doesn't support the HBAs which doesn't handle the transfer length of SCSI data by default. The reason is because the safety of the data but LTFS provides a option to relax this limitation.

You can use such kind of HBAs if you run the `configure` script with `--enable-buggy-ifs` option and build.

List of the HBA `--enable-buggy-ifs` is needed is below.

[HBA list require `--enable-buggy-ifs`](https://github.com/LinearTapeFileSystem/ltfs/wiki/HBA-info)

#### IBM lin_tape driver support

You need to add `--enable-lintape` as an argument of ./configure script if you want to build the backend for lin_tape. You also need to add `DEFAULT_TAPE=lin_tape` if you set the lin_tape backend as default backend.

#### Buildable distributions

  | Dist                               | Arch    | Status                                                                                                                           |
  |:----------------------------------:|:-------:|:--------------------------------------------------------------------------------------------------------------------------------:|
  | RHEL 8                             | x86\_64 | OK - Not checked automatically                                                                                                   |
  | RHEL 8                             | ppc64le | OK - Not checked automatically                                                                                                   |
  | CentOS 8 (Rocky Linux)             | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-centos8.yml/badge.svg)        |
  | CentOS 8 (Rocky Linux)             | ppc64le | OK - Not checked automatically                                                                                                   |
  | Fedora 28                          | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-fedora28.yml/badge.svg)       |
  | Ubuntu 16.04 LTS                   | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-ubuntu-xeneal.yml/badge.svg) |
  | Ubuntu 16.04 LTS                   | ppc64le | OK - Not checked automatically                                                                                                   |
  | Ubuntu 18.04 LTS                   | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-ubuntu-bionic.yml/badge.svg) |
  | Ubuntu 18.04 LTS                   | ppc64le | OK - Not checked automatically                                                                                                   |
  | Ubuntu 20.04 LTS (Need icu-config) | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-ubuntu-focal.yml/badge.svg) |
  | Debian 9                           | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-debian9.yml/badge.svg)        |
  | Debian 10 (Need icu-config)        | x86\_64 | ![GH Action status](https://github.com/LinearTapeFileSystem/ltfs/actions/workflows/build-debian10.yml/badge.svg)       |
  | ArchLinux 2018.08.01               | x86\_64 | OK - Not checked automatically                                                                                                   |
  | ArchLinux 2018.12.31 (rolling)     | x86\_64 | OK - Not checked automatically                                                                                                   |

Currently, automatic build checking is working on GitHub Actions and Travis CI.

For Ubuntu20.04 and Debian10, dummy `icu-config` is needed in the build machine. See Issue [#153](https://github.com/LinearTapeFileSystem/ltfs/issues/153).

### Build and install on OSX (macOS)

#### Recent Homedrew system setup

Before build on macOS, you need to configure the environment like below.

```
export ICU_PATH="/usr/local/opt/icu4c/bin"
export LIBXML2_PATH="/usr/local/opt/libxml2/bin"
export PKG_CONFIG_PATH="/usr/local/opt/icu4c/lib/pkgconfig:/usr/local/opt/libxml2/lib/pkgconfig"
export PATH="$PATH:$ICU_PATH:$LIBXML2_PATH"
```

#### Old Homedrew system setup
Before build on OSX (macOS), some include path adjustment is required.

```
brew link --force icu4c
brew link --force libxml2
```

#### Building LTFS
On OSX (macOS), snmp cannot be supported, you need to disable it on configure script. And may be, you need to specify LDFLAGS while running configure script to link some required frameworks, CoreFundation and IOKit.

```
./autogen.sh
LDFLAGS="-framework CoreFoundation -framework IOKit" ./configure --disable-snmp
make
make install
```

`./configure --help` shows various options for build and install.

#### Buildable systems

  | OS            | Xcode  | Package system | Status      |
  |:-:            |:-:     |:-:             |:-:          |
  | macOS 10.14.6 | 11.3   | Homebrew       | Probably OK |

### Build and install on FreeBSD

Note that on FreeBSD, the usual 3rd party man directory is /usr/local/man. Configure defaults to using /usr/local/share/man.  So, override it on the command line to avoid having man pages put in the wrong place.

```
./autogen.sh
./configure --prefix=/usr/local --mandir=/usr/local/man
make
make install
```

#### Buildable versions

  | Version | Arch    | Status      |
  |:-:      |:-:      |:-:          |
  | 11      | x86_64  | OK          |

### Build and install on NetBSD

```
./autogen.sh
./configure
make
make install
```

#### Buildable versions

  | Version | Arch    | Status      |
  |:-:      |:-:      |:-:          |
  | 8.1     | amd64   | OK          |
  | 8.0     | i386    | OK          |
  | 7.2     | amd64   | OK          |

## Contributing

Please read [CONTRIBUTING.md](.github/CONTRIBUTING.md) for details on our code of conduct, and the process for submitting pull requests to us.

## License

The IBM-derived LTFS code carries the BSD-3-Clause terms in [LICENSE](LICENSE).
Vendored uthash components carry a separate BSD-1-Clause notice in
[NOTICES](NOTICES). This downstream candidate also includes the
[LGPL 2.1-only text](COPYING.LIB) and a [conservative HPE notice](LGPL-NOTICE)
for the downstream components identified in
[the source inventory](provenance/license-inventory.json). The package license
expression retains all three obligations. Seven components use an approved
conservative licensing policy while their exact HPE derivation remains
unverified; the mechanical inventory check is not a legal or origin opinion.
The candidate is not yet cleared for public distribution or deployment.
