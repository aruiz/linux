// SPDX-License-Identifier: GPL-2.0
/*
 * EML UEFI chainloader — loads a Linux kernel (with EFI stub) and
 * optional initramfs from the EFI System Partition, then boots it
 * via UEFI LoadImage/StartImage.
 *
 * Command line parameters:
 *   eml.kernel=<path>    Kernel file on the ESP (required to activate)
 *   eml.initrd=<path>    Initramfs file on the ESP (optional)
 *   eml.bootargs=<args>  Command line for the loaded kernel (to end of string)
 */
#include <linux/init.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <asm/setup.h>
#include <asm/eml_efi.h>

#define EFI_LOADER_DATA 2
#define EFI_NATIVE_INTERFACE 0

#define LINUX_EFI_INITRD_MEDIA_GUID                                        \
	EFI_GUID(0x5568e427, 0x68fc, 0x4f3d, 0xac, 0x74, 0xca, 0x55, 0x52, \
		 0x31, 0xcc, 0x68)

#define EFI_LOAD_FILE2_PROTOCOL_GUID                                       \
	EFI_GUID(0x4006c0c1, 0xfcb3, 0x403e, 0x99, 0x6d, 0x4a, 0x6c, 0x87, \
		 0x24, 0xe0, 0x6d)

#define EFI_DEVICE_PATH_PROTOCOL_GUID                                      \
	EFI_GUID(0x09576e91, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, \
		 0x69, 0x72, 0x3b)

struct eml_load_file2 {
	efi_status_t(MSABI *load_file)(struct eml_load_file2 *this,
				       void *file_path, int boot_policy,
				       unsigned long *buffer_size,
				       void *buffer);
};

struct eml_initrd_dev_path {
	struct {
		u8 type;
		u8 sub_type;
		u16 length;
		efi_guid_t guid;
	} vendor;
	struct {
		u8 type;
		u8 sub_type;
		u16 length;
	} end;
} __packed;

static void *initrd_buf;
static unsigned long initrd_len;
static efi_handle_t initrd_handle;
static struct eml_initrd_dev_path initrd_dp;
static struct eml_load_file2 initrd_lf2;

static char boot_kernel[256];
static char boot_initrd[256];
static char boot_args[COMMAND_LINE_SIZE];

static efi_status_t MSABI eml_initrd_load_file(struct eml_load_file2 *this,
					       void *file_path, int boot_policy,
					       unsigned long *buffer_size,
					       void *buffer)
{
	if (!buffer_size)
		return EFI_INVALID_PARAMETER;
	if (boot_policy)
		return EFI_UNSUPPORTED;
	if (!buffer || *buffer_size < initrd_len) {
		*buffer_size = initrd_len;
		return EFI_BUFFER_TOO_SMALL;
	}
	memcpy(buffer, initrd_buf, initrd_len);
	*buffer_size = initrd_len;
	return EFI_SUCCESS;
}

static void __init parse_boot_params(const char *cmdline)
{
	const char *p;
	int i;

	p = strstr(cmdline, "eml.kernel=");
	if (p) {
		p += 11;
		for (i = 0; *p && *p != ' ' && i < (int)sizeof(boot_kernel) - 1;
		     p++, i++)
			boot_kernel[i] = *p;
		boot_kernel[i] = '\0';
	}

	p = strstr(cmdline, "eml.initrd=");
	if (p) {
		p += 11;
		for (i = 0; *p && *p != ' ' && i < (int)sizeof(boot_initrd) - 1;
		     p++, i++)
			boot_initrd[i] = *p;
		boot_initrd[i] = '\0';
	}

	p = strstr(cmdline, "eml.bootargs=");
	if (p) {
		p += 13;
		strscpy(boot_args, p, sizeof(boot_args));
	}
}

static void path_to_ucs2(const char *path, efi_char16_t *ucs2, int max)
{
	int i;

	for (i = 0; i < max - 1 && path[i]; i++)
		ucs2[i] = path[i] == '/' ? L'\\' : path[i];
	ucs2[i] = 0;
}

static void ascii_to_ucs2(const char *s, efi_char16_t *ucs2, int max)
{
	int i;

	for (i = 0; i < max - 1 && s[i]; i++)
		ucs2[i] = s[i];
	ucs2[i] = 0;
}

static efi_status_t __init eml_read_esp_file(const char *path, void **buf,
					     unsigned long *size)
{
	efi_guid_t fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	efi_guid_t info_guid = EFI_FILE_INFO_ID;
	struct eml_simple_file_system *fs;
	struct eml_file_protocol *root, *file;
	efi_handle_t *handles = NULL;
	unsigned long num_handles = 0;
	efi_char16_t ucs2_path[256];
	u8 info_buf[sizeof(struct eml_file_info) + 512];
	struct eml_file_info *info = (struct eml_file_info *)info_buf;
	unsigned long info_size, read_size;
	efi_status_t status;

	status = eml_systab->boot_services->locate_handle_buffer(
		EFI_LOCATE_BY_PROTOCOL, &fs_guid, NULL, &num_handles, &handles);
	if (status != EFI_SUCCESS || num_handles == 0)
		return EFI_NOT_FOUND;

	status = eml_systab->boot_services->handle_protocol(
		handles[0], &fs_guid, (void **)&fs);
	eml_systab->boot_services->free_pool(handles);
	if (status != EFI_SUCCESS)
		return status;

	status = fs->open_volume(fs, &root);
	if (status != EFI_SUCCESS)
		return status;

	path_to_ucs2(path, ucs2_path, ARRAY_SIZE(ucs2_path));
	status = root->open(root, &file, ucs2_path, EFI_FILE_MODE_READ, 0);
	if (status != EFI_SUCCESS) {
		root->close(root);
		return status;
	}
	root->close(root);

	info_size = sizeof(info_buf);
	status = file->get_info(file, &info_guid, &info_size, info);
	if (status != EFI_SUCCESS) {
		file->close(file);
		return status;
	}

	*size = info->file_size;

	{
		unsigned long pages = (*size + PAGE_SIZE - 1) / PAGE_SIZE;
		u64 addr;

		status = eml_systab->boot_services->allocate_pages(
			EFI_ALLOCATE_ANY_PAGES, EFI_LOADER_DATA, pages, &addr);
		if (status != EFI_SUCCESS) {
			file->close(file);
			return status;
		}
		*buf = (void *)addr;
	}

	read_size = *size;
	status = file->read(file, &read_size, *buf);
	file->close(file);
	if (status != EFI_SUCCESS) {
		unsigned long pages = (*size + PAGE_SIZE - 1) / PAGE_SIZE;

		eml_systab->boot_services->free_pages((u64)(unsigned long)*buf,
						      pages);
		*buf = NULL;
		return status;
	}

	return EFI_SUCCESS;
}

static efi_status_t __init eml_install_initrd_protocol(void)
{
	efi_guid_t dp_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
	efi_guid_t lf2_guid = EFI_LOAD_FILE2_PROTOCOL_GUID;
	efi_guid_t initrd_guid = LINUX_EFI_INITRD_MEDIA_GUID;
	efi_status_t status;

	initrd_dp.vendor.type = 4;
	initrd_dp.vendor.sub_type = 3;
	initrd_dp.vendor.length = 20;
	memcpy(&initrd_dp.vendor.guid, &initrd_guid, sizeof(efi_guid_t));
	initrd_dp.end.type = 0x7F;
	initrd_dp.end.sub_type = 0xFF;
	initrd_dp.end.length = 4;

	initrd_lf2.load_file = eml_initrd_load_file;

	initrd_handle = NULL;
	status = eml_systab->boot_services->install_protocol_interface(
		&initrd_handle, &dp_guid, EFI_NATIVE_INTERFACE, &initrd_dp);
	if (status != EFI_SUCCESS)
		return status;

	status = eml_systab->boot_services->install_protocol_interface(
		&initrd_handle, &lf2_guid, EFI_NATIVE_INTERFACE, &initrd_lf2);

	return status;
}

void __init eml_boot_kernel(void)
{
	extern char eml_cmdline[];
	efi_guid_t li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	struct eml_loaded_image *li;
	void *kernel_buf = NULL;
	unsigned long kernel_size = 0;
	efi_handle_t image_handle = NULL;
	efi_char16_t *cmdline_ucs2;
	unsigned long cmdline_len;
	efi_status_t status;

	parse_boot_params(eml_cmdline);

	if (!boot_kernel[0])
		return;

	pr_info("EML boot: loading kernel from %s\n", boot_kernel);

	status = eml_read_esp_file(boot_kernel, &kernel_buf, &kernel_size);
	if (status != EFI_SUCCESS) {
		pr_err("EML boot: failed to read kernel: 0x%lx\n",
		       (unsigned long)status);
		return;
	}
	pr_info("EML boot: kernel loaded, %lu bytes\n", kernel_size);

	if (boot_initrd[0]) {
		pr_info("EML boot: loading initrd from %s\n", boot_initrd);
		status = eml_read_esp_file(boot_initrd, &initrd_buf,
					   &initrd_len);
		if (status == EFI_SUCCESS) {
			pr_info("EML boot: initrd loaded, %lu bytes\n",
				initrd_len);
			status = eml_install_initrd_protocol();
			if (status != EFI_SUCCESS)
				pr_err("EML boot: initrd protocol failed: 0x%lx\n",
				       (unsigned long)status);
		} else {
			pr_err("EML boot: failed to read initrd: 0x%lx\n",
			       (unsigned long)status);
		}
	}

	status = eml_systab->boot_services->load_image(0, eml_image_handle,
						       NULL, kernel_buf,
						       kernel_size,
						       &image_handle);
	if (status != EFI_SUCCESS) {
		pr_err("EML boot: LoadImage failed: 0x%lx (no EFI stub?)\n",
		       (unsigned long)status);
		goto cleanup;
	}

	if (boot_args[0]) {
		status = eml_systab->boot_services->handle_protocol(
			image_handle, &li_guid, (void **)&li);
		if (status == EFI_SUCCESS) {
			cmdline_len = strlen(boot_args);
			status = eml_systab->boot_services->allocate_pool(
				EFI_LOADER_DATA,
				(cmdline_len + 1) * sizeof(efi_char16_t),
				(void **)&cmdline_ucs2);
			if (status == EFI_SUCCESS) {
				ascii_to_ucs2(boot_args, cmdline_ucs2,
					      cmdline_len + 1);
				li->load_options = cmdline_ucs2;
				li->load_options_size =
					cmdline_len * sizeof(efi_char16_t);
			}
		}
	}

	pr_info("EML boot: starting kernel...\n");
	status = eml_systab->boot_services->start_image(image_handle, NULL,
							NULL);
	pr_err("EML boot: StartImage returned: 0x%lx\n", (unsigned long)status);

cleanup:
	if (kernel_buf) {
		unsigned long pages = (kernel_size + PAGE_SIZE - 1) / PAGE_SIZE;
		eml_systab->boot_services->free_pages(
			(u64)(unsigned long)kernel_buf, pages);
	}
}
