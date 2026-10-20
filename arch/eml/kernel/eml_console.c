// SPDX-License-Identifier: GPL-2.0
#include <linux/console.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>

#define EML_SERIAL_PORT 0x3f8
#define EML_SERIAL_LSR 0x3fd

static void eml_serial_putchar(char c)
{
	u8 status;

	do {
		asm volatile("inb %1, %0"
			     : "=a"(status)
			     : "Nd"((u16)EML_SERIAL_LSR));
	} while (!(status & 0x20));
	asm volatile("outb %0, %1" : : "a"(c), "Nd"((u16)EML_SERIAL_PORT));
}

static void eml_console_write(struct console *co, const char *s,
			      unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (s[i] == '\n')
			eml_serial_putchar('\r');
		eml_serial_putchar(s[i]);
	}
}

static struct console eml_early_console = {
	.name = "eml",
	.write = eml_console_write,
	.flags = CON_PRINTBUFFER | CON_BOOT,
	.index = -1,
};

static struct console eml_console = {
	.name = "eml",
	.write = eml_console_write,
	.flags = CON_PRINTBUFFER,
	.index = 0,
};

void __init eml_setup_early_console(void)
{
	register_console(&eml_early_console);
}

static int __init eml_console_init(void)
{
	register_console(&eml_console);
	return 0;
}
console_initcall(eml_console_init);
