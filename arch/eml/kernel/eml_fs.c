// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/string.h>
#include <asm/eml_efi.h>

static void eml_print_ucs2(const efi_char16_t *s, char *buf, int buflen)
{
	int i;

	for (i = 0; i < buflen - 1 && s[i]; i++)
		buf[i] = (char)s[i];
	buf[i] = '\0';
}

static void eml_list_directory(struct eml_file_protocol *dir, const char *label)
{
	u8 buf[512];
	unsigned long size;
	efi_status_t status;
	char name[128];
	int count = 0;

	pr_info("EML: ESP %s:\n", label);

	for (;;) {
		size = sizeof(buf);
		status = dir->read(dir, &size, buf);
		if (status != EFI_SUCCESS || size == 0)
			break;

		struct eml_file_info *info = (struct eml_file_info *)buf;

		eml_print_ucs2(info->file_name, name, sizeof(name));

		if (info->attribute & EFI_FILE_DIRECTORY)
			pr_info("  %s/\n", name);
		else
			pr_info("  %s (%llu bytes)\n", name, info->file_size);
		count++;
	}

	if (count == 0)
		pr_info("  (empty)\n");
}

static int __init eml_fs_init(void)
{
	efi_guid_t fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	efi_handle_t *handles = NULL;
	unsigned long num_handles = 0;
	efi_status_t status;
	unsigned long i;

	status = eml_systab->boot_services->locate_handle_buffer(
		EFI_LOCATE_BY_PROTOCOL, &fs_guid, NULL, &num_handles, &handles);

	if (status != EFI_SUCCESS || num_handles == 0) {
		pr_info("EML: no EFI file systems found\n");
		return 0;
	}

	pr_info("EML: found %lu EFI file system(s)\n", num_handles);

	for (i = 0; i < num_handles; i++) {
		struct eml_simple_file_system *fs;
		struct eml_file_protocol *root;
		char label[32];

		status = eml_systab->boot_services->handle_protocol(
			handles[i], &fs_guid, (void **)&fs);
		if (status != EFI_SUCCESS)
			continue;

		status = fs->open_volume(fs, &root);
		if (status != EFI_SUCCESS) {
			pr_err("EML: OpenVolume failed on fs %lu: 0x%lx\n", i,
			       (unsigned long)status);
			continue;
		}

		snprintf(label, sizeof(label), "volume %lu root", i);
		eml_list_directory(root, label);
		root->close(root);
	}

	if (handles)
		eml_systab->boot_services->free_pool(handles);

	return 0;
}
device_initcall(eml_fs_init);
