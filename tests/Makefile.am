# SPDX-License-Identifier: BSD-3-Clause

check_PROGRAMS =
TESTS =
TEST_EXTENSIONS = .py
PY_LOG_COMPILER = $(PYTHON)

if ENABLE_TESTS
check_PROGRAMS += c/test_version test_ltfs_endian test_events test_operation_control \
	test_device_identity test_operation_lock test_retry_policy test_finalization \
	test_tape_close test_tape_destructive test_sg_destructive_cdb test_sg_capacity \
	test_ltfs_info test_ltfs_info_transport test_perf_counters test_perf_reporter \
	test_streaming_stats test_standalone_receipt test_fsync_receipt \
	test_arch_open test_fcfs_flush
TESTS += c/test_version test_ltfs_endian test_events test_operation_control \
	test_device_identity test_operation_lock test_retry_policy test_finalization \
	test_tape_close test_tape_destructive test_sg_destructive_cdb test_sg_capacity \
	test_ltfs_info test_ltfs_info_transport test_perf_counters test_perf_reporter \
	test_streaming_stats test_standalone_receipt test_fsync_receipt \
	test_arch_open test_fcfs_flush \
	python/test_cli.py \
	python/test_cli_surface.py \
	python/test_complete_qualification.py \
	python/test_file_destructive.py \
	python/test_test_surface.py \
	python/test_containerfile.py python/test_copyright_symbols.py \
	python/test_event_schema.py python/test_retry_policy_build.py \
	python/test_ltfs_info.py python/test_bench.py \
	python/test_sg_lbp_response_bounds.py \
	python/test_streaming_telemetry_build.py \
	python/test_sanitizer_qualification.py \
	python/test_vendor.py python/test_finalization_install.py
endif

test_ltfs_endian_SOURCES = c/test_ltfs_endian.c c/test.h \
	../src/libltfs/ltfs_endian.h

test_arch_open_SOURCES = c/test_arch_open.c c/test.h
test_arch_open_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src

test_fcfs_flush_SOURCES = c/test_fcfs_flush.c c/test.h \
	c/fake-fcfs/ltfs_copyright.h \
	c/fake-fcfs/libltfs/iosched_ops.h \
	c/fake-fcfs/libltfs/ltfs.h \
	c/fake-fcfs/libltfs/ltfs_fsops_raw.h \
	../src/iosched/fcfs.c \
	../src/libltfs/finalization.c \
	../src/libltfs/finalization.h
test_fcfs_flush_CPPFLAGS = \
	-I$(top_srcdir)/tests/c/fake-fcfs \
	$(AM_CPPFLAGS) \
	-I$(top_srcdir)/src \
	-D_POSIX_C_SOURCE=200809L
test_fcfs_flush_CFLAGS = $(AM_CFLAGS) -Wno-unused-parameter \
	-ffunction-sections -fdata-sections
test_fcfs_flush_LDFLAGS = -Wl,--gc-sections

test_perf_counters_SOURCES = \
	c/test_perf_counters.c \
	c/test.h \
	../src/libltfs/perf_counters.c \
	../src/libltfs/perf_counters.h
test_perf_counters_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs

test_perf_reporter_SOURCES = \
	c/test_perf_reporter.c \
	c/test.h \
	../src/libltfs/perf_reporter.c \
	../src/libltfs/perf_reporter.h \
	../src/libltfs/perf_counters.c \
	../src/libltfs/perf_counters.h \
	../src/libltfs/ltfs_events.c \
	../src/libltfs/ltfs_events.h
test_perf_reporter_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_perf_reporter_LDADD = -lpthread

test_streaming_stats_SOURCES = c/test_streaming_stats.c c/test.h \
	../src/libltfs/streaming_stats.c ../src/libltfs/streaming_stats.h
test_streaming_stats_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs

test_standalone_receipt_SOURCES = c/test_standalone_receipt.c c/test.h \
	../src/libltfs/standalone_receipt.c ../src/libltfs/standalone_receipt.h \
	../src/libltfs/finalization.h
test_standalone_receipt_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs

test_fsync_receipt_SOURCES = c/test_fsync_receipt.c c/test.h \
	../src/libltfs/file_write_state.c ../src/libltfs/file_write_state.h \
	../src/libltfs/perf_counters.c ../src/libltfs/perf_counters.h \
	../src/libltfs/standalone_receipt.c ../src/libltfs/standalone_receipt.h \
	../src/libltfs/finalization.h
test_fsync_receipt_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_fsync_receipt_LDADD = -lpthread

c_test_version_SOURCES = c/test_version.c c/test.h
test_events_SOURCES = \
	c/test_events.c \
	c/test.h \
	../src/libltfs/ltfs_events.c \
	../src/libltfs/ltfs_events.h
test_events_CPPFLAGS = \
	-I$(top_srcdir)/src/libltfs \
	-DLTFS_EVENTS_WRITE=test_event_write
test_events_LDADD = -lpthread
test_events_LDFLAGS = -Wl,--wrap=realloc

test_operation_control_SOURCES = \
	c/test_operation_control.c \
	c/test.h \
	../src/libltfs/operation_control.c \
	../src/libltfs/operation_control.h
test_operation_control_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_operation_control_LDADD = -lpthread

test_device_identity_SOURCES = \
	c/test_device_identity.c \
	c/test.h \
	../src/libltfs/device_guard.c \
	../src/libltfs/device_guard.h \
	../src/libltfs/device_identity.c \
	../src/libltfs/device_identity.h \
	../src/libltfs/operation_lock.c \
	../src/libltfs/operation_lock.h
test_device_identity_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_device_identity_LDADD = -lpthread -lcrypto

test_operation_lock_SOURCES = \
	c/test_operation_lock.c \
	c/test.h \
	c/fake-fuse/fuse.h \
	../src/libltfs/fuse_foreground.c \
	../src/libltfs/fuse_foreground.h \
	../src/libltfs/operation_lock.c \
	../src/libltfs/operation_lock.h \
	../src/libltfs/device_identity.h
test_operation_lock_CPPFLAGS = $(AM_CPPFLAGS) \
	-I$(top_srcdir)/tests/c/fake-fuse \
	-I$(top_srcdir)/src/libltfs
test_operation_lock_LDADD = -lpthread

test_retry_policy_SOURCES = \
	c/test_retry_policy.c \
	c/test_retry_clock.c \
	c/test.h \
	../src/libltfs/retry_policy.c \
	../src/libltfs/retry_policy.h \
	../src/tape_drivers/linux/sg/sg_scsi_tape.c \
	../src/tape_drivers/linux/sg/sg_scsi_tape.h
test_retry_policy_CPPFLAGS = $(AM_CPPFLAGS) \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/libltfs \
	-DLTFS_SG_TESTING

test_finalization_SOURCES = \
	c/test_finalization.c \
	c/test.h \
	../src/libltfs/finalization.c \
	../src/libltfs/finalization.h
test_finalization_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs

test_tape_close_SOURCES = c/test_tape_close.c c/test.h
test_tape_close_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_tape_close_LDADD = ../src/libltfs/libltfs.la

test_tape_destructive_SOURCES = c/test_tape_destructive.c c/test.h
test_tape_destructive_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_tape_destructive_CFLAGS = $(AM_CFLAGS) -Werror \
	-Wno-deprecated-declarations
test_tape_destructive_LDADD = ../src/libltfs/libltfs.la

test_sg_destructive_cdb_SOURCES = c/test_sg_destructive_cdb.c c/test.h
test_sg_destructive_cdb_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src/libltfs
test_sg_destructive_cdb_CFLAGS = $(AM_CFLAGS) -Werror \
	-Wno-deprecated-declarations -fno-keep-inline-functions

test_sg_capacity_SOURCES = \
	c/test_sg_capacity.c \
	c/test.h \
	../src/tape_drivers/linux/sg/sg_capacity.c \
	../src/tape_drivers/linux/sg/sg_capacity.h
test_sg_capacity_CPPFLAGS = $(AM_CPPFLAGS) -I$(top_srcdir)/src
test_sg_capacity_CFLAGS = $(AM_CFLAGS) -Werror -fno-keep-inline-functions

test_ltfs_info_SOURCES = \
	c/test_ltfs_info.c \
	c/test.h \
	../src/utils/ltfs_info.c \
	../src/utils/ltfs_info.h \
	../src/tape_drivers/linux/sg/sg_capacity.c \
	../src/tape_drivers/linux/sg/sg_capacity.h
test_ltfs_info_CPPFLAGS = $(AM_CPPFLAGS) \
	-I$(top_srcdir)/src \
	-I$(top_srcdir)/src/libltfs \
	-I$(top_srcdir)/src/utils \
	-DLTFS_INFO_NO_MAIN \
	-DLTFS_INFO_TEST_CLI
test_ltfs_info_LDADD = ../src/libltfs/libltfs.la

test_ltfs_info_transport_SOURCES = c/test_ltfs_info_transport.c c/test.h \
	../src/tape_drivers/linux/sg/sg_capacity.c \
	../src/tape_drivers/linux/sg/sg_capacity.h
test_ltfs_info_transport_CPPFLAGS = $(AM_CPPFLAGS) \
	-I$(top_srcdir)/src -I$(top_srcdir)/src/libltfs -I$(top_srcdir)/src/utils
test_ltfs_info_transport_CFLAGS = $(AM_CFLAGS) -ffunction-sections -fdata-sections
test_ltfs_info_transport_LDFLAGS = -Wl,--gc-sections
test_ltfs_info_transport_LDADD = ../src/libltfs/libltfs.la

TESTS_ENVIRONMENT = \
	PYTHONPATH="$(abs_top_srcdir)$${PYTHONPATH:+:$${PYTHONPATH}}" \
	LTFS_BINARY="$(abs_top_builddir)/src/ltfs" \
	MKLTFS_BINARY="$(abs_top_builddir)/src/utils/mkltfs" \
	LTFSCK_BINARY="$(abs_top_builddir)/src/utils/ltfsck" \
	LTFS_INFO_CLI_BINARY="$(abs_top_builddir)/src/utils/ltfs-info" \
	LTFS_BUILD_DIR="$(abs_top_builddir)" \
	LTFS_SOURCE_DIR="$(abs_top_srcdir)" \
	LTFS_EVENT_FIXTURE_BINARY="$(abs_top_builddir)/tests/test_events" \
	LTFS_INFO_BINARY="$(abs_top_builddir)/tests/test_ltfs_info"

EXTRA_DIST = \
	../schemas/ltfs-event-v1.schema.json \
	../schemas/ltfs-info-v1.schema.json \
	c/config_tokenizer_oracle.c \
	python/test_containerfile.py \
	python/test_bench.py \
	python/test_copyright_symbols.py \
	python/test_cli.py \
	python/test_cli_surface.py \
	python/test_complete_qualification.py \
	python/test_file_destructive.py \
	python/test_test_surface.py \
	python/test_event_schema.py \
	python/test_ltfs_info.py \
	python/test_sg_lbp_response_bounds.py \
	python/test_retry_policy_build.py \
	python/test_finalization_install.py \
	python/test_provenance.py \
	python/test_sanitizer_qualification.py \
	python/test_streaming_telemetry_build.py \
	python/test_vendor.py
