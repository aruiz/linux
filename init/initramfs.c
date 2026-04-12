// SPDX-License-Identifier: GPL-2.0
#include <linux/async.h>
#include <linux/delay.h>
#include <linux/dirent.h>
#include <linux/export.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/init_syscalls.h>
#include <linux/kstrtox.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/namei.h>
#include <linux/overflow.h>
#include <linux/magic.h>
#include <linux/fs_struct.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/umh.h>
#include <linux/utime.h>
#include <uapi/linux/mount.h>

#include <asm/byteorder.h>

#include "../fs/erofs/erofs_fs.h"
#include "do_mounts.h"
#include "initramfs_internal.h"

static __initdata bool csum_present;
static __initdata u32 io_csum;

#ifdef CONFIG_INITRD_EROFS
static bool erofs_initrd_deferred;
static bool __initdata erofs_initrd_root_pivoted;
void __init erofs_memback_set_pending(void *data, unsigned long size);
#endif

static ssize_t __init xwrite(struct file *file, const unsigned char *p,
		size_t count, loff_t *pos)
{
	ssize_t out = 0;

	/* sys_write only can write MAX_RW_COUNT aka 2G-4K bytes at most */
	while (count) {
		ssize_t rv = kernel_write(file, p, count, pos);

		if (rv < 0) {
			if (rv == -EINTR || rv == -EAGAIN)
				continue;
			return out ? out : rv;
		} else if (rv == 0)
			break;

		if (csum_present) {
			ssize_t i;

			for (i = 0; i < rv; i++)
				io_csum += p[i];
		}

		p += rv;
		out += rv;
		count -= rv;
	}

	return out;
}

static __initdata char *message;
static void __init error(char *x)
{
	if (!message)
		message = x;
}

#define panic_show_mem(fmt, ...) \
	({ show_mem(); panic(fmt, ##__VA_ARGS__); })

/* link hash */

#define N_ALIGN(len) ((((len) + 1) & ~3) + 2)

static __initdata struct hash {
	int ino, minor, major;
	umode_t mode;
	struct hash *next;
	char name[N_ALIGN(PATH_MAX)];
} *head[32];
static __initdata bool hardlink_seen;

static inline int hash(int major, int minor, int ino)
{
	unsigned long tmp = ino + minor + (major << 3);
	tmp += tmp >> 5;
	return tmp & 31;
}

static char __init *find_link(int major, int minor, int ino,
			      umode_t mode, char *name)
{
	struct hash **p, *q;
	for (p = head + hash(major, minor, ino); *p; p = &(*p)->next) {
		if ((*p)->ino != ino)
			continue;
		if ((*p)->minor != minor)
			continue;
		if ((*p)->major != major)
			continue;
		if (((*p)->mode ^ mode) & S_IFMT)
			continue;
		return (*p)->name;
	}
	q = kmalloc_obj(struct hash);
	if (!q)
		panic_show_mem("can't allocate link hash entry");
	q->major = major;
	q->minor = minor;
	q->ino = ino;
	q->mode = mode;
	strscpy(q->name, name);
	q->next = NULL;
	*p = q;
	hardlink_seen = true;
	return NULL;
}

static void __init free_hash(void)
{
	struct hash **p, *q;
	for (p = head; hardlink_seen && p < head + 32; p++) {
		while (*p) {
			q = *p;
			*p = q->next;
			kfree(q);
		}
	}
	hardlink_seen = false;
}

#ifdef CONFIG_INITRAMFS_PRESERVE_MTIME
static void __init do_utime(char *filename, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	init_utimes(filename, t);
}

static void __init do_utime_path(const struct path *path, time64_t mtime)
{
	struct timespec64 t[2] = { { .tv_sec = mtime }, { .tv_sec = mtime } };
	vfs_utimes(path, t);
}

static __initdata LIST_HEAD(dir_list);
struct dir_entry {
	struct list_head list;
	time64_t mtime;
	char name[];
};

static void __init dir_add(const char *name, size_t nlen, time64_t mtime)
{
	struct dir_entry *de;

	de = kmalloc_flex(*de, name, nlen);
	if (!de)
		panic_show_mem("can't allocate dir_entry buffer");
	INIT_LIST_HEAD(&de->list);
	strscpy(de->name, name, nlen);
	de->mtime = mtime;
	list_add(&de->list, &dir_list);
}

static void __init dir_utime(void)
{
	struct dir_entry *de, *tmp;
	list_for_each_entry_safe(de, tmp, &dir_list, list) {
		list_del(&de->list);
		do_utime(de->name, de->mtime);
		kfree(de);
	}
}
#else
static void __init do_utime(char *filename, time64_t mtime) {}
static void __init do_utime_path(const struct path *path, time64_t mtime) {}
static void __init dir_add(const char *name, size_t nlen, time64_t mtime) {}
static void __init dir_utime(void) {}
#endif

static __initdata time64_t mtime;

/* cpio header parsing */

static __initdata unsigned long ino, major, minor, nlink;
static __initdata umode_t mode;
static __initdata unsigned long body_len, name_len;
static __initdata uid_t uid;
static __initdata gid_t gid;
static __initdata unsigned rdev;
static __initdata u32 hdr_csum;

static int __init parse_header(char *s)
{
	__be32 header[13];
	int ret;

	ret = hex2bin((u8 *)header, s + 6, sizeof(header));
	if (ret) {
		error("damaged header");
		return ret;
	}

	ino = be32_to_cpu(header[0]);
	mode = be32_to_cpu(header[1]);
	uid = be32_to_cpu(header[2]);
	gid = be32_to_cpu(header[3]);
	nlink = be32_to_cpu(header[4]);
	mtime = be32_to_cpu(header[5]); /* breaks in y2106 */
	body_len = be32_to_cpu(header[6]);
	major = be32_to_cpu(header[7]);
	minor = be32_to_cpu(header[8]);
	rdev = new_encode_dev(MKDEV(be32_to_cpu(header[9]), be32_to_cpu(header[10])));
	name_len = be32_to_cpu(header[11]);
	hdr_csum = be32_to_cpu(header[12]);
	return 0;
}

/* Finite-state machine */

static __initdata enum state {
	Start,
	Collect,
	GotHeader,
	SkipIt,
	GotName,
	CopyFile,
	GotSymlink,
	Reset
} state, next_state;

static __initdata char *victim;
static unsigned long byte_count __initdata;
static __initdata loff_t this_header, next_header;

static inline void __init eat(unsigned n)
{
	victim += n;
	this_header += n;
	byte_count -= n;
}

static __initdata char *collected;
static long remains __initdata;
static __initdata char *collect;

static void __init read_into(char *buf, unsigned size, enum state next)
{
	if (byte_count >= size) {
		collected = victim;
		eat(size);
		state = next;
	} else {
		collect = collected = buf;
		remains = size;
		next_state = next;
		state = Collect;
	}
}

static __initdata char *header_buf, *symlink_buf, *name_buf;

static int __init do_start(void)
{
	read_into(header_buf, CPIO_HDRLEN, GotHeader);
	return 0;
}

static int __init do_collect(void)
{
	unsigned long n = remains;
	if (byte_count < n)
		n = byte_count;
	memcpy(collect, victim, n);
	eat(n);
	collect += n;
	if ((remains -= n) != 0)
		return 1;
	state = next_state;
	return 0;
}

static int __init do_header(void)
{
	if (!memcmp(collected, "070701", 6)) {
		csum_present = false;
	} else if (!memcmp(collected, "070702", 6)) {
		csum_present = true;
	} else {
		if (memcmp(collected, "070707", 6) == 0)
			error("incorrect cpio method used: use -H newc option");
		else
			error("no cpio magic");
		return 1;
	}
	if (parse_header(collected))
		return 1;
	next_header = this_header + N_ALIGN(name_len) + body_len;
	next_header = (next_header + 3) & ~3;
	state = SkipIt;
	if (name_len <= 0 || name_len > PATH_MAX)
		return 0;
	if (S_ISLNK(mode)) {
		if (body_len > PATH_MAX)
			return 0;
		collect = collected = symlink_buf;
		remains = N_ALIGN(name_len) + body_len;
		next_state = GotSymlink;
		state = Collect;
		return 0;
	}
	if (S_ISREG(mode) || !body_len)
		read_into(name_buf, N_ALIGN(name_len), GotName);
	return 0;
}

static int __init do_skip(void)
{
	if (this_header + byte_count < next_header) {
		eat(byte_count);
		return 1;
	} else {
		eat(next_header - this_header);
		state = next_state;
		return 0;
	}
}

static int __init do_reset(void)
{
	while (byte_count && *victim == '\0')
		eat(1);
	if (byte_count && (this_header & 3))
		error("broken padding");
	return 1;
}

static void __init clean_path(char *path, umode_t fmode)
{
	struct kstat st;

	if (!init_stat(path, &st, AT_SYMLINK_NOFOLLOW) &&
	    (st.mode ^ fmode) & S_IFMT) {
		if (S_ISDIR(st.mode))
			init_rmdir(path);
		else
			init_unlink(path);
	}
}

static int __init maybe_link(void)
{
	if (nlink >= 2) {
		char *old = find_link(major, minor, ino, mode, collected);
		if (old) {
			clean_path(collected, 0);
			return (init_link(old, collected) < 0) ? -1 : 1;
		}
	}
	return 0;
}

static __initdata struct file *wfile;
static __initdata loff_t wfile_pos;

static int __init do_name(void)
{
	state = SkipIt;
	next_state = Reset;

	/* name_len > 0 && name_len <= PATH_MAX checked in do_header */
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs name without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}

	if (strcmp(collected, "TRAILER!!!") == 0) {
		free_hash();
		return 0;
	}
	clean_path(collected, mode);
	if (S_ISREG(mode)) {
		int ml = maybe_link();
		if (ml >= 0) {
			int openflags = O_WRONLY|O_CREAT|O_LARGEFILE;
			if (ml != 1)
				openflags |= O_TRUNC;
			wfile = filp_open(collected, openflags, mode);
			if (IS_ERR(wfile))
				return 0;
			wfile_pos = 0;
			io_csum = 0;

			vfs_fchown(wfile, uid, gid);
			vfs_fchmod(wfile, mode);
			if (body_len)
				vfs_truncate(&wfile->f_path, body_len);
			state = CopyFile;
		}
	} else if (S_ISDIR(mode)) {
		init_mkdir(collected, mode);
		init_chown(collected, uid, gid, 0);
		init_chmod(collected, mode);
		dir_add(collected, name_len, mtime);
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (maybe_link() == 0) {
			init_mknod(collected, mode, rdev);
			init_chown(collected, uid, gid, 0);
			init_chmod(collected, mode);
			do_utime(collected, mtime);
		}
	}
	return 0;
}

static int __init do_copy(void)
{
	if (byte_count >= body_len) {
		if (xwrite(wfile, victim, body_len, &wfile_pos) != body_len)
			error("write error");

		do_utime_path(&wfile->f_path, mtime);
		fput(wfile);
		if (csum_present && io_csum != hdr_csum)
			error("bad data checksum");
		eat(body_len);
		state = SkipIt;
		return 0;
	} else {
		if (xwrite(wfile, victim, byte_count, &wfile_pos) != byte_count)
			error("write error");
		body_len -= byte_count;
		eat(byte_count);
		return 1;
	}
}

static int __init do_symlink(void)
{
	if (collected[name_len - 1] != '\0') {
		pr_err("initramfs symlink without nulterm: %.*s\n",
		       (int)name_len, collected);
		error("malformed archive");
		return 1;
	}
	collected[N_ALIGN(name_len) + body_len] = '\0';
	clean_path(collected, 0);
	init_symlink(collected + N_ALIGN(name_len), collected);
	init_chown(collected, uid, gid, AT_SYMLINK_NOFOLLOW);
	do_utime(collected, mtime);
	state = SkipIt;
	next_state = Reset;
	return 0;
}

static __initdata int (*actions[])(void) = {
	[Start]		= do_start,
	[Collect]	= do_collect,
	[GotHeader]	= do_header,
	[SkipIt]	= do_skip,
	[GotName]	= do_name,
	[CopyFile]	= do_copy,
	[GotSymlink]	= do_symlink,
	[Reset]		= do_reset,
};

static long __init write_buffer(char *buf, unsigned long len)
{
	byte_count = len;
	victim = buf;

	while (!actions[state]())
		;
	return len - byte_count;
}

static long __init flush_buffer(void *bufv, unsigned long len)
{
	char *buf = bufv;
	long written;
	long origLen = len;
	if (message)
		return -1;
	while ((written = write_buffer(buf, len)) < len && !message) {
		char c = buf[written];
		if (c == '0') {
			buf += written;
			len -= written;
			state = Start;
		} else if (c == 0) {
			buf += written;
			len -= written;
			state = Reset;
		} else
			error("junk within compressed archive");
	}
	return origLen;
}

static unsigned long my_inptr __initdata; /* index of next byte to be processed in inbuf */

#include <linux/decompress/generic.h>

/**
 * unpack_to_rootfs - decompress and extract an initramfs archive
 * @buf: input initramfs archive to extract
 * @len: length of initramfs data to process
 * @consumed: if non-NULL, set to the number of bytes consumed from @buf
 *
 * Returns: NULL for success or an error message string
 *
 * When @consumed is non-NULL and the loop encounters data that is neither
 * cpio nor a recognised compression format, it breaks cleanly instead of
 * reporting an error.  The caller can then inspect the remaining data
 * (e.g. to check for an EROFS image) and decide how to proceed.
 *
 * This symbol shouldn't be used externally. It's available for unit tests.
 */
char *__init unpack_to_rootfs(char *buf, unsigned long len,
			      unsigned long *consumed)
{
	char *original_buf = buf;
	long written;
	decompress_fn decompress;
	const char *compress_name;
	struct {
		char header[CPIO_HDRLEN];
		char symlink[PATH_MAX + N_ALIGN(PATH_MAX) + 1];
		char name[N_ALIGN(PATH_MAX)];
	} *bufs = kmalloc_obj(*bufs);

	if (!bufs)
		panic_show_mem("can't allocate buffers");

	header_buf = bufs->header;
	symlink_buf = bufs->symlink;
	name_buf = bufs->name;

	state = Start;
	this_header = 0;
	message = NULL;
	while (!message && len) {
		loff_t saved_offset = this_header;
		if (*buf == '0' && !(this_header & 3)) {
			state = Start;
			written = write_buffer(buf, len);
			buf += written;
			len -= written;
			continue;
		}
		if (!*buf) {
			buf++;
			len--;
			this_header++;
			continue;
		}
		this_header = 0;
		decompress = decompress_method(buf, len, &compress_name);
		pr_debug("Detected %s compressed data\n", compress_name);
		if (decompress) {
			int res = decompress(buf, len, NULL, flush_buffer, NULL,
				   &my_inptr, error);
			if (res)
				error("decompressor failed");
		} else if (compress_name) {
			pr_err("compression method %s not configured\n",
			       compress_name);
			error("decompressor failed");
		} else if (consumed) {
			/* Let the caller handle unrecognised data */
			break;
		} else {
			error("invalid magic at start of compressed archive");
		}
		if (state != Reset)
			error("junk at the end of compressed archive");
		this_header = saved_offset + my_inptr;
		buf += my_inptr;
		len -= my_inptr;
	}
	dir_utime();
	/* free any hardlink state collected without optional TRAILER!!! */
	free_hash();
	kfree(bufs);
	if (consumed)
		*consumed = buf - original_buf;
	return message;
}

static int __initdata do_retain_initrd;

static int __init retain_initrd_param(char *str)
{
	if (*str)
		return 0;
	do_retain_initrd = 1;
	return 1;
}
__setup("retain_initrd", retain_initrd_param);

#ifdef CONFIG_ARCH_HAS_KEEPINITRD
static int __init keepinitrd_setup(char *__unused)
{
	do_retain_initrd = 1;
	return 1;
}
__setup("keepinitrd", keepinitrd_setup);
#endif

static bool __initdata initramfs_async = true;
static int __init initramfs_async_setup(char *str)
{
	return kstrtobool(str, &initramfs_async) == 0;
}
__setup("initramfs_async=", initramfs_async_setup);

extern char __initramfs_start[];
extern unsigned long __initramfs_size;
#include <linux/initrd.h>
#include <linux/kexec.h>

static BIN_ATTR(initrd, 0440, sysfs_bin_attr_simple_read, NULL, 0);

void __init reserve_initrd_mem(void)
{
	phys_addr_t start;
	unsigned long size;

	/* Ignore the virtul address computed during device tree parsing */
	initrd_start = initrd_end = 0;

	if (!phys_initrd_size)
		return;
	/*
	 * Round the memory region to page boundaries as per free_initrd_mem()
	 * This allows us to detect whether the pages overlapping the initrd
	 * are in use, but more importantly, reserves the entire set of pages
	 * as we don't want these pages allocated for other purposes.
	 */
	start = round_down(phys_initrd_start, PAGE_SIZE);
	size = phys_initrd_size + (phys_initrd_start - start);
	size = round_up(size, PAGE_SIZE);

	if (!memblock_is_region_memory(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx is not a memory region",
		       (u64)start, size);
		goto disable;
	}

	if (memblock_is_region_reserved(start, size)) {
		pr_err("INITRD: 0x%08llx+0x%08lx overlaps in-use memory region\n",
		       (u64)start, size);
		goto disable;
	}

	memblock_reserve(start, size);
	/* Now convert initrd to virtual addresses */
	initrd_start = (unsigned long)__va(phys_initrd_start);
	initrd_end = initrd_start + phys_initrd_size;
	initrd_below_start_ok = 1;

	return;
disable:
	pr_cont(" - disabling initrd\n");
	initrd_start = 0;
	initrd_end = 0;
}

void __weak __init free_initrd_mem(unsigned long start, unsigned long end)
{
	free_reserved_area((void *)start, (void *)end, POISON_FREE_INITMEM,
			"initrd");
}

#ifdef CONFIG_CRASH_RESERVE
static bool __init kexec_free_initrd(void)
{
	unsigned long crashk_start = (unsigned long)__va(crashk_res.start);
	unsigned long crashk_end   = (unsigned long)__va(crashk_res.end);

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (initrd_start >= crashk_end || initrd_end <= crashk_start)
		return false;

	/*
	 * Initialize initrd memory region since the kexec boot does not do.
	 */
	memset((void *)initrd_start, 0, initrd_end - initrd_start);
	if (initrd_start < crashk_start)
		free_initrd_mem(initrd_start, crashk_start);
	if (initrd_end > crashk_end)
		free_initrd_mem(crashk_end, initrd_end);
	return true;
}
#else
static inline bool kexec_free_initrd(void)
{
	return false;
}
#endif /* CONFIG_KEXEC_CORE */

#ifdef CONFIG_BLK_DEV_RAM
static void __init populate_initrd_image(char *err)
{
	ssize_t written;
	struct file *file;
	loff_t pos = 0;

	printk(KERN_INFO "rootfs image is not initramfs (%s); looks like an initrd\n",
			err);
	file = filp_open("/initrd.image", O_WRONLY|O_CREAT|O_LARGEFILE, 0700);
	if (IS_ERR(file))
		return;

	written = xwrite(file, (char *)initrd_start, initrd_end - initrd_start,
			&pos);
	if (written != initrd_end - initrd_start)
		pr_err("/initrd.image: incomplete write (%zd != %ld)\n",
		       written, initrd_end - initrd_start);
	fput(file);
}
#endif /* CONFIG_BLK_DEV_RAM */

#ifdef CONFIG_INITRD_EROFS
#define EROFS_SB_MINSIZE (EROFS_SUPER_OFFSET + sizeof(struct erofs_super_block))

/* Total number of initrd layers (cpio + EROFS combined). */
#define MAX_INITRD_LAYERS 128
#define MAX_INITRD_LAYER_DIGITS (sizeof(__stringify(MAX_INITRD_LAYERS)) - 1)

#define INITRD_LAYER_MNT_MAX \
	(sizeof("/initrd_layers/") + MAX_INITRD_LAYER_DIGITS)

/*
 * Try to parse an EROFS superblock at @buf + @off.
 * Returns the image size in bytes, or 0 if not a valid EROFS image.
 */
unsigned long __init try_parse_erofs(void *buf, unsigned long off,
				     unsigned long len)
{
	struct erofs_super_block *sb;
	u64 blocks, img_size;

	/* Need enough room for the 1024-byte reserved area + superblock */
	if (len < EROFS_SB_MINSIZE || off > len - EROFS_SB_MINSIZE)
		return 0;

	sb = buf + off + EROFS_SUPER_OFFSET;
	if (le32_to_cpu(sb->magic) != EROFS_SUPER_MAGIC_V1)
		return 0;

	/* blkszbits is log2(block_size); valid range is 512 B .. 1 GiB */
	if (sb->blkszbits < EROFS_BLKSZBITS_MIN ||
	    sb->blkszbits > EROFS_BLKSZBITS_MAX)
		return 0;

	if (le32_to_cpu(sb->feature_incompat) & ~EROFS_ALL_FEATURE_INCOMPAT)
		return 0;

	/*
	 * The block count is a 32-bit field, extended to 48 bits when
	 * the INCOMPAT_48BIT feature flag is set.
	 */
	blocks = le32_to_cpu(sb->blocks_lo);
	if (le32_to_cpu(sb->feature_incompat) & EROFS_FEATURE_INCOMPAT_48BIT)
		blocks |= (u64)le16_to_cpu(sb->rb.blocks_hi) << 32;

	/* Reject zero blocks or values that would overflow on shift */
	if (!blocks || blocks > (U64_MAX >> sb->blkszbits))
		return 0;

	/* Verify the computed image size fits within the remaining buffer */
	img_size = blocks << sb->blkszbits;
	if (img_size > len - off)
		return 0;

	return img_size;
}

/*
 * Check whether an EROFS superblock indicates extended attribute
 * metadata and warn once if the kernel lacks the config options
 * needed to honour them through the overlay.
 */
static void __init erofs_initrd_check_xattrs(void *buf)
{
	struct erofs_super_block *sb = buf + EROFS_SUPER_OFFSET;
	static bool warned;

	if (warned)
		return;
	if (!le32_to_cpu(sb->xattr_blkaddr) && !sb->xattr_prefix_count)
		return;

	warned = true;

	if (!IS_ENABLED(CONFIG_EROFS_FS_XATTR)) {
		pr_warn("initrd: EROFS image has extended attributes but CONFIG_EROFS_FS_XATTR is not set; xattrs will be ignored\n");
		return;
	}
	if (!IS_ENABLED(CONFIG_EROFS_FS_SECURITY))
		pr_warn("initrd: EROFS image has extended attributes but CONFIG_EROFS_FS_SECURITY is not set; security labels will be ignored\n");
	if (!IS_ENABLED(CONFIG_TMPFS_XATTR))
		pr_warn("initrd: EROFS image has extended attributes but CONFIG_TMPFS_XATTR is not set; copy-up of xattrs will fail\n");
}

/*
 * Mount an EROFS image segment directly from initrd memory, without
 * going through the block layer.
 */
static int __init mount_erofs_layer(int layer, void *buf, unsigned long size)
{
	char mntpoint[INITRD_LAYER_MNT_MAX];
	int err;

	snprintf(mntpoint, sizeof(mntpoint), "/initrd_layers/%d", layer);
	init_mkdir(mntpoint, 0755);

	erofs_memback_set_pending(buf, size);
	err = init_mount("none", mntpoint, "erofs", MS_RDONLY, NULL);
	if (err)
		pr_err("initrd: EROFS mount for layer %d failed: %d\n", layer,
		       err);
	return err;
}

/*
 * Extract a cpio (or compressed cpio) segment into a tmpfs and seal
 * it read-only.  Reports the number of bytes consumed through @consumed.
 */
static int __init mount_cpio_layer(int layer, char *buf, unsigned long len,
				   unsigned long *consumed)
{
	char mntpoint[INITRD_LAYER_MNT_MAX];
	struct path saved_root, saved_pwd;
	char *err;
	int ret;

	*consumed = 0;

	snprintf(mntpoint, sizeof(mntpoint), "/initrd_layers/%d", layer);
	init_mkdir(mntpoint, 0755);

	ret = init_mount("tmpfs", mntpoint, "tmpfs", 0, NULL);
	if (ret) {
		pr_err("initrd: tmpfs mount for layer %d failed: %d\n", layer,
		       ret);
		return ret;
	}

	get_fs_root(current->fs, &saved_root);
	get_fs_pwd(current->fs, &saved_pwd);

	ret = init_chdir(mntpoint);
	if (ret) {
		pr_err("initrd: chdir to layer %d failed: %d\n", layer, ret);
		goto restore;
	}
	ret = init_chroot(".");
	if (ret) {
		pr_err("initrd: chroot into layer %d failed: %d\n", layer, ret);
		goto restore;
	}

	err = unpack_to_rootfs(buf, len, consumed);
	if (err) {
		pr_err("initrd: cpio layer %d: %s\n", layer, err);
		ret = -EINVAL;
	}

	if (!*consumed)
		ret = -ENODATA;

restore:
	set_fs_root(current->fs, &saved_root);
	set_fs_pwd(current->fs, &saved_pwd);
	path_put(&saved_root);
	path_put(&saved_pwd);

	if (ret)
		goto out_umount;

	init_flush_fput();
	ret = init_mount(mntpoint, mntpoint, NULL, MS_REMOUNT | MS_RDONLY,
			 NULL);
	if (ret) {
		pr_warn("initrd: read-only remount of layer %d failed: %d\n",
			layer, ret);
		goto out_umount;
	}

	return 0;

out_umount:
	init_umount(mntpoint, 0);
	init_rmdir(mntpoint);
	return ret;
}

/*
 * Per-layer: "/initrd_layers/" + up to MAX_INITRD_LAYER_DIGITS + ':'
 * Fixed:     the "upperdir=…,workdir=…,lowerdir=" prefix + '\0'
 */
#define OVLOPT_PER_LAYER                                           \
	(sizeof("/initrd_layers/") - 1 + MAX_INITRD_LAYER_DIGITS + \
	 sizeof(":") - 1)
#define OVLOPT_FIXED                                \
	(sizeof("upperdir=/initrd_layers/rw/upper," \
		"workdir=/initrd_layers/rw/work,"   \
		"lowerdir="))

/*
 * Assemble an overlayfs from the stacked initrd layers with a tmpfs
 * upper layer for writability, mount the result at /root, and pivot
 * the process root into it so subsequent path lookups see the overlay.
 */
static int __init erofs_initrd_assemble_overlay(int nlayers)
{
	char *opts;
	int i, pos, ret;

	if (nlayers <= 0)
		return -ENODEV;

	init_mkdir("/initrd_layers/rw", 0755);
	ret = init_mount("tmpfs", "/initrd_layers/rw", "tmpfs", 0, NULL);
	if (ret)
		return ret;
	init_mkdir("/initrd_layers/rw/upper", 0755);
	init_mkdir("/initrd_layers/rw/work", 0755);

	opts = kmalloc(nlayers * OVLOPT_PER_LAYER + OVLOPT_FIXED, GFP_KERNEL);
	if (!opts)
		return -ENOMEM;

	pos = sprintf(opts, "upperdir=/initrd_layers/rw/upper,"
			    "workdir=/initrd_layers/rw/work,"
			    "lowerdir=");

	for (i = nlayers - 1; i >= 0; i--) {
		if (i < nlayers - 1)
			opts[pos++] = ':';
		pos += sprintf(opts + pos, "/initrd_layers/%d", i);
	}

	init_mkdir("/root", 0700);
	ret = init_mount("overlay", "/root", "overlay", 0, opts);
	kfree(opts);
	if (ret) {
		pr_err("initrd: overlayfs mount failed: %d\n", ret);
		return ret;
	}

	pr_info("initrd: mounted %d layer(s) via overlayfs\n", nlayers);

	/*
	 * Record that the overlay is ready at /root so
	 * initramfs_pivot_root() can chdir/chroot PID 1 into it.
	 * Do NOT pivot here — when the async worker finishes before
	 * kernel_init_freeable() calls wait_for_initramfs(), this
	 * function runs in PID 1's context and a premature pivot
	 * would cause initramfs_pivot_root() to double-pivot into
	 * the overlay's /root home directory instead of the overlay
	 * root itself.
	 */
	erofs_initrd_root_pivoted = true;

	return 0;
}

static void __init erofs_initrd_cleanup(int nlayers)
{
	char mntpoint[INITRD_LAYER_MNT_MAX];
	int i;

	for (i = nlayers - 1; i >= 0; i--) {
		snprintf(mntpoint, sizeof(mntpoint), "/initrd_layers/%d", i);
		init_umount(mntpoint, 0);
		init_rmdir(mntpoint);
	}
	init_umount("/initrd_layers/rw", 0);
	init_rmdir("/initrd_layers/rw");
	init_rmdir("/initrd_layers");
}

/*
 * Lightweight scan to determine whether the initrd contains any EROFS
 * images.  Probes every minimum-block-aligned offset for a valid EROFS
 * superblock using the full try_parse_erofs() validation (magic,
 * blkszbits range, feature flags, block count, and image-size fit).
 *
 * For non-EROFS positions the check exits on the first 4-byte magic
 * comparison, making the scan effectively a sequential memory read at
 * 512-byte strides — typically < 2 ms for a 256 MB initrd.
 */
static bool __init initrd_has_erofs(char *buf, unsigned long len)
{
	unsigned long off;

	for (off = 0; off + EROFS_SB_MINSIZE <= len;
	     off += (1 << EROFS_BLKSZBITS_MIN)) {
		if (try_parse_erofs(buf, off, len))
			return true;
	}
	return false;
}

static int __init erofs_initrd_setup(void)
{
	char *buf = (char *)initrd_start;
	unsigned long len = initrd_end - initrd_start;
	unsigned long offset = 0;
	int layer = 0;
	bool has_erofs = false;
	int ret;

	if (!initrd_has_erofs(buf, len))
		return -ENODEV;

	init_mkdir("/initrd_layers", 0755);

	while (offset < len) {
		unsigned long erofs_size, consumed, back;

		if (layer >= MAX_INITRD_LAYERS) {
			pr_err("initrd: too many layers (max %d)\n",
			       MAX_INITRD_LAYERS);
			break;
		}

		erofs_size = try_parse_erofs(buf, offset, len);
		if (erofs_size) {
			erofs_initrd_check_xattrs(buf + offset);
			ret = mount_erofs_layer(layer, buf + offset,
						erofs_size);
			if (ret)
				goto fail;
			offset += erofs_size;
			layer++;
			has_erofs = true;
			continue;
		}

		consumed = 0;
		ret = mount_cpio_layer(layer, buf + offset, len - offset,
				       &consumed);
		if (!consumed)
			break;
		if (ret)
			goto fail;
		offset += consumed;
		layer++;

		/*
		 * cpio NUL-skipping may have consumed bytes belonging to a
		 * subsequent EROFS image's reserved area (first 1024 bytes,
		 * typically all NULs).  Scan backward to find the true start.
		 */
		for (back = 1; back <= EROFS_SUPER_OFFSET && back <= consumed;
		     back++) {
			if (!IS_ALIGNED(offset - back,
					1 << EROFS_BLKSZBITS_MIN))
				continue;
			if (try_parse_erofs(buf, offset - back, len)) {
				offset -= back;
				break;
			}
		}
	}

	/* Alignment padding is often NUL-filled; do not treat that as junk. */
	while (offset < len && !buf[offset])
		offset++;

	if (offset < len) {
		pr_err("initrd: %lu trailing byte(s) after last layer (offset %lu, len %lu)\n",
		       len - offset, offset, len);
		ret = -EINVAL;
		goto fail;
	}

	if (!layer)
		return -ENODEV;

	/*
	 * No EROFS detected: discard the temporary layers and fall back
	 * to the standard unpack_to_rootfs() path.  That path extracts
	 * into the existing rootfs, preserving any content from the
	 * built-in initramfs (CONFIG_INITRAMFS_SOURCE).
	 */
	if (!has_erofs) {
		erofs_initrd_cleanup(layer);
		return -ENODEV;
	}

	ret = erofs_initrd_assemble_overlay(layer);
	if (ret)
		goto fail;

	if (!do_retain_initrd)
		initrd_start = initrd_end = 0;

	return 0;

fail:
	erofs_initrd_cleanup(layer);
	return ret;
}

#endif

static void __init do_populate_rootfs(void *unused, async_cookie_t cookie)
{
	/* Load the built in initramfs */
	char *err = unpack_to_rootfs(__initramfs_start, __initramfs_size, NULL);
	if (err)
		panic_show_mem("%s", err); /* Failed to decompress INTERNAL initramfs */

	if (!initrd_start || IS_ENABLED(CONFIG_INITRAMFS_FORCE))
		goto done;

#ifdef CONFIG_INITRD_EROFS
	/*
	 * EROFS and overlayfs register at device_initcall (level 6), which
	 * runs AFTER rootfs_initcall.  Since we are executing asynchronously
	 * from rootfs_initcall, those filesystem types are not yet available.
	 *
	 * Always defer when CONFIG_INITRD_EROFS is enabled: the initrd may
	 * contain EROFS images at any position (e.g. after a cpio segment).
	 * erofs_initrd_setup() will iterate all segments and fall back to
	 * the standard unpack path if no EROFS is found.
	 */
	erofs_initrd_deferred = true;
	goto done;
#endif

	if (IS_ENABLED(CONFIG_BLK_DEV_RAM))
		printk(KERN_INFO "Trying to unpack rootfs image as initramfs...\n");
	else
		printk(KERN_INFO "Unpacking initramfs...\n");

	err = unpack_to_rootfs((char *)initrd_start, initrd_end - initrd_start,
			       NULL);
	if (err) {
#ifdef CONFIG_BLK_DEV_RAM
		populate_initrd_image(err);
#else
		printk(KERN_EMERG "Initramfs unpacking failed: %s\n", err);
#endif
	}

done:
#ifdef CONFIG_INITRD_EROFS
	if (erofs_initrd_deferred)
		goto out_flush;
#endif
	security_initramfs_populated();

	/*
	 * If the initrd region is overlapped with crashkernel reserved region,
	 * free only memory that is not part of crashkernel region.
	 */
	if (!do_retain_initrd && initrd_start && !kexec_free_initrd()) {
		free_initrd_mem(initrd_start, initrd_end);
	} else if (do_retain_initrd && initrd_start) {
		bin_attr_initrd.size = initrd_end - initrd_start;
		bin_attr_initrd.private = (void *)initrd_start;
		if (sysfs_create_bin_file(firmware_kobj, &bin_attr_initrd))
			pr_err("Failed to create initrd sysfs file");
	}
	initrd_start = 0;
	initrd_end = 0;

out_flush:
	init_flush_fput();
}

static ASYNC_DOMAIN_EXCLUSIVE(initramfs_domain);
static async_cookie_t initramfs_cookie;

void wait_for_initramfs(void)
{
	if (!initramfs_cookie) {
		/*
		 * Something before rootfs_initcall wants to access
		 * the filesystem/initramfs. Probably a bug. Make a
		 * note, avoid deadlocking the machine, and let the
		 * caller's access fail as it used to.
		 */
		pr_warn_once("wait_for_initramfs() called before rootfs_initcalls\n");
		return;
	}
	async_synchronize_cookie_domain(initramfs_cookie + 1, &initramfs_domain);

#ifdef CONFIG_INITRD_EROFS
	/*
	 * The EROFS setup was deferred from do_populate_rootfs() because
	 * filesystem types (erofs, overlay) register at device_initcall
	 * level 6, after rootfs_initcall.  By the time we get here,
	 * do_initcalls() has completed and all types are available.
	 */
	if (erofs_initrd_deferred) {
		int ret;

		erofs_initrd_deferred = false;
		ret = erofs_initrd_setup();
		if (ret) {
			/*
			 * No EROFS found, or EROFS setup failed.
			 * Fall back to the standard unpack path.
			 */
			char *err;

			if (ret != -ENODEV)
				pr_err("initrd: EROFS setup failed (%d), trying cpio\n",
				       ret);
			pr_info("Unpacking initramfs...\n");
			err = unpack_to_rootfs((char *)initrd_start,
					       initrd_end - initrd_start,
					       NULL);
			if (err)
				pr_emerg("Initramfs unpacking failed: %s\n",
					 err);
		}

		if (!do_retain_initrd && initrd_start &&
		    !kexec_free_initrd())
			free_initrd_mem(initrd_start, initrd_end);
		if (do_retain_initrd && initrd_start) {
			bin_attr_initrd.size = initrd_end - initrd_start;
			bin_attr_initrd.private = (void *)initrd_start;
			if (sysfs_create_bin_file(firmware_kobj,
						  &bin_attr_initrd))
				pr_err("Failed to create initrd sysfs file");
		}
		initrd_start = 0;
		initrd_end = 0;
		security_initramfs_populated();
	}
#endif
}
EXPORT_SYMBOL_GPL(wait_for_initramfs);

/*
 * If the EROFS initrd overlay was assembled on a kworker thread
 * (triggered by an early wait_for_initramfs() from a usermodehelper),
 * PID 1 still has the old root.  Replay the pivot so PID 1 resolves
 * paths (e.g. rdinit=) against the overlay.
 */
void __init initramfs_pivot_root(void)
{
#ifdef CONFIG_INITRD_EROFS
	if (erofs_initrd_root_pivoted) {
		erofs_initrd_root_pivoted = false;
		init_chdir("/root");
		init_chroot(".");

		if (IS_ENABLED(CONFIG_DEVTMPFS)) {
			int ret;

			ret = init_mount("devtmpfs", "/dev", "devtmpfs",
					 MS_SILENT, NULL);
			if (ret)
				pr_warn("initrd: devtmpfs mount on /dev failed: %d\n",
					ret);
		}
	}
#endif
}

static int __init populate_rootfs(void)
{
	initramfs_cookie = async_schedule_domain(do_populate_rootfs, NULL,
						 &initramfs_domain);
	usermodehelper_enable();
	if (!initramfs_async)
		wait_for_initramfs();
	return 0;
}
rootfs_initcall(populate_rootfs);
