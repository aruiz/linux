/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_EML_EFI_H
#define _ASM_EML_EFI_H

#include <linux/types.h>
#include <linux/efi.h>

#define MSABI __attribute__((ms_abi))

/*
 * EFI Simple Text Output Protocol
 */
struct eml_simple_text_output {
	void *reset;
	efi_status_t(MSABI *output_string)(struct eml_simple_text_output *this,
					   efi_char16_t *string);
	void *test_string;
};

/*
 * EFI Simple Text Input Protocol
 */
struct efi_input_key {
	u16 scan_code;
	u16 unicode_char;
};

struct eml_simple_text_input {
	efi_status_t(MSABI *reset)(struct eml_simple_text_input *this,
				   u8 extended_verification);
	efi_status_t(MSABI *read_key_stroke)(struct eml_simple_text_input *this,
					     struct efi_input_key *key);
	void *wait_for_key;
};

/*
 * EFI Boot Services (partial — only functions EML uses)
 */
typedef enum {
	EFI_ALLOCATE_ANY_PAGES,
	EFI_ALLOCATE_MAX_ADDRESS,
	EFI_ALLOCATE_ADDRESS,
} efi_allocate_type_t;

typedef void(MSABI *efi_event_notify_t)(void *event, void *context);

struct eml_boot_services {
	efi_table_hdr_t hdr;

	/* Task priority services */
	void *raise_tpl;
	void *restore_tpl;

	/* Memory services */
	efi_status_t(MSABI *allocate_pages)(efi_allocate_type_t type,
					    u32 memory_type,
					    unsigned long pages, u64 *memory);
	efi_status_t(MSABI *free_pages)(u64 memory, unsigned long pages);
	efi_status_t(MSABI *get_memory_map)(unsigned long *memory_map_size,
					    efi_memory_desc_t *memory_map,
					    unsigned long *map_key,
					    unsigned long *descriptor_size,
					    u32 *descriptor_version);
	efi_status_t(MSABI *allocate_pool)(u32 pool_type, unsigned long size,
					   void **buffer);
	efi_status_t(MSABI *free_pool)(void *buffer);

	/* Event & timer services */
	efi_status_t(MSABI *create_event)(u32 type, unsigned long notify_tpl,
					  efi_event_notify_t notify_function,
					  void *notify_context, void **event);
	efi_status_t(MSABI *set_timer)(void *event, u32 type, u64 trigger_time);
	efi_status_t(MSABI *wait_for_event)(unsigned long number_of_events,
					    void **event, unsigned long *index);
	efi_status_t(MSABI *signal_event)(void *event);
	efi_status_t(MSABI *close_event)(void *event);
	efi_status_t(MSABI *check_event)(void *event);

	/* Protocol handler services */
	void *install_protocol_interface;
	void *reinstall_protocol_interface;
	void *uninstall_protocol_interface;
	efi_status_t(MSABI *handle_protocol)(efi_handle_t handle,
					     efi_guid_t *protocol,
					     void **interface);
	void *reserved;
	void *register_protocol_notify;
	efi_status_t(MSABI *locate_handle)(u32 search_type,
					   efi_guid_t *protocol,
					   void *search_key,
					   unsigned long *buffer_size,
					   efi_handle_t *buffer);
	void *locate_device_path;
	void *install_configuration_table;

	/* Image services */
	void *load_image;
	void *start_image;
	void *exit;
	void *unload_image;
	void *exit_boot_services;

	/* Miscellaneous services */
	void *get_next_monotonic_count;
	efi_status_t(MSABI *stall)(unsigned long microseconds);
	efi_status_t(MSABI *set_watchdog_timer)(unsigned long timeout,
						u64 watchdog_code,
						unsigned long data_size,
						efi_char16_t *watchdog_data);

	/* Driver support services */
	void *connect_controller;
	void *disconnect_controller;

	/* Open/close protocol services */
	void *open_protocol;
	void *close_protocol;
	void *open_protocol_information;

	/* Library services */
	void *protocols_per_handle;
	efi_status_t(MSABI *locate_handle_buffer)(u32 search_type,
						  efi_guid_t *protocol,
						  void *search_key,
						  unsigned long *no_handles,
						  efi_handle_t **buffer);
	void *locate_protocol;
	void *install_multiple_protocol_interfaces;
	void *uninstall_multiple_protocol_interfaces;

	/* 32-bit CRC services */
	void *calculate_crc32;

	/* Miscellaneous services */
	void *copy_mem;
	void *set_mem;
	void *create_event_ex;
};

/*
 * EFI System Table — complete enough for EML
 */
struct eml_system_table {
	efi_table_hdr_t hdr;
	unsigned long fw_vendor;
	u32 fw_revision;
	unsigned long con_in_handle;
	struct eml_simple_text_input *con_in;
	unsigned long con_out_handle;
	struct eml_simple_text_output *con_out;
	unsigned long std_err_handle;
	struct eml_simple_text_output *std_err;
	void *runtime_services;
	struct eml_boot_services *boot_services;
	unsigned long nr_tables;
	void *tables;
};

/*
 * EFI timer types
 */
#define EFI_TIMER_CANCEL 0
#define EFI_TIMER_PERIODIC 1
#define EFI_TIMER_RELATIVE 2

/*
 * EFI event types
 */
#define EVT_TIMER 0x80000000
#define EVT_NOTIFY_SIGNAL 0x00000200
#define EVT_NOTIFY_WAIT 0x00000100

/*
 * EFI TPL levels
 */
#define TPL_APPLICATION 4
#define TPL_CALLBACK 8
#define TPL_NOTIFY 16

/*
 * EFI locate search types
 */
#define EFI_LOCATE_ALL_HANDLES 0
#define EFI_LOCATE_BY_REGISTER 1
#define EFI_LOCATE_BY_PROTOCOL 2

/*
 * EFI Block I/O Protocol
 */
#define EFI_BLOCK_IO_PROTOCOL_GUID                                         \
	EFI_GUID(0x964e5b21, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, \
		 0x69, 0x72, 0x3b)

struct efi_block_io_media {
	u32 media_id;
	u8 removable_media;
	u8 media_present;
	u8 logical_partition;
	u8 read_only;
	u8 write_caching;
	u8 pad[3];
	u32 block_size;
	u32 io_align;
	u64 last_block;
	u64 lowest_aligned_lba;
	u32 logical_blocks_per_physical_block;
	u32 optimal_transfer_length_granularity;
};

struct eml_block_io;

struct eml_block_io {
	u64 revision;
	struct efi_block_io_media *media;
	efi_status_t(MSABI *reset)(struct eml_block_io *this,
				   u8 extended_verification);
	efi_status_t(MSABI *read_blocks)(struct eml_block_io *this,
					 u32 media_id, u64 lba,
					 unsigned long buffer_size,
					 void *buffer);
	efi_status_t(MSABI *write_blocks)(struct eml_block_io *this,
					  u32 media_id, u64 lba,
					  unsigned long buffer_size,
					  void *buffer);
	efi_status_t(MSABI *flush_blocks)(struct eml_block_io *this);
};

/* Global EFI state */
extern struct eml_system_table *eml_systab;
extern efi_handle_t eml_image_handle;

/* Subsystem init functions */
extern void eml_setup_early_console(void);
extern void eml_setup_memory(void);
extern void eml_setup_timer(void);

#endif /* _ASM_EML_EFI_H */
