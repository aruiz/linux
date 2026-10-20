// SPDX-License-Identifier: GPL-2.0
#include <linux/types.h>
#include <linux/efi.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/start_kernel.h>
#include <asm/eml_efi.h>
#include <asm/setup.h>

struct eml_system_table *eml_systab;
efi_handle_t eml_image_handle;

char eml_cmdline[COMMAND_LINE_SIZE];

#define EML_SERIAL_PORT 0x3f8
#define EML_SERIAL_LSR 0x3fd

static void eml_serial_putc(char c)
{
	u8 status;

	do {
		asm volatile("inb %1, %0"
			     : "=a"(status)
			     : "Nd"((u16)EML_SERIAL_LSR));
	} while (!(status & 0x20));
	asm volatile("outb %0, %1" : : "a"(c), "Nd"((u16)EML_SERIAL_PORT));
}

static void eml_serial_puts(const char *s)
{
	while (*s)
		eml_serial_putc(*s++);
}

static efi_char16_t banner[] = { 'H', 'e', 'l', 'l', 'o',  ' ',	 'f',
				 'r', 'o', 'm', ' ', 'E',  'F',	 'I',
				 ' ', 'M', 'o', 'd', 'e',  ' ',	 'L',
				 'i', 'n', 'u', 'x', '\r', '\n', 0 };

void __init eml_parse_command_line(void)
{
	efi_guid_t li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	struct eml_loaded_image *li;
	efi_status_t status;
	efi_char16_t *opts;
	u32 opts_size;
	int i, len;
	char *dst;

	status = eml_systab->boot_services->handle_protocol(
		eml_image_handle, &li_guid, (void **)&li);
	if (status != EFI_SUCCESS) {
		pr_info("EML: Loaded Image Protocol not available\n");
		return;
	}

	opts = li->load_options;
	opts_size = li->load_options_size;
	if (!opts || opts_size == 0)
		return;

	len = opts_size / sizeof(efi_char16_t);
	if (len >= COMMAND_LINE_SIZE)
		len = COMMAND_LINE_SIZE - 1;

	dst = eml_cmdline;
	for (i = 0; i < len && opts[i]; i++)
		*dst++ = (char)opts[i];
	*dst = '\0';

	/* Skip the binary path (first word) if present */
	dst = strchr(eml_cmdline, ' ');
	if (dst) {
		dst++;
		memmove(eml_cmdline, dst, strlen(dst) + 1);
	} else {
		eml_cmdline[0] = '\0';
	}
}

efi_status_t __init eml_efi_entry(efi_handle_t handle, void *systab)
{
	eml_systab = (struct eml_system_table *)systab;
	eml_image_handle = handle;

	eml_serial_puts("Hello from EFI Mode Linux\r\n");

	if (eml_systab->con_out)
		eml_systab->con_out->output_string(eml_systab->con_out, banner);

	start_kernel();

	for (;;)
		asm volatile("hlt");

	return 0;
}
