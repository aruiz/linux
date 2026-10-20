// SPDX-License-Identifier: GPL-2.0
/*
 * EFI Mode Linux - UEFI application entry point
 *
 * This is the C entry point called from the PE trampoline in head.S.
 * It receives the EFI handle and system table, prints a greeting via
 * the UEFI console, and halts.
 */

#include <linux/types.h>
#include <linux/efi.h>

/*
 * EFI Simple Text Output Protocol - minimal definition for direct use.
 * The full union is in efistub.h but we avoid pulling that in.
 */
struct eml_simple_text_output {
	void *reset;
	efi_status_t(__attribute__((ms_abi)) *
		     output_string)(struct eml_simple_text_output *this,
				    efi_char16_t *string);
	void *test_string;
};

/*
 * Minimal system table view with the fields we access directly.
 */
struct eml_system_table {
	efi_table_hdr_t hdr;
	unsigned long fw_vendor;
	u32 fw_revision;
	unsigned long con_in_handle;
	void *con_in;
	unsigned long con_out_handle;
	struct eml_simple_text_output *con_out;
	/* remaining fields omitted */
};

/* Global system table pointer, used by all EML subsystems */
struct eml_system_table *eml_systab;
efi_handle_t eml_image_handle;

static void eml_serial_putc(char c)
{
	u8 status;

	do {
		asm volatile("inb %1, %0" : "=a"(status) : "Nd"((u16)0x3fd));
	} while (!(status & 0x20));
	asm volatile("outb %0, %1" : : "a"(c), "Nd"((u16)0x3f8));
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

/*
 * eml_efi_entry - UEFI application entry (called from head.S)
 * @handle:  firmware image handle
 * @systab:  pointer to the EFI System Table
 *
 * Uses MS x64 ABI: handle in %rcx, systab in %rdx.
 */
efi_status_t eml_efi_entry(efi_handle_t handle, void *systab)
{
	eml_systab = (struct eml_system_table *)systab;
	eml_image_handle = handle;

	/* Print banner via serial port and UEFI console */
	eml_serial_puts("Hello from EFI Mode Linux\r\n");

	if (eml_systab->con_out)
		eml_systab->con_out->output_string(eml_systab->con_out, banner);

	/* Halt - later commits will call start_kernel() here */
	for (;;)
		asm volatile("hlt");

	return 0; /* unreachable */
}
