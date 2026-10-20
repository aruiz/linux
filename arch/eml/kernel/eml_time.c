// SPDX-License-Identifier: GPL-2.0
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/timex.h>
#include <asm/eml_efi.h>

extern void update_process_times(int user);

void __delay(unsigned long loops)
{
	eml_systab->boot_services->stall(loops);
}

void __const_udelay(unsigned long xloops)
{
	unsigned long usecs = xloops * 10;

	if (usecs > 0)
		eml_systab->boot_services->stall(usecs);
}

void __udelay(unsigned long usecs)
{
	eml_systab->boot_services->stall(usecs);
}

void __ndelay(unsigned long nsecs)
{
	unsigned long usecs = (nsecs + 999) / 1000;

	if (usecs > 0)
		eml_systab->boot_services->stall(usecs);
}

static void *eml_timer_event;
static unsigned long eml_pending_ticks;

static void MSABI eml_timer_callback(void *event, void *context)
{
	jiffies_64++;
	eml_pending_ticks++;
}

void eml_process_ticks(void)
{
	unsigned long ticks;

	ticks = eml_pending_ticks;
	if (!ticks)
		return;
	eml_pending_ticks = 0;

	while (ticks--)
		update_process_times(0);
}

void __init eml_setup_timer(void)
{
	efi_status_t status;
	u64 interval;

	/* Disable the UEFI watchdog timer (default 5-minute reset) */
	eml_systab->boot_services->set_watchdog_timer(0, 0, 0, NULL);

	interval = 10000000ULL / HZ;

	status = eml_systab->boot_services->create_event(
		EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, eml_timer_callback,
		NULL, &eml_timer_event);
	if (status != EFI_SUCCESS) {
		pr_err("EML: CreateEvent failed: 0x%lx\n",
		       (unsigned long)status);
		return;
	}

	status = eml_systab->boot_services->set_timer(
		eml_timer_event, EFI_TIMER_PERIODIC, interval);
	if (status != EFI_SUCCESS)
		pr_err("EML: SetTimer failed: 0x%lx\n", (unsigned long)status);
	else
		pr_info("EML: timer event created, %d Hz (watchdog disabled)\n",
			HZ);
}
