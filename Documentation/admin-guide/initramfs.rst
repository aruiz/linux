.. SPDX-License-Identifier: GPL-2.0

==================================
The Initial RAM Filesystem (initramfs)
==================================

Modern Linux systems use an initial RAM filesystem, known as **initramfs**,
to perform early userspace setup before the real root filesystem is mounted.
Despite its name, the mechanism commonly referred to as "initrd" by
bootloaders and distribution tooling is almost always an initramfs image on
any kernel 2.6 or later.

This document describes how initramfs works from a system administrator's
perspective.  For kernel internals and developer details, see
Documentation/filesystems/ramfs-rootfs-initramfs.rst.  For the legacy RAM
disk mechanism that initramfs replaced, see Documentation/admin-guide/initrd.rst.


Terminology
-----------

The naming around early boot images is a persistent source of confusion:

**initrd** (initial RAM disk)
  The legacy mechanism (Linux 2.4 and earlier) that loaded a filesystem image
  into a RAM block device (``/dev/ram0``).  The kernel mounted this block
  device as the temporary root, ran ``/linuxrc``, and then performed a
  ``pivot_root`` to the real root filesystem.  This required a filesystem
  driver (e.g., ext2) and a block device driver to be compiled into the
  kernel, and involved an unnecessary copy through the block layer.

**initramfs** (initial RAM filesystem)
  The modern mechanism (Linux 2.6+).  A cpio archive (optionally compressed)
  is extracted directly into a tmpfs instance (rootfs) during boot.  No block
  device or filesystem driver is needed for the archive itself.  The ``/init``
  program in the archive takes full responsibility for finding and mounting the
  real root filesystem.

**The naming problem**: Bootloader configuration files (GRUB, LILO, U-Boot,
etc.), kernel boot parameters (``initrd=``, ``initrdmem=``), device tree
properties, and distribution tools (``dracut``, ``mkinitramfs``,
``mkinitcpio``) all continue to use the term "initrd" even though the actual
image format has been cpio-based initramfs for over two decades.  The kernel
itself uses ``initrd_start`` / ``initrd_end`` for the memory region regardless
of format; the autodetection happens later.

When this document or the kernel source refers to an "initrd image", it
almost always means a cpio-based initramfs image unless explicitly stated
otherwise.  True initrd (block-device-backed) images require
``CONFIG_BLK_DEV_RAM`` and are essentially a compatibility mechanism at this
point.


Why initramfs replaced initrd
------------------------------

The original initrd mechanism suffered from several limitations:

1. **Double caching**: Data was copied into the RAM block device and then
   again into the page cache when files were accessed, wasting memory and
   bus bandwidth.

2. **Fixed size**: The RAM disk had to be created with a predetermined size.
   Too small and the image would not fit; too large and memory was wasted.

3. **Required a filesystem driver**: The initrd image had to contain a
   recognized filesystem (ext2, minix, etc.), which meant the corresponding
   driver had to be compiled into the kernel.

4. **Clumsy root transition**: The ``change_root`` / ``pivot_root`` dance
   required careful ordering and left edge cases around mounted filesystems
   and open file descriptors.

initramfs solved all of these by extracting a cpio archive directly into the
VFS cache (rootfs/tmpfs), eliminating the block layer entirely.  The archive
format is trivial (``newc`` cpio), self-contained extraction code is small
enough to be ``__init`` (discarded after boot), and the resulting filesystem
grows and shrinks dynamically like any tmpfs.


How it works
------------

The boot sequence involving initramfs proceeds as follows:

1. The bootloader loads the kernel and the initramfs image into memory and
   passes the image's location to the kernel (via platform-specific means:
   device tree ``linux,initrd-start``/``linux,initrd-end`` properties,
   boot protocol fields, or ``initrd=``/``initrdmem=`` parameters).

2. Early in boot, the kernel mounts an empty tmpfs (or ramfs) as rootfs at
   ``/``.

3. The kernel extracts the **built-in initramfs** — a cpio archive linked into
   the kernel image at build time via ``CONFIG_INITRAMFS_SOURCE``.  This
   archive is always present; when ``CONFIG_INITRAMFS_SOURCE`` is empty, it
   contains only a minimal default (a ``/dev`` directory and ``/root``
   directory, totaling ~134 bytes on x86).

4. If the bootloader provided an **external initramfs image** and
   ``CONFIG_INITRAMFS_FORCE`` is not set, the kernel first checks for
   alternative image formats (e.g., EROFS when ``CONFIG_INITRD_EROFS`` is
   enabled).  If an alternative format is detected, processing is deferred to
   ``prepare_namespace()``; see `EROFS-formatted initrd images`_ below.
   Otherwise, the kernel attempts to unpack the image as a cpio archive into
   the same rootfs, overlaying the built-in contents.  Files from the external
   image overwrite any conflicting built-in files.

5. If the external image cannot be parsed as a cpio archive and
   ``CONFIG_BLK_DEV_RAM`` is enabled, the kernel falls back to the legacy
   initrd path: the image is written to ``/dev/ram0`` and handled as a
   traditional initrd.

6. If rootfs contains ``/init``, the kernel executes it as PID 1 (or
   whatever ``rdinit=`` specifies).  This init process is responsible for
   mounting the real root filesystem, performing any early setup, and
   eventually switching root (typically via ``switch_root`` or equivalent).

7. If rootfs does **not** contain ``/init``, the kernel falls through to
   ``prepare_namespace()`` — the legacy path that directly mounts the device
   specified by ``root=`` and executes ``/sbin/init`` (or whatever ``init=``
   specifies).


Providing an initramfs image
-----------------------------

There are three ways to provide an initramfs to the kernel:

**Built into the kernel image** (``CONFIG_INITRAMFS_SOURCE``)
  The build system generates a cpio archive from the specified source
  (a directory tree, a cpio file list, or a pre-built ``.cpio`` archive) and
  links it into the kernel binary.  This is always present; an empty default
  is used when the config option is blank.  See ``usr/Kconfig`` for details.

**Loaded by the bootloader** (external image)
  The bootloader loads a separate file into memory and tells the kernel where
  it is.  This is by far the most common method on distribution systems.
  The image is generated by tools such as ``dracut`` (Fedora, RHEL, SUSE),
  ``mkinitramfs`` / ``update-initramfs`` (Debian, Ubuntu), or ``mkinitcpio``
  (Arch Linux).

**Both**
  The built-in archive is extracted first, then the external image is overlaid
  on top.  This is useful for embedding a minimal rescue environment into the
  kernel while allowing distributions to augment it.


Image format
------------

The kernel expects a cpio archive in **newc** (SVR4 with no CRC, or
optionally with CRC) format.  The archive may be compressed with any
algorithm the kernel has decompressor support for: gzip, bzip2, LZMA, XZ,
LZO, LZ4, or Zstandard.  The kernel autodetects the compression format.

Creating an image manually::

    # From a directory tree:
    (cd /path/to/rootfs && find . | cpio -o -H newc) | gzip > initramfs.img

    # Or with dracut (Fedora/RHEL):
    dracut /boot/initramfs-$(uname -r).img $(uname -r)

    # Or with mkinitramfs (Debian/Ubuntu):
    mkinitramfs -o /boot/initrd.img-$(uname -r) $(uname -r)

Inspecting an existing image::

    # Detect compression and list contents:
    zcat /boot/initramfs.img | cpio -t

    # Or for images compressed with other algorithms:
    lsinitrd /boot/initramfs.img          # dracut-based systems
    lsinitramfs /boot/initrd.img          # Debian-based systems


Concatenated cpio archives
--------------------------

Multiple cpio archives (each optionally compressed independently) can be
concatenated together to form a single initramfs image.  The kernel unpacks
them sequentially; later archives overlay earlier ones, so files from a later
archive overwrite files from an earlier one at the same path.

This is the mechanism used by several kernel features:

- **Boot config** (``CONFIG_BOOT_CONFIG``): A boot configuration file is
  appended to the initramfs as a separate cpio member, along with a trailer
  containing the size and a magic number.

- **CPU microcode early loading**: Architecture-specific microcode updates
  can be prepended to the initramfs as an uncompressed cpio archive.  The
  kernel extracts the microcode in the very early boot stages, before the main
  initramfs is unpacked.  See Documentation/arch/x86/microcode.rst.

- **Distribution customization**: Administrators can overlay site-specific
  files onto a distribution-generated initramfs by concatenating a second
  archive.


Boot parameters
---------------

The following kernel command line parameters affect initramfs behavior:

``initrd=<path>``
  (Bootloader-interpreted) Specifies the file to load as the initramfs image.

``initrdmem=<address>,<size>``
  Specifies a physical memory region containing the initramfs image.  Useful
  for kexec and firmware-loaded scenarios.

``noinitrd``
  Tells the kernel not to process the bootloader-provided initrd/initramfs
  image.  Note: this does **not** affect the built-in initramfs.

``rdinit=<path>``
  Specifies the init program to execute from the initramfs (default:
  ``/init``).  Analogous to ``init=`` for the real root filesystem.

``initramfs_async=<bool>``
  Controls whether initramfs unpacking runs asynchronously, concurrently
  with device probing.  Default is ``1`` (async).  Set to ``0`` to force
  synchronous unpacking, which ensures the initramfs is fully unpacked before
  device and late initcalls run.

``retain_initrd``
  Keeps the initramfs memory region after extraction rather than freeing it.
  The raw image data is then accessible via ``/sys/firmware/initrd``.

``keepinitrd``
  Architecture-specific alias for ``retain_initrd`` (ARM).

``initramfs_options=<opts>``
  Mount options applied to the rootfs tmpfs (e.g., ``size=50%``).

``rootfstype=ramfs``
  Forces rootfs to use ramfs instead of tmpfs (tmpfs is the default when
  ``CONFIG_TMPFS`` is enabled).

See Documentation/admin-guide/kernel-parameters.txt for the complete and
authoritative list.


EROFS-formatted initrd images
-----------------------------

When ``CONFIG_INITRD_EROFS`` is enabled, the kernel can accept EROFS
filesystem images in the initrd region instead of cpio archives.  This
provides a read-only, compressed root filesystem that is mounted directly
from initrd memory using the erofs memory-backed mode
(``CONFIG_EROFS_FS_BACKED_BY_MEM``), without any block device and without
unpacking.

The kernel detects the EROFS magic (``0xE0F5E1E2``) at the expected
superblock offset and, if found, defers processing to ``prepare_namespace()``
rather than attempting cpio extraction.

The initrd may contain an arbitrary mix of cpio archives and EROFS images in
any order.  The kernel scans the initrd left-to-right, identifying each
contiguous segment as either a cpio archive (uncompressed newc header) or an
EROFS image (superblock magic at offset 1024).  NUL padding between segments
(e.g., cpio block alignment to 512-byte boundaries) is skipped
automatically.

Each segment becomes a separate overlayfs lower layer, preserved in order:

- **EROFS segments** are mounted directly from initrd memory via the
  memory-backed mode — zero copy, no extraction.
- **cpio segments** are extracted into a per-layer tmpfs.  Both uncompressed
  and compressed cpios are supported (the kernel auto-detects the
  compression format).
- A final tmpfs is mounted as the overlayfs upper layer, making the combined
  root filesystem writable (writes are stored in RAM and lost on reboot).

Example layouts::

    [erofs]                                  # single erofs image
    [microcode cpio] [erofs]                 # early microcode + erofs
    [erofs] [erofs]                          # layered erofs images
    [cpio] [erofs] [cpio]                    # dracut-style overlay
    [erofs] [cpio] [erofs]                   # interleaved

A leading cpio archive is additionally extracted to the initial rootfs
during ``populate_rootfs``, before the erofs path defers to
``prepare_namespace``.  This supports early-boot consumers that scan the
raw initrd for files (e.g., CPU microcode loading), though those consumers
actually read from the raw memory region and do not depend on the rootfs
extraction.

Data that is neither a valid cpio header nor an EROFS superblock is treated
as a compressed cpio (whose boundary cannot be determined without
decompression) and forms the final segment.

If no EROFS image is found, the entire initrd is handled by the traditional
``unpack_to_rootfs`` cpio path — no overlayfs is involved and behavior is
identical to an unpatched kernel.

The initrd pages backing EROFS images are inserted into the erofs page
cache and the initial boot references are dropped.  When the filesystem is
unmounted, the pages are freed to the buddy allocator — no explicit discard
step is needed.  Pages backing cpio segments are freed after extraction.

This is useful for systems that need a compact, directly-mountable root
filesystem without the overhead of cpio extraction, particularly in embedded
and appliance scenarios.  It also allows layered image composition, where a
base OS image can be extended or patched by concatenating additional EROFS
images.

Creating an EROFS initrd image::

    mkfs.erofs initrd.erofs /path/to/rootfs

    # Multiple layers (concatenated):
    mkfs.erofs base.erofs /path/to/base
    mkfs.erofs overlay.erofs /path/to/overlay
    cat base.erofs overlay.erofs > initrd.erofs

    # With microcode prefix:
    cat microcode.cpio base.erofs > initrd.erofs

    # Mixed cpio/erofs (e.g. dracut overlay after erofs base):
    cat base.erofs dracut-overlay.cpio > initrd.img

    # Full mix: microcode + erofs base + cpio config overlay:
    cat microcode.cpio base.erofs site-config.cpio > initrd.img


Memory management
-----------------

After the initramfs cpio archive is extracted, the kernel frees the original
memory region occupied by the compressed image (unless ``retain_initrd`` was
specified).  The extracted files live in the rootfs tmpfs/ramfs, consuming
memory proportional to their size.

When the init process switches to the real root filesystem (typically via
``switch_root``), all files in the initramfs are deleted and the memory is
reclaimed.  ``switch_root`` accomplishes this by recursively deleting
everything in the old root before pivoting.

If rootfs uses tmpfs (the default), its pages can also be swapped out under
memory pressure once swap is available, though this rarely matters in
practice since initramfs contents are typically deleted before the real
root is mounted.


Relationship to other documentation
------------------------------------

Documentation/admin-guide/initrd.rst
  Documents the legacy initrd (RAM disk) mechanism.  Largely of historical
  interest; the "Compressed cpio images" section in that document was an
  early acknowledgment of the transition to initramfs.

Documentation/filesystems/ramfs-rootfs-initramfs.rst
  Developer-oriented document covering the internals of ramfs, rootfs, and
  the initramfs extraction mechanism, including the rationale for cpio over
  tar.

Documentation/driver-api/early-userspace/early_userspace_support.rst
  Developer documentation for the early userspace build infrastructure
  (``gen_init_cpio``, ``CONFIG_INITRAMFS_SOURCE``, etc.).

Documentation/driver-api/early-userspace/buffer-format.rst
  Specification of the cpio ``newc`` archive format used by initramfs.
