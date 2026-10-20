// SPDX-License-Identifier: GPL-2.0
#include <linux/console.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/types.h>
#include <asm/eml_efi.h>

#define EML_SERIAL_PORT 0x3f8
#define EML_SERIAL_LSR 0x3fd

static struct tty_driver *eml_tty_driver;
static struct tty_port eml_tty_port;

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

static struct tty_driver *eml_console_device(struct console *co, int *index)
{
	*index = 0;
	return eml_tty_driver;
}

static struct console eml_early_console = {
	.name = "eml",
	.write = eml_console_write,
	.flags = CON_PRINTBUFFER | CON_BOOT,
	.index = -1,
};

static struct console eml_console = {
	.name = "ttyEML",
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

static int eml_tty_open(struct tty_struct *tty, struct file *filp)
{
	return tty_port_open(&eml_tty_port, tty, filp);
}

static void eml_tty_close(struct tty_struct *tty, struct file *filp)
{
	tty_port_close(&eml_tty_port, tty, filp);
}

static ssize_t eml_tty_write(struct tty_struct *tty, const u8 *buf,
			     size_t count)
{
	size_t i;

	for (i = 0; i < count; i++) {
		if (buf[i] == '\n')
			eml_serial_putchar('\r');
		eml_serial_putchar(buf[i]);
	}
	return count;
}

static unsigned int eml_tty_write_room(struct tty_struct *tty)
{
	return 4096;
}

static const struct tty_operations eml_tty_ops = {
	.open = eml_tty_open,
	.close = eml_tty_close,
	.write = eml_tty_write,
	.write_room = eml_tty_write_room,
};

static int eml_tty_port_activate(struct tty_port *port, struct tty_struct *tty)
{
	return 0;
}

static const struct tty_port_operations eml_tty_port_ops = {
	.activate = eml_tty_port_activate,
};

void eml_kbd_poll(void)
{
	struct efi_input_key key;
	efi_status_t status;
	int pushed = 0;

	for (;;) {
		status = eml_systab->con_in->read_key_stroke(eml_systab->con_in,
							     &key);
		if (status != EFI_SUCCESS)
			break;

		if (key.unicode_char) {
			char c = (char)key.unicode_char;

			if (c == '\r')
				c = '\n';
			tty_insert_flip_char(&eml_tty_port, c, TTY_NORMAL);
			pushed++;
		}
	}

	if (pushed)
		tty_flip_buffer_push(&eml_tty_port);
}

static int __init eml_tty_init(void)
{
	struct tty_driver *driver;
	int ret;

	driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);
	if (IS_ERR(driver))
		return PTR_ERR(driver);

	driver->driver_name = "eml_serial";
	driver->name = "ttyEML";
	driver->type = TTY_DRIVER_TYPE_SERIAL;
	driver->subtype = SERIAL_TYPE_NORMAL;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;

	tty_set_operations(driver, &eml_tty_ops);

	tty_port_init(&eml_tty_port);
	eml_tty_port.ops = &eml_tty_port_ops;
	tty_port_link_device(&eml_tty_port, driver, 0);

	ret = tty_register_driver(driver);
	if (ret) {
		tty_driver_kref_put(driver);
		tty_port_destroy(&eml_tty_port);
		pr_err("EML: tty_register_driver failed: %d\n", ret);
		return ret;
	}

	eml_tty_driver = driver;
	eml_console.device = eml_console_device;

	return 0;
}
device_initcall(eml_tty_init);
