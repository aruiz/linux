// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init_syscalls.h>
#include <linux/stringify.h>
#include <linux/timekeeping.h>
#include "initramfs_internal.h"

struct initramfs_test_cpio {
	char *magic;
	unsigned int ino;
	unsigned int mode;
	unsigned int uid;
	unsigned int gid;
	unsigned int nlink;
	unsigned int mtime;
	unsigned int filesize;
	unsigned int devmajor;
	unsigned int devminor;
	unsigned int rdevmajor;
	unsigned int rdevminor;
	unsigned int namesize;
	unsigned int csum;
	char *fname;
	char *data;
};

static size_t fill_cpio(struct initramfs_test_cpio *cs, size_t csz, char *out)
{
	int i;
	size_t off = 0;

	for (i = 0; i < csz; i++) {
		char *pos = &out[off];
		struct initramfs_test_cpio *c = &cs[i];
		size_t thislen;

		/* +1 to account for nulterm */
		thislen = sprintf(pos, "%s"
			"%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x"
			"%s",
			c->magic, c->ino, c->mode, c->uid, c->gid, c->nlink,
			c->mtime, c->filesize, c->devmajor, c->devminor,
			c->rdevmajor, c->rdevminor, c->namesize, c->csum,
			c->fname) + 1;

		pr_debug("packing (%zu): %.*s\n", thislen, (int)thislen, pos);
		if (thislen != CPIO_HDRLEN + c->namesize)
			pr_debug("padded to: %u\n", CPIO_HDRLEN + c->namesize);
		off += CPIO_HDRLEN + c->namesize;
		while (off & 3)
			out[off++] = '\0';

		memcpy(&out[off], c->data, c->filesize);
		off += c->filesize;
		while (off & 3)
			out[off++] = '\0';
	}

	return off;
}

static void __init initramfs_test_extract(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct timespec64 ts_before, ts_after;
	struct kstat st = {};
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 12,
		.gid = 34,
		.nlink = 1,
		.mtime = 56,
		.filesize = 0,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_extract"),
		.csum = 0,
		.fname = "initramfs_test_extract",
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.mtime = 57,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_extract_dir"),
		.fname = "initramfs_test_extract_dir",
	}, {
		.magic = "070701",
		.namesize = sizeof("TRAILER!!!"),
		.fname = "TRAILER!!!",
	} };

	/* +3 to cater for any 4-byte end-alignment */
	cpio_srcbuf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3),
			      GFP_KERNEL);
	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	ktime_get_real_ts64(&ts_before);
	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	ktime_get_real_ts64(&ts_after);
	if (err) {
		KUNIT_FAIL(test, "unpack failed %s", err);
		goto out;
	}

	KUNIT_EXPECT_EQ(test, init_stat(c[0].fname, &st, 0), 0);
	KUNIT_EXPECT_TRUE(test, S_ISREG(st.mode));
	KUNIT_EXPECT_TRUE(test, uid_eq(st.uid, KUIDT_INIT(c[0].uid)));
	KUNIT_EXPECT_TRUE(test, gid_eq(st.gid, KGIDT_INIT(c[0].gid)));
	KUNIT_EXPECT_EQ(test, st.nlink, 1);
	if (IS_ENABLED(CONFIG_INITRAMFS_PRESERVE_MTIME)) {
		KUNIT_EXPECT_EQ(test, st.mtime.tv_sec, c[0].mtime);
	} else {
		KUNIT_EXPECT_GE(test, st.mtime.tv_sec, ts_before.tv_sec);
		KUNIT_EXPECT_LE(test, st.mtime.tv_sec, ts_after.tv_sec);
	}
	KUNIT_EXPECT_EQ(test, st.blocks, c[0].filesize);

	KUNIT_EXPECT_EQ(test, init_stat(c[1].fname, &st, 0), 0);
	KUNIT_EXPECT_TRUE(test, S_ISDIR(st.mode));
	if (IS_ENABLED(CONFIG_INITRAMFS_PRESERVE_MTIME)) {
		KUNIT_EXPECT_EQ(test, st.mtime.tv_sec, c[1].mtime);
	} else {
		KUNIT_EXPECT_GE(test, st.mtime.tv_sec, ts_before.tv_sec);
		KUNIT_EXPECT_LE(test, st.mtime.tv_sec, ts_after.tv_sec);
	}

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_rmdir(c[1].fname), 0);
out:
	kfree(cpio_srcbuf);
}

/*
 * Don't terminate filename. Previously, the cpio filename field was passed
 * directly to filp_open(collected, O_CREAT|..) without nulterm checks. See
 * https://lore.kernel.org/linux-fsdevel/20241030035509.20194-2-ddiss@suse.de
 */
static void __init initramfs_test_fname_overrun(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len, suffix_off;
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = 0,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_fname_overrun"),
		.csum = 0,
		.fname = "initramfs_test_fname_overrun",
	} };

	/*
	 * poison cpio source buffer, so we can detect overrun. source
	 * buffer is used by read_into() when hdr or fname
	 * are already available (e.g. no compression).
	 */
	cpio_srcbuf = kmalloc(CPIO_HDRLEN + PATH_MAX + 3, GFP_KERNEL);
	memset(cpio_srcbuf, 'B', CPIO_HDRLEN + PATH_MAX + 3);
	/* limit overrun to avoid crashes / filp_open() ENAMETOOLONG */
	cpio_srcbuf[CPIO_HDRLEN + strlen(c[0].fname) + 20] = '\0';

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);
	/* overwrite trailing fname terminator and padding */
	suffix_off = len - 1;
	while (cpio_srcbuf[suffix_off] == '\0') {
		cpio_srcbuf[suffix_off] = 'P';
		suffix_off--;
	}

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NOT_NULL(test, err);

	kfree(cpio_srcbuf);
}

static void __init initramfs_test_data(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct file *file;
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = sizeof("ASDF") - 1,
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		.namesize = sizeof("initramfs_test_data"),
		.csum = 0,
		.fname = "initramfs_test_data",
		.data = "ASDF",
	} };

	/* +6 for max name and data 4-byte padding */
	cpio_srcbuf = kmalloc(CPIO_HDRLEN + c[0].namesize + c[0].filesize + 6,
			      GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	file = filp_open(c[0].fname, O_RDONLY, 0);
	if (IS_ERR(file)) {
		KUNIT_FAIL(test, "open failed");
		goto out;
	}

	/* read back file contents into @cpio_srcbuf and confirm match */
	len = kernel_read(file, cpio_srcbuf, c[0].filesize, NULL);
	KUNIT_EXPECT_EQ(test, len, c[0].filesize);
	KUNIT_EXPECT_MEMEQ(test, cpio_srcbuf, c[0].data, len);

	fput(file);
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
out:
	kfree(cpio_srcbuf);
}

static void __init initramfs_test_csum(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct initramfs_test_cpio c[] = { {
		/* 070702 magic indicates a valid csum is present */
		.magic = "070702",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 1,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_csum"),
		.csum = 'A' + 'S' + 'D' + 'F',
		.fname = "initramfs_test_csum",
		.data = "ASDF",
	}, {
		/* mix csum entry above with no-csum entry below */
		.magic = "070701",
		.ino = 2,
		.mode = S_IFREG | 0777,
		.nlink = 1,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_csum_not_here"),
		/* csum ignored */
		.csum = 5555,
		.fname = "initramfs_test_csum_not_here",
		.data = "ASDF",
	} };

	cpio_srcbuf = kmalloc(8192, GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), 0);

	/* mess up the csum and confirm that unpack fails */
	c[0].csum--;
	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NOT_NULL(test, err);

	/*
	 * file (with content) is still retained in case of bad-csum abort.
	 * Perhaps we should change this.
	 */
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), -ENOENT);
	kfree(cpio_srcbuf);
}

/*
 * hardlink hashtable may leak when the archive omits a trailer:
 * https://lore.kernel.org/r/20241107002044.16477-10-ddiss@suse.de/
 */
static void __init initramfs_test_hardlink(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct kstat st0, st1;
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 2,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_hardlink"),
		.fname = "initramfs_test_hardlink",
	}, {
		/* hardlink data is present in last archive entry */
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.nlink = 2,
		.filesize = sizeof("ASDF") - 1,
		.devminor = 1,
		.namesize = sizeof("initramfs_test_hardlink_link"),
		.fname = "initramfs_test_hardlink_link",
		.data = "ASDF",
	} };

	cpio_srcbuf = kmalloc(8192, GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_stat(c[0].fname, &st0, 0), 0);
	KUNIT_EXPECT_EQ(test, init_stat(c[1].fname, &st1, 0), 0);
	KUNIT_EXPECT_EQ(test, st0.ino, st1.ino);
	KUNIT_EXPECT_EQ(test, st0.nlink, 2);
	KUNIT_EXPECT_EQ(test, st1.nlink, 2);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	KUNIT_EXPECT_EQ(test, init_unlink(c[1].fname), 0);

	kfree(cpio_srcbuf);
}

#define INITRAMFS_TEST_MANY_LIMIT 1000
#define INITRAMFS_TEST_MANY_PATH_MAX (sizeof("initramfs_test_many-") \
			+ sizeof(__stringify(INITRAMFS_TEST_MANY_LIMIT)))
static void __init initramfs_test_many(struct kunit *test)
{
	char *err, *cpio_srcbuf, *p;
	size_t len = INITRAMFS_TEST_MANY_LIMIT *
		     (CPIO_HDRLEN + INITRAMFS_TEST_MANY_PATH_MAX + 3);
	char thispath[INITRAMFS_TEST_MANY_PATH_MAX];
	int i;

	p = cpio_srcbuf = kmalloc(len, GFP_KERNEL);

	for (i = 0; i < INITRAMFS_TEST_MANY_LIMIT; i++) {
		struct initramfs_test_cpio c = {
			.magic = "070701",
			.ino = i,
			.mode = S_IFREG | 0777,
			.nlink = 1,
			.devminor = 1,
			.fname = thispath,
		};

		c.namesize = 1 + sprintf(thispath, "initramfs_test_many-%d", i);
		p += fill_cpio(&c, 1, p);
	}

	len = p - cpio_srcbuf;
	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	for (i = 0; i < INITRAMFS_TEST_MANY_LIMIT; i++) {
		sprintf(thispath, "initramfs_test_many-%d", i);
		KUNIT_EXPECT_EQ(test, init_unlink(thispath), 0);
	}

	kfree(cpio_srcbuf);
}

/*
 * An initramfs filename is namesize in length, including the zero-terminator.
 * A filename can be zero-terminated prior to namesize, with the remainder used
 * as padding. This can be useful for e.g. alignment of file data segments with
 * a 4KB filesystem block, allowing for extent sharing (reflinks) between cpio
 * source and destination. This hack works with both GNU cpio and initramfs, as
 * long as PATH_MAX isn't exceeded.
 */
static void __init initramfs_test_fname_pad(struct kunit *test)
{
	char *err;
	size_t len;
	struct file *file;
	char fdata[] = "this file data is aligned at 4K in the archive";
	struct test_fname_pad {
		char padded_fname[4096 - CPIO_HDRLEN];
		char cpio_srcbuf[CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)];
	} *tbufs = kzalloc_obj(struct test_fname_pad);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0,
		.gid = 0,
		.nlink = 1,
		.mtime = 1,
		.filesize = sizeof(fdata),
		.devmajor = 0,
		.devminor = 1,
		.rdevmajor = 0,
		.rdevminor = 0,
		/* align file data at 4K archive offset via padded fname */
		.namesize = 4096 - CPIO_HDRLEN,
		.csum = 0,
		.fname = tbufs->padded_fname,
		.data = fdata,
	} };

	memcpy(tbufs->padded_fname, "padded_fname", sizeof("padded_fname"));
	len = fill_cpio(c, ARRAY_SIZE(c), tbufs->cpio_srcbuf);

	err = unpack_to_rootfs(tbufs->cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	file = filp_open(c[0].fname, O_RDONLY, 0);
	if (IS_ERR(file)) {
		KUNIT_FAIL(test, "open failed");
		goto out;
	}

	/* read back file contents into @cpio_srcbuf and confirm match */
	len = kernel_read(file, tbufs->cpio_srcbuf, c[0].filesize, NULL);
	KUNIT_EXPECT_EQ(test, len, c[0].filesize);
	KUNIT_EXPECT_MEMEQ(test, tbufs->cpio_srcbuf, c[0].data, len);

	fput(file);
	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
out:
	kfree(tbufs);
}

static void __init initramfs_test_fname_path_max(struct kunit *test)
{
	char *err;
	size_t len;
	struct kstat st0, st1;
	char fdata[] = "this file data will not be unpacked";
	struct test_fname_path_max {
		char fname_oversize[PATH_MAX + 1];
		char fname_ok[PATH_MAX];
		char cpio_src[(CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)) * 2];
	} *tbufs = kzalloc_obj(struct test_fname_path_max);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.namesize = sizeof(tbufs->fname_oversize),
		.fname = tbufs->fname_oversize,
		.filesize = sizeof(fdata),
		.data = fdata,
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.nlink = 1,
		.namesize = sizeof(tbufs->fname_ok),
		.fname = tbufs->fname_ok,
	} };

	memset(tbufs->fname_oversize, '/', sizeof(tbufs->fname_oversize) - 1);
	memset(tbufs->fname_ok, '/', sizeof(tbufs->fname_ok) - 1);
	memcpy(tbufs->fname_oversize, "fname_oversize",
	       sizeof("fname_oversize") - 1);
	memcpy(tbufs->fname_ok, "fname_ok", sizeof("fname_ok") - 1);
	len = fill_cpio(c, ARRAY_SIZE(c), tbufs->cpio_src);

	/* unpack skips over fname_oversize instead of returning an error */
	err = unpack_to_rootfs(tbufs->cpio_src, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_stat("fname_oversize", &st0, 0), -ENOENT);
	KUNIT_EXPECT_EQ(test, init_stat("fname_ok", &st1, 0), 0);
	KUNIT_EXPECT_EQ(test, init_rmdir("fname_ok"), 0);

	kfree(tbufs);
}

static void __init initramfs_test_hdr_hex(struct kunit *test)
{
	char *err;
	size_t len;
	char fdata[] = "this file data will not be unpacked";
	struct initramfs_test_bufs {
		char cpio_src[(CPIO_HDRLEN + PATH_MAX + 3 + sizeof(fdata)) * 2];
	} *tbufs = kzalloc(sizeof(struct initramfs_test_bufs), GFP_KERNEL);
	struct initramfs_test_cpio c[] = { {
		.magic = "070701",
		.ino = 1,
		.mode = S_IFREG | 0777,
		.uid = 0x123456,
		.gid = 0x123457,
		.nlink = 1,
		.namesize = sizeof("initramfs_test_hdr_hex_0"),
		.fname = "initramfs_test_hdr_hex_0",
		.filesize = sizeof(fdata),
		.data = fdata,
	}, {
		.magic = "070701",
		.ino = 2,
		.mode = S_IFDIR | 0777,
		.uid = 0x000056,
		.gid = 0x000057,
		.nlink = 1,
		.namesize = sizeof("initramfs_test_hdr_hex_1"),
		.fname = "initramfs_test_hdr_hex_1",
	} };

	/* inject_ox=true to add "0x" cpio field prefixes */
	len = fill_cpio(c, ARRAY_SIZE(c), true, tbufs->cpio_src);

	err = unpack_to_rootfs(tbufs->cpio_src, len);
	KUNIT_EXPECT_NOT_NULL(test, err);

	kfree(tbufs);
}

/* Boilerplate for a regular file entry in cpio test arrays */
#define CPIO_FILE(fname_str)                   \
	{                                      \
		.magic = "070701",             \
		.ino = 1,                      \
		.mode = S_IFREG | 0777,        \
		.nlink = 1,                    \
		.devminor = 1,                 \
		.namesize = sizeof(fname_str), \
		.fname = fname_str,            \
	}

/* Same as CPIO_FILE but with inline data */
#define CPIO_FILE_DATA(fname_str, data_str)       \
	{                                         \
		.magic = "070701",                \
		.ino = 1,                         \
		.mode = S_IFREG | 0777,           \
		.nlink = 1,                       \
		.filesize = sizeof(data_str) - 1, \
		.devminor = 1,                    \
		.namesize = sizeof(fname_str),    \
		.fname = fname_str,               \
		.data = data_str,                 \
	}

#define CPIO_TRAILER                              \
	{                                         \
		.magic = "070701",                \
		.namesize = sizeof("TRAILER!!!"), \
		.fname = "TRAILER!!!",            \
	}

/* Test that the consumed out-parameter tracks bytes eaten by a cpio archive */
static void __init initramfs_test_consumed_cpio(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	unsigned long consumed = 0;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE("consumed_cpio"), { CPIO_TRAILER };

	cpio_srcbuf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3),
			      GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, len);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(cpio_srcbuf);
}

/*
 * With consumed != NULL, unpack_to_rootfs should stop cleanly (no error)
 * when it encounters unrecognised (non-cpio, non-compressed) data after
 * a valid cpio segment.
 */
static void __init initramfs_test_consumed_cpio_then_junk(struct kunit *test)
{
	char *err, *buf;
	size_t cpio_len;
	unsigned long consumed = 0;
	size_t junk_sz = 256;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE_DATA("consumed_cpio_junk", "HELLO"),
		  { CPIO_TRAILER };

	buf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3) + junk_sz,
		      GFP_KERNEL);

	cpio_len = fill_cpio(c, ARRAY_SIZE(c), buf);

	/* Append non-cpio, non-compressed junk after the trailer */
	memset(buf + cpio_len, 'X', junk_sz);

	err = unpack_to_rootfs(buf, cpio_len + junk_sz, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	/*
	 * consumed should account for the cpio segment only; the NUL padding
	 * between trailer and junk may also be consumed, but never the junk.
	 */
	KUNIT_EXPECT_GE(test, consumed, cpio_len);
	KUNIT_EXPECT_LE(test, consumed, cpio_len + junk_sz);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(buf);
}

/*
 * Without consumed (NULL), encountering non-cpio data should produce an
 * error string.
 */
static void __init initramfs_test_no_consumed_junk_errors(struct kunit *test)
{
	char *err;
	char junk[64];

	memset(junk, 'X', sizeof(junk));

	err = unpack_to_rootfs(junk, sizeof(junk), NULL);
	KUNIT_EXPECT_NOT_NULL(test, err);
}

/*
 * Two cpio archives concatenated: with consumed != NULL, the first call
 * should consume exactly the first archive, leaving the second for a
 * subsequent call.
 */
static void __init initramfs_test_consumed_two_cpios(struct kunit *test)
{
	char *err, *buf;
	size_t len1, len2;
	unsigned long consumed = 0;
	struct initramfs_test_cpio c1[] = {
		{ CPIO_FILE("consumed_two_1"), { CPIO_TRAILER };
	struct initramfs_test_cpio c2[] = {
		{
			.magic = "070701",
			.ino = 2,
			.mode = S_IFREG | 0777,
			.nlink = 1,
			.devminor = 1,
			.namesize = sizeof("consumed_two_2"),
			.fname = "consumed_two_2",
		},
		{ CPIO_TRAILER };

	buf = kzalloc(4 * (CPIO_HDRLEN + PATH_MAX + 3), GFP_KERNEL);

	len1 = fill_cpio(c1, ARRAY_SIZE(c1), buf);
	len2 = fill_cpio(c2, ARRAY_SIZE(c2), buf + len1);

	/* First call: should consume exactly the first archive */
	err = unpack_to_rootfs(buf, len1 + len2, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, len1 + len2);

	KUNIT_EXPECT_EQ(test, init_unlink("consumed_two_1"), 0);
	KUNIT_EXPECT_EQ(test, init_unlink("consumed_two_2"), 0);
	kfree(buf);
}

/*
 * cpio archive followed by NUL padding then non-cpio data: the NUL-skipping
 * loop in unpack_to_rootfs should eat the padding, then stop cleanly at
 * the unrecognised data when consumed is provided.
 */
static void __init initramfs_test_consumed_cpio_nul_pad_junk(struct kunit *test)
{
	char *err, *buf;
	size_t cpio_len, total;
	unsigned long consumed = 0;
	size_t nul_pad = 64;
	size_t junk_sz = 128;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE("consumed_nulpad"), { CPIO_TRAILER };

	total = ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3) + nul_pad +
		junk_sz;
	buf = kzalloc(total, GFP_KERNEL);

	cpio_len = fill_cpio(c, ARRAY_SIZE(c), buf);

	/* NUL padding is already zero from kzalloc; add junk after */
	memset(buf + cpio_len + nul_pad, 'J', junk_sz);

	err = unpack_to_rootfs(buf, cpio_len + nul_pad + junk_sz, &consumed);
	KUNIT_EXPECT_NULL(test, err);

	/* Should have consumed the cpio + NUL padding but stopped at junk */
	KUNIT_EXPECT_GE(test, consumed, cpio_len);
	KUNIT_EXPECT_LE(test, consumed, cpio_len + nul_pad + junk_sz);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(buf);
}

/*
 * cpio with file data followed by non-cpio data: ensures consumed is
 * correct even when body_len > 0.
 */
static void __init
initramfs_test_consumed_cpio_data_then_junk(struct kunit *test)
{
	char *err, *buf;
	size_t cpio_len, junk_sz = 256;
	unsigned long consumed = 0;
	struct file *file;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE_DATA("consumed_data_junk", "FILEDATA"),
		  { CPIO_TRAILER };

	buf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3) + junk_sz,
		      GFP_KERNEL);

	cpio_len = fill_cpio(c, ARRAY_SIZE(c), buf);
	memset(buf + cpio_len, 'Z', junk_sz);

	err = unpack_to_rootfs(buf, cpio_len + junk_sz, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_GE(test, consumed, cpio_len);

	file = filp_open(c[0].fname, O_RDONLY, 0);
	if (!IS_ERR(file)) {
		char readback[16] = {};
		size_t n = kernel_read(file, readback, c[0].filesize, NULL);

		KUNIT_EXPECT_EQ(test, n, c[0].filesize);
		KUNIT_EXPECT_MEMEQ(test, readback, c[0].data, n);
		fput(file);
	} else {
		KUNIT_FAIL(test, "open of extracted file failed");
	}

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(buf);
}

/*
 * Pure junk with consumed != NULL and no leading cpio: consumed should be 0
 * and there should be no error (the caller handles unrecognised data).
 */
static void __init initramfs_test_consumed_junk_only(struct kunit *test)
{
	char *err;
	unsigned long consumed = 0;
	char junk[64];

	memset(junk, 'X', sizeof(junk));

	err = unpack_to_rootfs(junk, sizeof(junk), &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, (unsigned long)0);
}

/*
 * Empty buffer: unpack_to_rootfs should succeed with zero consumed
 * regardless of the consumed parameter.
 */
static void __init initramfs_test_consumed_empty(struct kunit *test)
{
	char *err;
	unsigned long consumed = 42;
	char dummy = '\0';

	err = unpack_to_rootfs(&dummy, 0, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, (unsigned long)0);

	err = unpack_to_rootfs(&dummy, 0, NULL);
	KUNIT_EXPECT_NULL(test, err);
}

/*
 * cpio without TRAILER followed by junk: consumed should still report
 * how far we got (the cpio data) and stop at the unrecognised bytes.
 */
static void __init initramfs_test_consumed_no_trailer(struct kunit *test)
{
	char *err, *buf;
	size_t cpio_len, junk_sz = 128;
	unsigned long consumed = 0;
	struct initramfs_test_cpio c[] = { { CPIO_FILE("consumed_notrailer") };

	buf = kzalloc((CPIO_HDRLEN + PATH_MAX + 3) + junk_sz, GFP_KERNEL);

	cpio_len = fill_cpio(c, ARRAY_SIZE(c), buf);
	memset(buf + cpio_len, 'Q', junk_sz);

	err = unpack_to_rootfs(buf, cpio_len + junk_sz, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_GE(test, consumed, cpio_len);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(buf);
}

/*
 * Simulate the boundary where cpio NUL padding overlaps with what would be
 * an EROFS reserved area (first 1024 bytes typically NULs).  The cpio with
 * trailer is followed by exactly EROFS_SUPER_OFFSET (1024) NUL bytes, then
 * a fake EROFS magic.  With consumed, unpack_to_rootfs should eat the cpio
 * and some/all NUL padding, stopping before or at the EROFS magic.
 */
static void __init
initramfs_test_consumed_cpio_erofs_boundary(struct kunit *test)
{
	char *err, *buf;
	size_t cpio_len, total;
	unsigned long consumed = 0;
	size_t erofs_reserved = 1024;
	size_t fake_erofs_sz = 256;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE("consumed_erofs_bnd"), { CPIO_TRAILER };

	total = ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3) + erofs_reserved +
		fake_erofs_sz;
	buf = kzalloc(total, GFP_KERNEL);

	cpio_len = fill_cpio(c, ARRAY_SIZE(c), buf);

	/*
	 * Place a fake EROFS magic at the superblock offset within the
	 * simulated EROFS image.  This is non-cpio, non-compressed data,
	 * so unpack_to_rootfs with consumed should stop.
	 */
	memset(buf + cpio_len + erofs_reserved, 'E', fake_erofs_sz);

	err = unpack_to_rootfs(buf, cpio_len + erofs_reserved + fake_erofs_sz,
			       &consumed);
	KUNIT_EXPECT_NULL(test, err);

	/*
	 * consumed must include the cpio; it may also include NUL padding
	 * that the NUL-skip loop eats.  It must not extend past the NULs
	 * into the fake EROFS region.
	 */
	KUNIT_EXPECT_GE(test, consumed, cpio_len);
	KUNIT_EXPECT_LE(test, consumed, cpio_len + erofs_reserved);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(buf);
}

/*
 * NUL-only buffer with consumed: the NUL-skip loop should eat everything,
 * consumed == len, no error.
 */
static void __init initramfs_test_consumed_all_nuls(struct kunit *test)
{
	char *err, *buf;
	unsigned long consumed = 0;
	size_t sz = 4096;

	buf = kzalloc(sz, GFP_KERNEL);

	err = unpack_to_rootfs(buf, sz, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, (unsigned long)sz);

	kfree(buf);
}

/*
 * cpio archive where the end padding runs right up to the buffer boundary
 * (no trailing junk at all) with consumed != NULL.
 */
static void __init initramfs_test_consumed_exact_fit(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	unsigned long consumed = 0;
	struct initramfs_test_cpio c[] = {
		{ CPIO_FILE_DATA("consumed_exact_fit", "FIT"), { CPIO_TRAILER };

	cpio_srcbuf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3),
			      GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, &consumed);
	KUNIT_EXPECT_NULL(test, err);
	KUNIT_EXPECT_EQ(test, consumed, len);

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(cpio_srcbuf);
}

static void __init initramfs_test_symlink(struct kunit *test)
{
	char *err, *cpio_srcbuf;
	size_t len;
	struct kstat st = {};
	struct initramfs_test_cpio c[] = {
		{
			.magic = "070701",
			.ino = 1,
			.mode = S_IFLNK | 0777,
			.nlink = 1,
			.filesize = sizeof("symlink_dest") - 1,
			.devminor = 1,
			.namesize = sizeof("initramfs_test_symlink"),
			.fname = "initramfs_test_symlink",
			.data = "symlink_dest",
		},
		{ CPIO_TRAILER };

	cpio_srcbuf = kzalloc(ARRAY_SIZE(c) * (CPIO_HDRLEN + PATH_MAX + 3),
			      GFP_KERNEL);

	len = fill_cpio(c, ARRAY_SIZE(c), cpio_srcbuf);

	err = unpack_to_rootfs(cpio_srcbuf, len, NULL);
	KUNIT_EXPECT_NULL(test, err);

	KUNIT_EXPECT_EQ(test, init_stat(c[0].fname, &st, AT_SYMLINK_NOFOLLOW),
			0);
	KUNIT_EXPECT_TRUE(test, S_ISLNK(st.mode));

	KUNIT_EXPECT_EQ(test, init_unlink(c[0].fname), 0);
	kfree(cpio_srcbuf);
}

#ifdef CONFIG_INITRD_EROFS
#include <uapi/linux/magic.h>
#include "../fs/erofs/erofs_fs.h"

/*
 * Helper to fill a minimal EROFS superblock at buf + off + EROFS_SUPER_OFFSET.
 * @blocks: block count, @blkszbits: log2(block_size),
 * @feat_incompat: feature_incompat flags.
 * The image size = blocks << blkszbits.  Caller must ensure buf is large
 * enough for the reserved area + superblock + the implied image size.
 */
static void fill_erofs_sb(void *buf, unsigned long off, u32 blocks,
			  u8 blkszbits, u32 feat_incompat)
{
	struct erofs_super_block *sb = buf + off + EROFS_SUPER_OFFSET;

	memset(sb, 0, sizeof(*sb));
	sb->magic = cpu_to_le32(EROFS_SUPER_MAGIC_V1);
	sb->blkszbits = blkszbits;
	sb->blocks_lo = cpu_to_le32(blocks);
	sb->feature_incompat = cpu_to_le32(feat_incompat);
}

struct erofs_parse_case {
	const char *desc;
	u32 blocks;
	u8 blkszbits;
	u32 feat;
	size_t buf_size;
	unsigned long off;
	unsigned long expected;
};

static const struct erofs_parse_case erofs_parse_cases[] = {
	{
		.desc = "valid 8x4K image at offset 0",
		.blocks = 8,
		.blkszbits = 12,
		.buf_size = 8 * 4096,
		.expected = 8 * 4096,
	},
	{
		.desc = "min blkszbits (512B)",
		.blocks = 16,
		.blkszbits = EROFS_BLKSZBITS_MIN,
		.buf_size = 16 * 512,
		.expected = 16 * 512,
	},
	{
		.desc = "blkszbits too low",
		.blocks = 16,
		.blkszbits = EROFS_BLKSZBITS_MIN - 1,
		.buf_size = 8192,
	},
	{
		.desc = "blkszbits too high",
		.blocks = 1,
		.blkszbits = EROFS_BLKSZBITS_MAX + 1,
		.buf_size = 8192,
	},
	{
		.desc = "unsupported feature flag",
		.blocks = 8,
		.blkszbits = 12,
		.feat = EROFS_ALL_FEATURE_INCOMPAT + 1,
		.buf_size = 8 * 4096,
	},
	{
		.desc = "zero block count",
		.blocks = 0,
		.blkszbits = 12,
		.buf_size = 8192,
	},
	{
		.desc = "image exceeds buffer",
		.blocks = 8,
		.blkszbits = 12,
		.buf_size = 4096,
	},
};

static void __init initramfs_test_erofs_parse(struct kunit *test)
{
	const struct erofs_parse_case *tc = test->param_value;
	void *buf;
	unsigned long result;

	buf = kzalloc(tc->buf_size, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);

	fill_erofs_sb(buf, tc->off, tc->blocks, tc->blkszbits, tc->feat);

	result = try_parse_erofs(buf, tc->off, tc->buf_size);
	KUNIT_EXPECT_EQ(test, result, tc->expected);

	kfree(buf);
}

static void erofs_parse_case_desc(const struct erofs_parse_case *tc, char *desc)
{
	strscpy(desc, tc->desc, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(erofs_parse, erofs_parse_cases, erofs_parse_case_desc);
static void __init initramfs_test_erofs_valid_nonzero_offset(struct kunit *test)
{
	void *buf;
	unsigned long result;
	u32 blocks = 4;
	u8 blkszbits = 12;
	size_t img_size = (size_t)blocks << blkszbits;
	size_t offset = 2048;

	buf = kzalloc(offset + img_size, GFP_KERNEL);

	fill_erofs_sb(buf, offset, blocks, blkszbits, 0);

	result = try_parse_erofs(buf, offset, offset + img_size);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)img_size);

	/* offset 0 should not find anything */
	result = try_parse_erofs(buf, 0, offset + img_size);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)0);

	kfree(buf);
}

static void __init initramfs_test_erofs_bad_magic(struct kunit *test)
{
	void *buf;
	unsigned long result;
	struct erofs_super_block *sb;
	size_t sz = 8 * 4096;

	buf = kzalloc(sz, GFP_KERNEL);

	fill_erofs_sb(buf, 0, 8, 12, 0);
	sb = buf + EROFS_SUPER_OFFSET;
	sb->magic = cpu_to_le32(0xDEADBEEF);

	result = try_parse_erofs(buf, 0, sz);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)0);

	kfree(buf);
}

static void __init initramfs_test_erofs_buffer_too_small(struct kunit *test)
{
	char buf[64] = {};
	unsigned long result;

	result = try_parse_erofs(buf, 0, sizeof(buf));
	KUNIT_EXPECT_EQ(test, result, (unsigned long)0);

	result = try_parse_erofs(buf, 0, 0);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)0);
}

static void __init initramfs_test_erofs_48bit_blocks(struct kunit *test)
{
	void *buf;
	unsigned long result;
	struct erofs_super_block *sb;
	u32 blocks_lo = 4;
	u8 blkszbits = 12;
	size_t img_size = (size_t)blocks_lo << blkszbits;

	buf = kzalloc(img_size, GFP_KERNEL);

	fill_erofs_sb(buf, 0, blocks_lo, blkszbits,
		      EROFS_FEATURE_INCOMPAT_48BIT);
	sb = buf + EROFS_SUPER_OFFSET;
	sb->rb.blocks_hi = cpu_to_le16(0);

	result = try_parse_erofs(buf, 0, img_size);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)img_size);

	kfree(buf);
}

static void __init initramfs_test_erofs_offset_near_end(struct kunit *test)
{
	void *buf;
	unsigned long result;
	size_t sz = 2048;

	buf = kzalloc(sz, GFP_KERNEL);

	fill_erofs_sb(buf, 0, 1, 12, 0);
	/* offset so close to end that reserved area + sb won't fit */
	result = try_parse_erofs(buf, sz - 64, sz);
	KUNIT_EXPECT_EQ(test, result, (unsigned long)0);

	kfree(buf);
}
#endif /* CONFIG_INITRD_EROFS */

/*
 * The kunit_case/_suite struct cannot be marked as __initdata as this will be
 * used in debugfs to retrieve results after test has run.
 */
static struct kunit_case __refdata initramfs_test_cases[] = {
	KUNIT_CASE(initramfs_test_extract),
	KUNIT_CASE(initramfs_test_fname_overrun),
	KUNIT_CASE(initramfs_test_data),
	KUNIT_CASE(initramfs_test_csum),
	KUNIT_CASE(initramfs_test_hardlink),
	KUNIT_CASE(initramfs_test_many),
	KUNIT_CASE(initramfs_test_fname_pad),
	KUNIT_CASE(initramfs_test_fname_path_max),
	KUNIT_CASE(initramfs_test_hdr_hex),
	KUNIT_CASE(initramfs_test_consumed_cpio),
	KUNIT_CASE(initramfs_test_consumed_cpio_then_junk),
	KUNIT_CASE(initramfs_test_no_consumed_junk_errors),
	KUNIT_CASE(initramfs_test_consumed_two_cpios),
	KUNIT_CASE(initramfs_test_consumed_cpio_nul_pad_junk),
	KUNIT_CASE(initramfs_test_consumed_cpio_data_then_junk),
	KUNIT_CASE(initramfs_test_consumed_junk_only),
	KUNIT_CASE(initramfs_test_consumed_empty),
	KUNIT_CASE(initramfs_test_consumed_no_trailer),
	KUNIT_CASE(initramfs_test_consumed_cpio_erofs_boundary),
	KUNIT_CASE(initramfs_test_consumed_all_nuls),
	KUNIT_CASE(initramfs_test_consumed_exact_fit),
	KUNIT_CASE(initramfs_test_symlink),
#ifdef CONFIG_INITRD_EROFS
	KUNIT_CASE_PARAM(initramfs_test_erofs_parse, erofs_parse_gen_params),
	KUNIT_CASE(initramfs_test_erofs_valid_nonzero_offset),
	KUNIT_CASE(initramfs_test_erofs_bad_magic),
	KUNIT_CASE(initramfs_test_erofs_buffer_too_small),
	KUNIT_CASE(initramfs_test_erofs_48bit_blocks),
	KUNIT_CASE(initramfs_test_erofs_offset_near_end),
#endif
	{},
};

static struct kunit_suite initramfs_test_suite = {
	.name = "initramfs",
	.test_cases = initramfs_test_cases,
};
kunit_test_init_section_suites(&initramfs_test_suite);

MODULE_DESCRIPTION("Initramfs KUnit test suite");
MODULE_LICENSE("GPL v2");
