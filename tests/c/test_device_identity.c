/* SPDX-License-Identifier: BSD-3-Clause */

#include "test.h"
#include "device_guard.h"
#include "device_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct identity_fixture {
	char root[PATH_MAX];
	char dev_root[PATH_MAX];
	char sysfs_root[PATH_MAX];
	char proc_root[PATH_MAX];
	char lock_root[PATH_MAX];
	char nst_alias[PATH_MAX];
	char nst_alias_two[PATH_MAX];
	char sg_alias[PATH_MAX];
	char nst_node[PATH_MAX];
	char sg_node[PATH_MAX];
	char scsi_device[PATH_MAX];
	char vpd80_path[PATH_MAX];
	char wwid_path[PATH_MAX];
	char sg_device_link[PATH_MAX];
};

struct resolve_context {
	const struct ltfs_device_target *target;
	struct ltfs_device_identity identity;
	int result;
};

static int join_path(char *output, size_t size, const char *left,
	const char *right)
{
	char saved_left[PATH_MAX];
	int length;
	if (strlen(left) >= sizeof(saved_left))
		return -1;
	strcpy(saved_left, left);
	length = snprintf(output, size, "%s/%s", saved_left, right);
	return length >= 0 && length < (int)size ? 0 : -1;
}

static int make_directory(const char *path)
{
	if (mkdir(path, 0700) == 0 || errno == EEXIST)
		return 0;
	perror(path);
	return -1;
}

static int write_text(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t length = strlen(text);
	ssize_t written;
	if (fd < 0)
		return -1;
	written = write(fd, text, length);
	if (close(fd) < 0)
		return -1;
	return written == (ssize_t)length ? 0 : -1;
}

static int write_bytes(const char *path, const unsigned char *bytes,
	size_t length)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	size_t offset = 0;
	if (fd < 0)
		return -1;
	while (offset < length) {
		ssize_t written = write(fd, bytes + offset, length - offset);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			close(fd);
			return -1;
		}
		offset += (size_t)written;
	}
	return close(fd);
}

static int write_vpd80(const char *path, const char *payload)
{
	unsigned char page[4 + LTFS_DEVICE_IDENTITY_TEXT_MAX];
	size_t length = strlen(payload);
	if (length == 0 || length >= LTFS_DEVICE_IDENTITY_TEXT_MAX)
		return -1;
	page[0] = 0x01;
	page[1] = 0x80;
	page[2] = (unsigned char)(length >> 8);
	page[3] = (unsigned char)length;
	memcpy(page + 4, payload, length);
	return write_bytes(path, page, length + 4);
}

static int make_sysfs_class_device(const struct identity_fixture *fixture,
	const char *device_number, const char *class_name, const char *device_name,
	const char *scsi_device, char *device_link, size_t device_link_size)
{
	char class_root[PATH_MAX];
	char class_directory[PATH_MAX];
	char class_group[PATH_MAX];
	char class_node[PATH_MAX];
	char path[PATH_MAX];

	if (join_path(class_root, sizeof(class_root), fixture->sysfs_root,
		"class") < 0 || make_directory(class_root) < 0 ||
		join_path(class_directory, sizeof(class_directory), class_root,
		class_name) < 0 || make_directory(class_directory) < 0 ||
		join_path(class_group, sizeof(class_group), scsi_device,
		class_name) < 0 || make_directory(class_group) < 0 ||
		join_path(class_node, sizeof(class_node), class_group,
		device_name) < 0 || make_directory(class_node) < 0 ||
		join_path(device_link, device_link_size, class_node, "device") < 0 ||
		symlink(scsi_device, device_link) < 0 ||
		join_path(path, sizeof(path), class_node, "subsystem") < 0 ||
		symlink(class_directory, path) < 0 ||
		join_path(path, sizeof(path), fixture->sysfs_root,
		"dev/char") < 0 ||
		join_path(path, sizeof(path), path, device_number) < 0 ||
		symlink(class_node, path) < 0)
		return -1;
	return 0;
}

static int fixture_init(struct identity_fixture *fixture)
{
	char template[] = "/tmp/lto-ltfs-identity.XXXXXX";
	char path[PATH_MAX];
	char proc_sys[PATH_MAX];
	char proc_kernel[PATH_MAX];
	char proc_random[PATH_MAX];
	char sysfs_devices[PATH_MAX];
	char unused_device_link[PATH_MAX];
	char *root = mkdtemp(template);
	if (!root)
		return -1;
	memset(fixture, 0, sizeof(*fixture));
	strncpy(fixture->root, root, sizeof(fixture->root) - 1);
	if (join_path(fixture->dev_root, sizeof(fixture->dev_root), root, "dev") < 0 ||
		join_path(fixture->sysfs_root, sizeof(fixture->sysfs_root), root, "sys") < 0 ||
		join_path(fixture->proc_root, sizeof(fixture->proc_root), root, "proc") < 0 ||
		join_path(fixture->lock_root, sizeof(fixture->lock_root), root, "locks") < 0)
		return -1;
	if (make_directory(fixture->dev_root) < 0 ||
		make_directory(fixture->sysfs_root) < 0 ||
		make_directory(fixture->proc_root) < 0 ||
		make_directory(fixture->lock_root) < 0)
		return -1;
	if (join_path(path, sizeof(path), fixture->sysfs_root, "dev") < 0 ||
		make_directory(path) < 0 ||
		join_path(path, sizeof(path), path, "char") < 0 ||
		make_directory(path) < 0)
		return -1;
	if (join_path(sysfs_devices, sizeof(sysfs_devices), fixture->sysfs_root,
		"devices") < 0 || make_directory(sysfs_devices) < 0 ||
		join_path(fixture->scsi_device, sizeof(fixture->scsi_device),
		sysfs_devices, "6:0:1:0") < 0 ||
		make_directory(fixture->scsi_device) < 0 ||
		join_path(fixture->vpd80_path, sizeof(fixture->vpd80_path),
		fixture->scsi_device, "vpd_pg80") < 0 ||
		write_vpd80(fixture->vpd80_path, "  SERIAL-001  ") < 0 ||
		join_path(fixture->wwid_path, sizeof(fixture->wwid_path),
		fixture->scsi_device, "wwid") < 0 ||
		write_text(fixture->wwid_path, "naa.6001\n") < 0)
		return -1;
	if (join_path(proc_sys, sizeof(proc_sys), fixture->proc_root, "sys") < 0 ||
		make_directory(proc_sys) < 0 ||
		join_path(proc_kernel, sizeof(proc_kernel), proc_sys, "kernel") < 0 ||
		make_directory(proc_kernel) < 0 ||
		join_path(proc_random, sizeof(proc_random), proc_kernel, "random") < 0 ||
		make_directory(proc_random) < 0 ||
		join_path(path, sizeof(path), proc_random, "boot_id") < 0 ||
		write_text(path, "11111111-2222-4333-8444-555555555555\n") < 0)
		return -1;

	if (join_path(fixture->nst_node, sizeof(fixture->nst_node),
		fixture->dev_root, "nst0") < 0 ||
		join_path(fixture->sg_node, sizeof(fixture->sg_node),
		fixture->dev_root, "sg0") < 0 ||
		symlink("/dev/null", fixture->nst_node) < 0 ||
		symlink("/dev/zero", fixture->sg_node) < 0)
		return -1;
	if (join_path(fixture->nst_alias, sizeof(fixture->nst_alias),
		fixture->dev_root, "tape-by-id-a") < 0 ||
		join_path(fixture->nst_alias_two, sizeof(fixture->nst_alias_two),
		fixture->dev_root, "tape-by-id-b") < 0 ||
		join_path(fixture->sg_alias, sizeof(fixture->sg_alias),
		fixture->dev_root, "sg-by-id-a") < 0 ||
		symlink("nst0", fixture->nst_alias) < 0 ||
		symlink("nst0", fixture->nst_alias_two) < 0 ||
		symlink("sg0", fixture->sg_alias) < 0)
		return -1;
	if (make_sysfs_class_device(fixture, "1:3", "scsi_tape", "nst0",
		fixture->scsi_device, unused_device_link,
		sizeof(unused_device_link)) < 0 ||
		make_sysfs_class_device(fixture, "1:5", "scsi_generic", "sg0",
		fixture->scsi_device, fixture->sg_device_link,
		sizeof(fixture->sg_device_link)) < 0)
		return -1;
	return 0;
}

static struct ltfs_device_target fixture_target(
	const struct identity_fixture *fixture)
{
	struct ltfs_device_target target;
	memset(&target, 0, sizeof(target));
	target.nst_path = fixture->nst_alias;
	target.sg_path = fixture->sg_alias;
	target.expected_serial = "SERIAL-001";
	target.expected_wwid = "naa.6001";
	target.device_root = fixture->dev_root;
	target.sysfs_root = fixture->sysfs_root;
	target.proc_root = fixture->proc_root;
	target.lock_root = fixture->lock_root;
	return target;
}

static void *resolve_identity(void *opaque)
{
	struct resolve_context *context = opaque;
	context->result = ltfs_device_identity_resolve(context->target,
		&context->identity);
	return NULL;
}

static int test_matching_identity_and_canonical_aliases(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity first;
	struct ltfs_device_identity second;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	CHECK_INT_EQ(ltfs_device_identity_resolve(&target, &first), 0);
	CHECK_STR_EQ(first.nst_path, fixture.nst_node);
	CHECK_STR_EQ(first.sg_path, fixture.sg_node);
	CHECK_STR_EQ(first.serial, "SERIAL-001");
	CHECK_STR_EQ(first.wwid, "naa.6001");
	CHECK_STR_EQ(first.scsi_tuple, "6:0:1:0");
	CHECK_STR_EQ(first.boot_id, "11111111-2222-4333-8444-555555555555");
	CHECK_INT_EQ(strlen(first.target_sha256), 64);
	target.nst_path = fixture.nst_alias_two;
	CHECK_INT_EQ(ltfs_device_identity_resolve(&target, &second), 0);
	CHECK_STR_EQ(first.target_sha256, second.target_sha256);
	target.device_root = fixture.lock_root;
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &second) < 0);
	return 0;
}

static int test_rejects_rewinding_and_non_character_paths(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity identity;
	char path[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	CHECK_INT_EQ(join_path(path, sizeof(path), fixture.dev_root, "st0"), 0);
	CHECK_INT_EQ(symlink("/dev/null", path), 0);
	target.nst_path = path;
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(unlink(path), 0);
	CHECK_INT_EQ(write_text(path, "not a device"), 0);
	target.nst_path = path;
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	return 0;
}

static int test_rejects_serial_wwid_and_tuple_mismatch(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity identity;
	char alternate[PATH_MAX];
	char devices[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	target.expected_serial = "OTHER";
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	target.expected_serial = "SERIAL-001";
	target.expected_wwid = "naa.other";
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	target.expected_wwid = "naa.6001";
	CHECK_INT_EQ(join_path(devices, sizeof(devices), fixture.sysfs_root,
		"devices"), 0);
	CHECK_INT_EQ(join_path(alternate, sizeof(alternate), devices,
		"7:0:1:0"), 0);
	CHECK_INT_EQ(make_directory(alternate), 0);
	CHECK_INT_EQ(unlink(fixture.sg_device_link), 0);
	CHECK_INT_EQ(symlink(alternate, fixture.sg_device_link), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	return 0;
}

static int test_rejects_malformed_vpd80_and_wwid(void)
{
	static const unsigned char wrong_page[] = { 0x01, 0x83, 0x00, 0x01, 'A' };
	static const unsigned char wrong_peripheral[] = {
		0xe1, 0x80, 0x00, 0x0a,
		'S', 'E', 'R', 'I', 'A', 'L', '-', '0', '0', '1'
	};
	static const unsigned char truncated[] = { 0x01, 0x80, 0x00, 0x04, 'A' };
	static const unsigned char control[] = {
		0x01, 0x80, 0x00, 0x03, 'A', '\n', 'B'
	};
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity identity;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	target.expected_serial = NULL;
	target.expected_wwid = NULL;
	CHECK_INT_EQ(write_bytes(fixture.vpd80_path, wrong_page,
		sizeof(wrong_page)), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_bytes(fixture.vpd80_path, wrong_peripheral,
		sizeof(wrong_peripheral)), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_bytes(fixture.vpd80_path, truncated,
		sizeof(truncated)), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_bytes(fixture.vpd80_path, control,
		sizeof(control)), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_vpd80(fixture.vpd80_path, "  SERIAL-001  "), 0);
	CHECK_INT_EQ(write_text(fixture.wwid_path, "naa.not-hex\n"), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_text(fixture.wwid_path, "naa.bad value\n"), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(unlink(fixture.wwid_path), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	return 0;
}

static int test_rejects_missing_boot_id(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity identity;
	char path[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	CHECK_INT_EQ(join_path(path, sizeof(path), fixture.proc_root,
		"sys/kernel/random/boot_id"), 0);
	CHECK_INT_EQ(unlink(path), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	CHECK_INT_EQ(write_text(path, "11111111-2222-4333-8444-555555555555X"), 0);
	CHECK_TRUE(ltfs_device_identity_resolve(&target, &identity) < 0);
	return 0;
}

static int test_identity_diagnostic_reports_exact_failure_phase(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct ltfs_device_identity identity;
	struct ltfs_device_identity_diagnostic diagnostic;
	char boot_path[PATH_MAX];
	char sysfs_char_path[PATH_MAX];

	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	CHECK_INT_EQ(join_path(sysfs_char_path, sizeof(sysfs_char_path),
		fixture.sysfs_root, "dev/char"), 0);
	CHECK_INT_EQ(chmod(sysfs_char_path, 0000), 0);
	CHECK_INT_EQ(ltfs_device_identity_resolve_diagnostic(&target, &identity,
		&diagnostic), -EACCES);
	CHECK_INT_EQ(diagnostic.phase,
		LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_CLASS);
	CHECK_INT_EQ(diagnostic.error, EACCES);
	CHECK_STR_EQ(ltfs_device_identity_phase_name(diagnostic.phase),
		"nst-sysfs-class");
	CHECK_INT_EQ(chmod(sysfs_char_path, 0700), 0);

	CHECK_INT_EQ(chmod(fixture.vpd80_path, 0000), 0);
	CHECK_INT_EQ(ltfs_device_identity_resolve_diagnostic(&target, &identity,
		&diagnostic), -EACCES);
	CHECK_INT_EQ(diagnostic.phase,
		LTFS_DEVICE_IDENTITY_PHASE_NST_SYSFS_SERIAL);
	CHECK_INT_EQ(diagnostic.error, EACCES);
	CHECK_STR_EQ(ltfs_device_identity_phase_name(diagnostic.phase),
		"nst-sysfs-serial");
	CHECK_INT_EQ(chmod(fixture.vpd80_path, 0600), 0);

	target.expected_serial = "OTHER";
	CHECK_INT_EQ(ltfs_device_identity_resolve_diagnostic(&target, &identity,
		&diagnostic), -EXDEV);
	CHECK_INT_EQ(diagnostic.phase,
		LTFS_DEVICE_IDENTITY_PHASE_EXPECTED_SERIAL);
	CHECK_INT_EQ(diagnostic.error, EXDEV);
	target.expected_serial = "SERIAL-001";

	CHECK_INT_EQ(join_path(boot_path, sizeof(boot_path), fixture.proc_root,
		"sys/kernel/random/boot_id"), 0);
	CHECK_INT_EQ(unlink(boot_path), 0);
	CHECK_INT_EQ(ltfs_device_identity_resolve_diagnostic(&target, &identity,
		&diagnostic), -ENOENT);
	CHECK_INT_EQ(diagnostic.phase, LTFS_DEVICE_IDENTITY_PHASE_BOOT_ID);
	CHECK_INT_EQ(diagnostic.error, ENOENT);
	return 0;
}

static int test_rejects_symlink_swap_during_resolution(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_target target;
	struct resolve_context context;
	pthread_t resolver;
	static const unsigned char vpd[] = {
		0x01, 0x80, 0x00, 0x0a,
		'S', 'E', 'R', 'I', 'A', 'L', '-', '0', '0', '1'
	};
	int writer;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	target = fixture_target(&fixture);
	CHECK_INT_EQ(unlink(fixture.vpd80_path), 0);
	CHECK_INT_EQ(mkfifo(fixture.vpd80_path, 0600), 0);
	memset(&context, 0, sizeof(context));
	context.target = &target;
	CHECK_INT_EQ(pthread_create(&resolver, NULL, resolve_identity, &context), 0);
	writer = open(fixture.vpd80_path, O_WRONLY | O_CLOEXEC);
	CHECK_TRUE(writer >= 0);
	CHECK_INT_EQ(unlink(fixture.nst_alias), 0);
	CHECK_INT_EQ(symlink("sg0", fixture.nst_alias), 0);
	CHECK_INT_EQ(unlink(fixture.vpd80_path), 0);
	CHECK_INT_EQ(write(writer, vpd, sizeof(vpd)), (int)sizeof(vpd));
	CHECK_INT_EQ(close(writer), 0);
	CHECK_INT_EQ(write_vpd80(fixture.vpd80_path, "SERIAL-001"), 0);
	CHECK_INT_EQ(pthread_join(resolver, NULL), 0);
	CHECK_TRUE(context.result < 0);
	return 0;
}

static int test_device_config_is_closed_and_fail_closed(void)
{
	static const char valid[] =
		"{\"nst_path\":\"/dev/tape/by-id/fake-nst\","
		"\"sg_path\":\"/dev/lto-archiver-scsi-fake-sg\","
		"\"serial\":\"SERIAL-TEST\",\"wwid\":\"naa.test\"}\n";
	static const char legacy_nested_sg[] =
		"{\"nst_path\":\"/dev/tape/by-id/fake-nst\","
		"\"sg_path\":\"/dev/lto-archiver/by-id/fake-sg\","
		"\"serial\":\"SERIAL-TEST\",\"wwid\":\"naa.test\"}\n";
	static const char packaged_placeholder[] =
		"{\"nst_path\":\"__UNPROVISIONED__\","
		"\"sg_path\":\"__UNPROVISIONED__\","
		"\"serial\":\"__UNPROVISIONED__\","
		"\"wwid\":\"__UNPROVISIONED__\"}\n";
	static const char legacy_null_selector[] =
		"{\"schema\":1,\"expected_serial\":null,"
		"\"expected_wwid\":null}\n";
	static const char missing[] =
		"{\"nst_path\":\"/dev/tape/by-id/fake-nst\","
		"\"sg_path\":\"/dev/lto-archiver-scsi-fake-sg\","
		"\"serial\":\"SERIAL-TEST\"}\n";
	static const char extra[] =
		"{\"nst_path\":\"/dev/tape/by-id/fake-nst\","
		"\"sg_path\":\"/dev/lto-archiver-scsi-fake-sg\","
		"\"serial\":\"SERIAL-TEST\",\"wwid\":\"naa.test\","
		"\"unexpected\":true}\n";
	static const char rewinding_or_unstable[] =
		"{\"nst_path\":\"/dev/st0\",\"sg_path\":\"/dev/sg0\","
		"\"serial\":\"SERIAL-TEST\",\"wwid\":\"naa.test\"}\n";
	struct identity_fixture fixture;
	struct ltfs_device_config config;
	char config_path[PATH_MAX];
	char symlink_path[PATH_MAX];
	CHECK_INT_EQ(ltfs_device_config_parse(valid, strlen(valid), &config), 0);
	CHECK_STR_EQ(config.nst_path, "/dev/tape/by-id/fake-nst");
	CHECK_STR_EQ(config.sg_path, "/dev/lto-archiver-scsi-fake-sg");
	CHECK_STR_EQ(config.expected_serial, "SERIAL-TEST");
	CHECK_STR_EQ(config.expected_wwid, "naa.test");
	CHECK_TRUE(ltfs_device_config_parse(extra, strlen(extra), &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse("{}\n", 3, &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse(packaged_placeholder,
		strlen(packaged_placeholder), &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse(legacy_null_selector,
		strlen(legacy_null_selector), &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse(legacy_nested_sg,
		strlen(legacy_nested_sg), &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse(missing, strlen(missing), &config) < 0);
	CHECK_TRUE(ltfs_device_config_parse(rewinding_or_unstable,
		strlen(rewinding_or_unstable), &config) < 0);

	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(join_path(config_path, sizeof(config_path), fixture.root,
		"device.json"), 0);
	CHECK_INT_EQ(write_text(config_path, valid), 0);
	CHECK_INT_EQ(chmod(config_path, 0660), 0);
	CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
	CHECK_INT_EQ(chmod(config_path, 0644), 0);
	CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
	CHECK_INT_EQ(chmod(config_path, 04640), 0);
	CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
	CHECK_INT_EQ(chmod(config_path, 0640), 0);
	if (geteuid() == 0) {
		CHECK_INT_EQ(ltfs_device_config_load(config_path, &config), 0);
		CHECK_STR_EQ(config.expected_serial, "SERIAL-TEST");
	} else
		CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
	CHECK_INT_EQ(chmod(config_path, 0600), 0);
	if (geteuid() == 0) {
		CHECK_INT_EQ(ltfs_device_config_load(config_path, &config), 0);
		CHECK_STR_EQ(config.expected_serial, "SERIAL-TEST");
		CHECK_INT_EQ(chown(config_path, 1, 1), 0);
		CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
		CHECK_INT_EQ(chown(config_path, 0, 0), 0);
	} else
		CHECK_TRUE(ltfs_device_config_load(config_path, &config) < 0);
	CHECK_INT_EQ(join_path(symlink_path, sizeof(symlink_path), fixture.root,
		"device-link.json"), 0);
	CHECK_INT_EQ(symlink("device.json", symlink_path), 0);
	CHECK_TRUE(ltfs_device_config_load(symlink_path, &config) < 0);
	return 0;
}

static int test_device_config_metadata_policy_is_exact(void)
{
	CHECK_TRUE(ltfs_device_config_metadata_is_trusted(S_IFREG | 0600, 0));
	CHECK_TRUE(ltfs_device_config_metadata_is_trusted(S_IFREG | 0640, 0));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFREG | 0640, 1));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFDIR | 0640, 0));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFLNK | 0640, 0));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFREG | 0660, 0));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFREG | 0644, 0));
	CHECK_TRUE(!ltfs_device_config_metadata_is_trusted(S_IFREG | 04640, 0));
	return 0;
}

static int test_broker_fd_is_anchored_to_the_configured_sg_identity(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_config config;
	struct ltfs_device_guard_roots roots;
	struct ltfs_device_guard guard;
	struct stat configured_status;
	struct stat anchored_status;
	char requested[64];
	int scsi_fd;
	int anchored_fd;

	CHECK_INT_EQ(fixture_init(&fixture), 0);
	memset(&config, 0, sizeof(config));
	strcpy(config.nst_path, fixture.nst_alias);
	strcpy(config.sg_path, fixture.sg_alias);
	strcpy(config.expected_serial, "SERIAL-001");
	strcpy(config.expected_wwid, "naa.6001");
	memset(&roots, 0, sizeof(roots));
	roots.device_root = fixture.dev_root;
	roots.sysfs_root = fixture.sysfs_root;
	roots.proc_root = "/proc";
	roots.lock_root = fixture.lock_root;
	scsi_fd = open(fixture.sg_alias, O_RDONLY | O_CLOEXEC);
	CHECK_TRUE(scsi_fd >= 0);
	CHECK_TRUE(snprintf(requested, sizeof(requested), "/proc/self/fd/%d",
		scsi_fd) > 0);
	CHECK_INT_EQ(ltfs_device_guard_acquire(&config, requested, &roots,
		"00000000-0000-4000-8000-000000000020",
		LTFS_COMMAND_CLASS_MOUNT, &guard), 0);
	CHECK_TRUE(guard.anchored_device_fd >= 3);
	CHECK_TRUE(strcmp(guard.backend_device_path, requested) != 0);
	CHECK_TRUE(snprintf(requested, sizeof(requested), "/proc/self/fd/%d",
		guard.anchored_device_fd) > 0);
	CHECK_STR_EQ(guard.backend_device_path, requested);
	CHECK_INT_EQ(stat(fixture.sg_alias, &configured_status), 0);
	CHECK_INT_EQ(fstat(guard.anchored_device_fd, &anchored_status), 0);
	CHECK_INT_EQ(configured_status.st_rdev, anchored_status.st_rdev);
	anchored_fd = guard.anchored_device_fd;
	CHECK_INT_EQ(close(scsi_fd), 0);
	CHECK_INT_EQ(unlink(fixture.sg_alias), 0);
	CHECK_INT_EQ(symlink("nst0", fixture.sg_alias), 0);
	CHECK_INT_EQ(fstat(anchored_fd, &anchored_status), 0);
	ltfs_device_guard_release(&guard);
	CHECK_TRUE(fcntl(anchored_fd, F_GETFD) < 0 && errno == EBADF);
	return 0;
}

static int test_broker_fd_rejects_untrusted_or_wrong_descriptors(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_config config;
	struct ltfs_device_guard_roots roots;
	struct ltfs_device_guard guard;
	char requested[64];
	char regular[PATH_MAX];
	int wrong_fd;
	int regular_fd;

	CHECK_INT_EQ(fixture_init(&fixture), 0);
	memset(&config, 0, sizeof(config));
	strcpy(config.nst_path, fixture.nst_alias);
	strcpy(config.sg_path, fixture.sg_alias);
	strcpy(config.expected_serial, "SERIAL-001");
	strcpy(config.expected_wwid, "naa.6001");
	memset(&roots, 0, sizeof(roots));
	roots.device_root = fixture.dev_root;
	roots.sysfs_root = fixture.sysfs_root;
	roots.proc_root = "/proc";
	roots.lock_root = fixture.lock_root;

	wrong_fd = open(fixture.nst_alias, O_RDONLY | O_CLOEXEC);
	CHECK_TRUE(wrong_fd >= 0);
	CHECK_TRUE(snprintf(requested, sizeof(requested), "/proc/self/fd/%d",
		wrong_fd) > 0);
	CHECK_TRUE(ltfs_device_guard_acquire(&config, requested, &roots,
		"00000000-0000-4000-8000-000000000021",
		LTFS_COMMAND_CLASS_MOUNT, &guard) < 0);
	CHECK_INT_EQ(close(wrong_fd), 0);

	CHECK_INT_EQ(join_path(regular, sizeof(regular), fixture.root,
		"ordinary-file"), 0);
	CHECK_INT_EQ(write_text(regular, "not a device"), 0);
	regular_fd = open(regular, O_RDONLY | O_CLOEXEC);
	CHECK_TRUE(regular_fd >= 0);
	CHECK_TRUE(snprintf(requested, sizeof(requested), "/proc/self/fd/%d",
		regular_fd) > 0);
	CHECK_TRUE(ltfs_device_guard_acquire(&config, requested, &roots,
		"00000000-0000-4000-8000-000000000022",
		LTFS_COMMAND_CLASS_MOUNT, &guard) < 0);
	CHECK_INT_EQ(close(regular_fd), 0);

	CHECK_TRUE(ltfs_device_guard_acquire(&config, "/proc/self/fd/03", &roots,
		"00000000-0000-4000-8000-000000000023",
		LTFS_COMMAND_CLASS_MOUNT, &guard) < 0);
	CHECK_TRUE(ltfs_device_guard_acquire(&config, "/proc/self/fd/3/extra", &roots,
		"00000000-0000-4000-8000-000000000024",
		LTFS_COMMAND_CLASS_MOUNT, &guard) < 0);
	CHECK_TRUE(ltfs_device_guard_acquire(&config, fixture.sg_node, &roots,
		"00000000-0000-4000-8000-000000000025",
		LTFS_COMMAND_CLASS_MOUNT, &guard) < 0);
	return 0;
}

static int test_mount_guard_released_after_plugin_startup_error(void)
{
	struct identity_fixture fixture;
	struct ltfs_device_config config;
	struct ltfs_device_guard_roots roots;
	struct ltfs_device_guard mount_guard;
	struct ltfs_device_guard check_guard;
	bool mount_guard_acquired = true;
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	memset(&config, 0, sizeof(config));
	strcpy(config.nst_path, fixture.nst_alias);
	strcpy(config.sg_path, fixture.sg_alias);
	strcpy(config.expected_serial, "SERIAL-001");
	strcpy(config.expected_wwid, "naa.6001");
	memset(&roots, 0, sizeof(roots));
	roots.device_root = fixture.dev_root;
	roots.sysfs_root = fixture.sysfs_root;
	roots.proc_root = "/proc";
	roots.lock_root = fixture.lock_root;
	CHECK_INT_EQ(ltfs_device_guard_acquire(&config, NULL, &roots,
		"00000000-0000-4000-8000-000000000018",
		LTFS_COMMAND_CLASS_MOUNT, &mount_guard), 0);
	CHECK_INT_EQ(ltfs_device_guard_acquire(&config, config.sg_path, &roots,
		"00000000-0000-4000-8000-000000000019",
		LTFS_COMMAND_CLASS_CHECK, &check_guard), LTFS_OPERATION_LOCK_BUSY);
	/* Simulate plugin_load failure after the main path acquired the guard. */
	CHECK_INT_EQ(ltfs_device_guard_release_preserving_result(&mount_guard,
		&mount_guard_acquired, -ENOENT), -ENOENT);
	CHECK_TRUE(!mount_guard_acquired);
	CHECK_INT_EQ(ltfs_device_guard_acquire(&config, config.sg_path, &roots,
		"00000000-0000-4000-8000-000000000019",
		LTFS_COMMAND_CLASS_CHECK, &check_guard), 0);
	ltfs_device_guard_release(&check_guard);
	return 0;
}

static int test_file_backend_uses_only_an_owned_virtual_directory(void)
{
	struct identity_fixture fixture;
	char cartridge[PATH_MAX];
	char regular[PATH_MAX];
	char link_path[PATH_MAX];
	CHECK_INT_EQ(fixture_init(&fixture), 0);
	CHECK_INT_EQ(join_path(cartridge, sizeof(cartridge), fixture.root,
		"cartridge"), 0);
	CHECK_INT_EQ(mkdir(cartridge, 0700), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", cartridge), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("sg", cartridge), 1);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", "/dev/sg0"), -EINVAL);
	CHECK_INT_EQ(join_path(regular, sizeof(regular), fixture.root, "regular"), 0);
	CHECK_INT_EQ(write_text(regular, "not-a-cartridge-directory"), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", regular), -EINVAL);
	CHECK_INT_EQ(join_path(link_path, sizeof(link_path), fixture.root,
		"cartridge-link"), 0);
	CHECK_INT_EQ(symlink("cartridge", link_path), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", link_path), -EINVAL);
	CHECK_INT_EQ(join_path(link_path, sizeof(link_path), fixture.root,
		"cartridge/../cartridge"), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", link_path), -EINVAL);
	CHECK_INT_EQ(chmod(cartridge, 0770), 0);
	CHECK_INT_EQ(ltfs_device_guard_requirement("file", cartridge), -EPERM);
	CHECK_INT_EQ(ltfs_device_guard_requirement(NULL, cartridge), -EINVAL);
	return 0;
}

static int test_media_identity_text_normalization_is_fixed_length(void)
{
	static const unsigned char padded[] = {
		' ', ' ', 'V', 'O', 'L', '0', '1', ' ', ' ', 0, 0,
	};
	static const unsigned char embedded_nul[] = {'V', 'O', 0, 'L', '1', ' '};
	static const unsigned char control[] = {'V', 'O', 0x1f, 'L', '1'};
	static const unsigned char non_ascii[] = {'V', 'O', 0x80, 'L', '1'};
	char normalized[32];

	CHECK_INT_EQ(ltfs_device_identity_normalize_text(normalized,
		sizeof(normalized), padded, sizeof(padded)), 0);
	CHECK_STR_EQ(normalized, "VOL01");
	CHECK_TRUE(ltfs_device_identity_normalize_text(normalized,
		sizeof(normalized), embedded_nul, sizeof(embedded_nul)) < 0);
	CHECK_TRUE(ltfs_device_identity_normalize_text(normalized,
		sizeof(normalized), control, sizeof(control)) < 0);
	CHECK_TRUE(ltfs_device_identity_normalize_text(normalized,
		sizeof(normalized), non_ascii, sizeof(non_ascii)) < 0);
	return 0;
}

int main(void)
{
	if (test_matching_identity_and_canonical_aliases() != 0)
		return 1;
	if (test_rejects_rewinding_and_non_character_paths() != 0)
		return 1;
	if (test_rejects_serial_wwid_and_tuple_mismatch() != 0)
		return 1;
	if (test_rejects_malformed_vpd80_and_wwid() != 0)
		return 1;
	if (test_rejects_missing_boot_id() != 0)
		return 1;
	if (test_identity_diagnostic_reports_exact_failure_phase() != 0)
		return 1;
	if (test_rejects_symlink_swap_during_resolution() != 0)
		return 1;
	if (test_device_config_is_closed_and_fail_closed() != 0)
		return 1;
	if (test_device_config_metadata_policy_is_exact() != 0)
		return 1;
	if (test_broker_fd_is_anchored_to_the_configured_sg_identity() != 0)
		return 1;
	if (test_broker_fd_rejects_untrusted_or_wrong_descriptors() != 0)
		return 1;
	if (test_mount_guard_released_after_plugin_startup_error() != 0)
		return 1;
	if (test_file_backend_uses_only_an_owned_virtual_directory() != 0)
		return 1;
	if (test_media_identity_text_normalization_is_fixed_length() != 0)
		return 1;
	return 0;
}
