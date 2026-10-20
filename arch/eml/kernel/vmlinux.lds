OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(eml_pe_header)
jiffies = jiffies_64;
SECTIONS
{
 . = 0;
 .head : ALIGN(4096) {
  *(.head)
 }
 _text = .;
 .text : ALIGN(4096) {
  _stext = .;
  KEEP(*(.head.text))
  . = ALIGN(0); *(.text.asan.* .text.tsan.*) *(.text.unknown .text.unknown.*) __split_text_start = .; *(.text.split .text.split.[0-9a-zA-Z_]*) __split_text_end = .; __unlikely_text_start = .; *(.text.unlikely .text.unlikely.*) __unlikely_text_end = .; . = ALIGN((1 << 12)); __hot_text_start = .; *(.text.hot .text.hot.*) __hot_text_end = .; *(.text .text.[_0-9A-Za-df-rt-z]* .text.s[_0-9A-Za-su-z]* .text.s .text.s.* .text.st[_0-9A-Zb-z]* .text.st .text.st.* .text.sta[_0-9A-Za-qs-z]* .text.sta .text.sta.* .text.star[_0-9A-Za-su-z]* .text.star .text.star.* .text.start[_0-9A-Za-tv-z]* .text.start .text.start.* .text.startu[_0-9A-Za-oq-z]* .text.startu .text.startu.* .text.startup[_0-9A-Za-z]* .text.e[_0-9A-Za-wy-z]* .text.e .text.e.* .text.ex[_0-9A-Za-hj-z]* .text.ex .text.ex.* .text.exi[_0-9A-Za-su-z]* .text.exi .text.exi.* .text.exit[_0-9A-Za-z]* .text.fixup) . = ALIGN(0); __noinstr_text_start = .; *(.noinstr.text) __cpuidle_text_start = .; *(.cpuidle.text) __cpuidle_text_end = .; __noinstr_text_end = .; *(.ref.text)
  . = ALIGN(0); __sched_text_start = .; *(.sched.text) __sched_text_end = .;
  . = ALIGN(0); __lock_text_start = .; *(.spinlock.text) __lock_text_end = .;
  . = ALIGN(0); __irqentry_text_start = .; *(.irqentry.text) __irqentry_text_end = .;
  . = ALIGN(0); __softirqentry_text_start = .; *(.softirqentry.text) __softirqentry_text_end = .;
  . = ALIGN(0); __noinstr_text_start = .; *(.noinstr.text) __cpuidle_text_start = .; *(.cpuidle.text) __cpuidle_text_end = .; __noinstr_text_end = .;
  *(.fixup)
  *(.gnu.warning)
 }
 . = ALIGN((1 << 12));
 __init_begin = .;
 _sinittext = .;
 .init.text : {
  *(.init.text .init.text.*) *(.text.startup .text.startup.*)
 }
 _einittext = .;
 .init.data : {
  KEEP(*(SORT(___kentry+*))) *(.init.data .init.data.*) *(.init.rodata .init.rodata.*) . = ALIGN(32); __dtb_start = .; KEEP(*(.dtb.init.rodata)) __dtb_end = .; . = ALIGN(8); __kunit_init_suites_start = .; KEEP(*(.kunit_init_test_suites)) __kunit_init_suites_end = .;
  . = ALIGN(16); __setup_start = .; KEEP(*(.init.setup)) __setup_end = .;
  __initcall_start = .; KEEP(*(.initcallearly.init)) __initcall0_start = .; KEEP(*(.initcall0.init)) KEEP(*(.initcall0s.init)) __initcall1_start = .; KEEP(*(.initcall1.init)) KEEP(*(.initcall1s.init)) __initcall2_start = .; KEEP(*(.initcall2.init)) KEEP(*(.initcall2s.init)) __initcall3_start = .; KEEP(*(.initcall3.init)) KEEP(*(.initcall3s.init)) __initcall4_start = .; KEEP(*(.initcall4.init)) KEEP(*(.initcall4s.init)) __initcall5_start = .; KEEP(*(.initcall5.init)) KEEP(*(.initcall5s.init)) __initcallrootfs_start = .; KEEP(*(.initcallrootfs.init)) KEEP(*(.initcallrootfss.init)) __initcall6_start = .; KEEP(*(.initcall6.init)) KEEP(*(.initcall6s.init)) __initcall7_start = .; KEEP(*(.initcall7.init)) KEEP(*(.initcall7s.init)) __initcall_end = .;
  __con_initcall_start = .; KEEP(*(.con_initcall.init)) __con_initcall_end = .;
 
 }
 . = ALIGN((1 << 12));
 __init_end = .;
 . = ALIGN(((1 << 12))); .rodata : AT(ADDR(.rodata) - 0) { __start_rodata = .; *(.rodata) *(.rodata.*) *(.data.rel.ro*) . = ALIGN(32); __sched_class_highest = .; *(__stop_sched_class) *(__dl_sched_class) *(__rt_sched_class) *(__fair_sched_class) *(__ext_sched_class) *(__idle_sched_class) __sched_class_lowest = .; . = ALIGN(8); __start_ro_after_init = .; *(.data..ro_after_init) . = ALIGN(8); __start___jump_table = .; KEEP(*(__jump_table)) __stop___jump_table = .; __end_ro_after_init = .; . = ALIGN(8); __start___tracepoints_ptrs = .; KEEP(*(__tracepoints_ptrs)) __stop___tracepoints_ptrs = .; *(__tracepoints_strings) } .rodata1 : AT(ADDR(.rodata1) - 0) { *(.rodata1) } .pci_fixup : AT(ADDR(.pci_fixup) - 0) { __start_pci_fixups_early = .; KEEP(*(.pci_fixup_early)) __end_pci_fixups_early = .; __start_pci_fixups_header = .; KEEP(*(.pci_fixup_header)) __end_pci_fixups_header = .; __start_pci_fixups_final = .; KEEP(*(.pci_fixup_final)) __end_pci_fixups_final = .; __start_pci_fixups_enable = .; KEEP(*(.pci_fixup_enable)) __end_pci_fixups_enable = .; __start_pci_fixups_resume = .; KEEP(*(.pci_fixup_resume)) __end_pci_fixups_resume = .; __start_pci_fixups_suspend = .; KEEP(*(.pci_fixup_suspend)) __end_pci_fixups_suspend = .; __start_pci_fixups_resume_early = .; KEEP(*(.pci_fixup_resume_early)) __end_pci_fixups_resume_early = .; __start_pci_fixups_suspend_late = .; KEEP(*(.pci_fixup_suspend_late)) __end_pci_fixups_suspend_late = .; } .builtin_fw : AT(ADDR(.builtin_fw) - 0) ALIGN(8) { __start_builtin_fw = .; KEEP(*(.builtin_fw)) __end_builtin_fw = .; } __ksymtab : AT(ADDR(__ksymtab) - 0) { __start___ksymtab = .; KEEP(*(SORT(___ksymtab+*))) __stop___ksymtab = .; } __kcrctab : AT(ADDR(__kcrctab) - 0) { __start___kcrctab = .; KEEP(*(SORT(___kcrctab+*))) __stop___kcrctab = .; } __kflagstab : AT(ADDR(__kflagstab) - 0) { __start___kflagstab = .; KEEP(*(SORT(___kflagstab+*))) __stop___kflagstab = .; } __ksymtab_strings : AT(ADDR(__ksymtab_strings) - 0) { *(__ksymtab_strings) } __init_rodata : AT(ADDR(__init_rodata) - 0) { *(.ref.rodata) } __param : AT(ADDR(__param) - 0) { __start___param = .; KEEP(*(__param)) __stop___param = .; } __modver : AT(ADDR(__modver) - 0) { __start___modver = .; KEEP(*(__modver)) __stop___modver = .; } /DISCARD/ : { *(.note.GNU-stack) *(.note.gnu.property) } .notes : AT(ADDR(.notes) - 0) { __start_notes = .; KEEP(*(.note.*)) __stop_notes = .; } . = ALIGN(((1 << 12))); __end_rodata = .;
 . = ALIGN(16); __ex_table : AT(ADDR(__ex_table) - 0) { __start___ex_table = .; KEEP(*(__ex_table)) __stop___ex_table = .; }
 /DISCARD/ : { *(.note.GNU-stack) *(.note.gnu.property) } .notes : AT(ADDR(.notes) - 0) { __start_notes = .; KEEP(*(.note.*)) __stop_notes = .; }
 _etext = ALIGN(4096);
 . = _etext;
 . = ALIGN(4096);
 _sdata = .;
 _data = .;
 . = ALIGN((1 << 12)); .data : AT(ADDR(.data) - 0) { . = ALIGN((4 * (1 << 12))); __start_init_stack = .; init_thread_union = .; init_stack = .; KEEP(*(.data..init_thread_info)) . = __start_init_stack + (4 * (1 << 12)); __end_init_stack = .; . = ALIGN((1 << 12)); __nosave_begin = .; *(.data..nosave) . = ALIGN((1 << 12)); __nosave_end = .; . = ALIGN((1 << 12)); *(.data..page_aligned) . = ALIGN((1 << 12)); . = ALIGN(64); *(SORT_BY_ALIGNMENT(.data..hot.*)) . = ALIGN(64); . = ALIGN(64); *(.data..cacheline_aligned) . = ALIGN(64); *(.data..read_mostly) . = ALIGN(64); *(.xiptext) *(.data .data.[0-9a-zA-Z_]* .data.rel.* .data..L* .data..compoundliteral* .data.$__unnamed_* .data.$L*) *(.data..decrypted) *(.ref.data) *(.data..shared_aligned) *(.data..unlikely) __start_once = .; *(.data..once) __end_once = .; *(.data..do_once) . = ALIGN(32); *(__tracepoints) . = ALIGN(8); __start___dyndbg_classes = .; KEEP(*(__dyndbg_classes)) __stop___dyndbg_classes = .; __start___dyndbg = .; KEEP(*(__dyndbg)) __stop___dyndbg = .; . = ALIGN(8); __kunit_suites_start = .; KEEP(*(.kunit_test_suites)) __kunit_suites_end = .; CONSTRUCTORS }
 _edata = .;
 . = ALIGN((1 << 12)); __bss_start = .; . = ALIGN((1 << 12)); .sbss : AT(ADDR(.sbss) - 0) { *(.dynsbss) *(.sbss .sbss.[0-9a-zA-Z_]*) *(.scommon) } . = ALIGN((1 << 12)); .bss : AT(ADDR(.bss) - 0) { . = ALIGN((1 << 12)); *(.bss..page_aligned) . = ALIGN((1 << 12)); *(.dynbss) *(.bss .bss.[0-9a-zA-Z_]* .bss..L* .bss..compoundliteral*) *(COMMON) } . = ALIGN((1 << 12)); __bss_stop = .;
 _end = .;
 .stab 0 : { *(.stab) } .stabstr 0 : { *(.stabstr) } .stab.excl 0 : { *(.stab.excl) } .stab.exclstr 0 : { *(.stab.exclstr) } .stab.index 0 : { *(.stab.index) } .stab.indexstr 0 : { *(.stab.indexstr) }
 .debug 0 : { *(.debug) } .line 0 : { *(.line) } .debug_srcinfo 0 : { *(.debug_srcinfo) } .debug_sfnames 0 : { *(.debug_sfnames) } .debug_aranges 0 : { *(.debug_aranges) } .debug_pubnames 0 : { *(.debug_pubnames) } .debug_info 0 : { *(.debug_info .gnu.linkonce.wi.*) } .debug_abbrev 0 : { *(.debug_abbrev) } .debug_line 0 : { *(.debug_line) } .debug_frame 0 : { *(.debug_frame) } .debug_str 0 : { *(.debug_str) } .debug_loc 0 : { *(.debug_loc) } .debug_macinfo 0 : { *(.debug_macinfo) } .debug_pubtypes 0 : { *(.debug_pubtypes) } .debug_ranges 0 : { *(.debug_ranges) } .debug_weaknames 0 : { *(.debug_weaknames) } .debug_funcnames 0 : { *(.debug_funcnames) } .debug_typenames 0 : { *(.debug_typenames) } .debug_varnames 0 : { *(.debug_varnames) } .debug_gnu_pubnames 0 : { *(.debug_gnu_pubnames) } .debug_gnu_pubtypes 0 : { *(.debug_gnu_pubtypes) } .debug_types 0 : { *(.debug_types) } .debug_addr 0 : { *(.debug_addr) } .debug_line_str 0 : { *(.debug_line_str) } .debug_loclists 0 : { *(.debug_loclists) } .debug_macro 0 : { *(.debug_macro) } .debug_names 0 : { *(.debug_names) } .debug_rnglists 0 : { *(.debug_rnglists) } .debug_str_offsets 0 : { *(.debug_str_offsets) }
 .comment 0 : { *(.comment) } .symtab 0 : { *(.symtab) } .strtab 0 : { *(.strtab) } .shstrtab 0 : { *(.shstrtab) }
 /DISCARD/ : {
  *(.discard .discard.*)
  *(.modinfo .init.modinfo)
  *(.comment)
  *(.eh_frame*)
  *(.got .got.plt)
 }
}
PROVIDE(__data_rawsize = ABSOLUTE(_edata - _data));
PROVIDE(__data_size = ABSOLUTE(_end - _data));
