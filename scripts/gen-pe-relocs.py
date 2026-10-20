#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Generate PE/COFF .reloc section and patch a flat PE binary.

Usage: gen-pe-relocs.py vmlinux linux.efi

1. Reads R_X86_64_64 relocations from vmlinux (linked with --emit-relocs)
2. Generates PE base relocation table
3. Appends it to linux.efi
4. Patches the PE header to reference the .reloc section
"""

import struct
import sys
from collections import defaultdict

IMAGE_REL_BASED_DIR64 = 10
IMAGE_REL_BASED_HIGHLOW = 3
IMAGE_REL_BASED_ABSOLUTE = 0
R_X86_64_64 = 1
R_X86_64_32 = 10
R_X86_64_32S = 11

# PE header offsets (computed from our head.S layout)
# DOS header: 64 bytes (MZ + padding + PE offset at 0x3c)
# PE signature: 4 bytes at offset 64
# COFF header: 20 bytes at offset 68
# Optional header starts at offset 88


def read_elf_relocs(elf_path):
    """Read R_X86_64_64/32/32S relocations from ELF, return sorted offset→size map."""
    import struct as s

    relocs = {}

    with open(elf_path, 'rb') as f:
        # Read ELF header
        ei_mag = f.read(4)
        assert ei_mag == b'\x7fELF', f"Not an ELF file: {elf_path}"
        f.seek(0)
        ehdr = f.read(64)

        ei_class = ehdr[4]
        assert ei_class == 2, "Only 64-bit ELF supported"

        e_shoff = s.unpack_from('<Q', ehdr, 40)[0]
        e_shentsize = s.unpack_from('<H', ehdr, 58)[0]
        e_shnum = s.unpack_from('<H', ehdr, 60)[0]
        e_shstrndx = s.unpack_from('<H', ehdr, 62)[0]

        # Read section headers
        f.seek(e_shoff)
        shdrs = []
        for i in range(e_shnum):
            shdrs.append(f.read(e_shentsize))

        # Read section name string table
        shstrtab_hdr = shdrs[e_shstrndx]
        str_off = s.unpack_from('<Q', shstrtab_hdr, 24)[0]
        str_size = s.unpack_from('<Q', shstrtab_hdr, 32)[0]
        f.seek(str_off)
        shstrtab = f.read(str_size)

        def get_name(sh_name):
            end = shstrtab.index(b'\x00', sh_name)
            return shstrtab[sh_name:end].decode('ascii')

        # Process RELA sections
        SHT_RELA = 4
        for shdr in shdrs:
            sh_type = s.unpack_from('<I', shdr, 4)[0]
            if sh_type != SHT_RELA:
                continue

            sh_name = s.unpack_from('<I', shdr, 0)[0]
            name = get_name(sh_name)

            # Get the target section
            sh_info = s.unpack_from('<I', shdr, 28)[0]
            if sh_info >= len(shdrs):
                continue
            target_hdr = shdrs[sh_info]
            target_name_idx = s.unpack_from('<I', target_hdr, 0)[0]
            target_name = get_name(target_name_idx)

            # Skip debug/discard sections
            if any(target_name.startswith(p) for p in
                   ('.debug', '.discard', '.comment', '.eh_frame',
                    '.got', '.got.plt', '.modinfo')):
                continue

            sh_offset = s.unpack_from('<Q', shdr, 24)[0]
            sh_size = s.unpack_from('<Q', shdr, 32)[0]
            sh_entsize = s.unpack_from('<Q', shdr, 56)[0]

            if sh_entsize == 0:
                continue

            f.seek(sh_offset)
            rela_data = f.read(sh_size)

            for i in range(0, len(rela_data), sh_entsize):
                r_offset = s.unpack_from('<Q', rela_data, i)[0]
                r_info = s.unpack_from('<Q', rela_data, i + 8)[0]
                r_type = r_info & 0xffffffff

                if r_type == R_X86_64_64:
                    relocs[r_offset] = 8
                elif r_type in (R_X86_64_32, R_X86_64_32S):
                    relocs[r_offset] = 4

    return relocs


def build_reloc_section(relocs):
    """Build a PE .reloc section from an offset→size map."""
    pages = defaultdict(list)
    for off in sorted(relocs.keys()):
        page = off & ~0xfff
        page_off = off & 0xfff
        size = relocs[off]
        if size == 8:
            pe_type = IMAGE_REL_BASED_DIR64
        else:
            pe_type = IMAGE_REL_BASED_HIGHLOW
        pages[page].append((pe_type << 12) | page_off)

    data = bytearray()
    for page_rva in sorted(pages.keys()):
        entries = pages[page_rva]

        # Pad to 4-byte alignment
        if len(entries) % 2:
            entries.append(0)  # IMAGE_REL_BASED_ABSOLUTE padding

        block_size = 8 + len(entries) * 2
        data += struct.pack('<II', page_rva, block_size)
        for e in entries:
            data += struct.pack('<H', e)

    return bytes(data)


def patch_pe(pe_path, reloc_data, image_end_rva):
    """Append .reloc section and patch PE headers."""
    with open(pe_path, 'r+b') as f:
        # Read DOS header to find PE offset
        f.seek(0x3c)
        pe_off = struct.unpack('<I', f.read(4))[0]

        # Read PE signature and COFF header
        f.seek(pe_off)
        pe_sig = f.read(4)
        assert pe_sig == b'PE\x00\x00'

        coff = f.read(20)
        num_sections = struct.unpack_from('<H', coff, 2)[0]
        opt_hdr_size = struct.unpack_from('<H', coff, 16)[0]

        opt_hdr_off = pe_off + 4 + 20
        sect_table_off = opt_hdr_off + opt_hdr_size

        # Align reloc RVA to page boundary
        reloc_rva = (image_end_rva + 0xfff) & ~0xfff
        reloc_size = len(reloc_data)

        # Pad reloc_data to 512-byte file alignment
        pad = (512 - (reloc_size % 512)) % 512
        reloc_data_padded = reloc_data + b'\x00' * pad

        # Update SizeOfImage
        new_image_size = reloc_rva + ((reloc_size + 0xfff) & ~0xfff)
        # SizeOfImage is at opt_hdr_off + 56
        f.seek(opt_hdr_off + 56)
        f.write(struct.pack('<I', new_image_size))

        # Update BaseRelocationTable data directory
        # Optional header: magic(2) + linkerVer(2) + sizeOfCode(4) + sizeOfInit(4) +
        # sizeOfUninit(4) + entryPoint(4) + baseOfCode(4) = 20 bytes
        # Then ImageBase(8) + sectAlign(4) + fileAlign(4) + osVer(4) + imgVer(4) +
        # subsysVer(4) + win32ver(4) + sizeOfImage(4) + sizeOfHeaders(4) +
        # checkSum(4) + subsystem(2) + dllChars(2) + stack/heap(32) +
        # loaderFlags(4) + numRvaAndSizes(4) = 92 bytes
        # Total before data dirs: 20 + 92 = 112 bytes from opt_hdr_off
        # Data dirs start at opt_hdr_off + 112
        # BaseRelocationTable is data dir index 5 (each dir is 8 bytes)
        reloc_dir_off = opt_hdr_off + 112 + 5 * 8
        f.seek(reloc_dir_off)
        f.write(struct.pack('<II', reloc_rva, reloc_size))

        # Find and update .reloc section header (section 3, 0-indexed = 2)
        reloc_sect_off = sect_table_off + 2 * 40
        f.seek(reloc_sect_off)
        sect_name = f.read(8)
        assert sect_name == b'.reloc\x00\x00', f"Expected .reloc section, got {sect_name}"

        # Get current file size for PointerToRawData
        f.seek(0, 2)
        file_end = f.tell()

        # Patch .reloc section header
        f.seek(reloc_sect_off + 8)  # after name
        f.write(struct.pack('<I', reloc_size))     # VirtualSize
        f.write(struct.pack('<I', reloc_rva))      # VirtualAddress
        f.write(struct.pack('<I', len(reloc_data_padded)))  # SizeOfRawData
        f.write(struct.pack('<I', file_end))       # PointerToRawData

        # Append reloc data
        f.seek(0, 2)
        f.write(reloc_data_padded)


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} vmlinux linux.efi", file=sys.stderr)
        sys.exit(1)

    elf_path = sys.argv[1]
    pe_path = sys.argv[2]

    relocs = read_elf_relocs(elf_path)
    if not relocs:
        print("gen-pe-relocs: no relocations found", file=sys.stderr)
        return

    reloc_data = build_reloc_section(relocs)

    # Get _end symbol value for image_end_rva
    # Read from ELF symbol table
    with open(elf_path, 'rb') as f:
        ehdr = f.read(64)
        e_shoff = struct.unpack_from('<Q', ehdr, 40)[0]
        e_shentsize = struct.unpack_from('<H', ehdr, 58)[0]
        e_shnum = struct.unpack_from('<H', ehdr, 60)[0]
        e_shstrndx = struct.unpack_from('<H', ehdr, 62)[0]

        # Read section headers
        f.seek(e_shoff)
        shdrs = []
        for i in range(e_shnum):
            shdrs.append(f.read(e_shentsize))

        # Read section name string table
        shstrtab_hdr = shdrs[e_shstrndx]
        str_off = struct.unpack_from('<Q', shstrtab_hdr, 24)[0]
        str_size = struct.unpack_from('<Q', shstrtab_hdr, 32)[0]
        f.seek(str_off)
        shstrtab = f.read(str_size)

        # Find symtab
        SHT_SYMTAB = 2
        image_end = 0
        for shdr in shdrs:
            sh_type = struct.unpack_from('<I', shdr, 4)[0]
            if sh_type != SHT_SYMTAB:
                continue

            sh_offset = struct.unpack_from('<Q', shdr, 24)[0]
            sh_size = struct.unpack_from('<Q', shdr, 32)[0]
            sh_entsize = struct.unpack_from('<Q', shdr, 56)[0]
            sh_link = struct.unpack_from('<I', shdr, 24 + 16)[0]

            # Read symbol string table
            strtab_hdr = shdrs[sh_link]
            strtab_off = struct.unpack_from('<Q', strtab_hdr, 24)[0]
            strtab_size = struct.unpack_from('<Q', strtab_hdr, 32)[0]
            f.seek(strtab_off)
            strtab = f.read(strtab_size)

            f.seek(sh_offset)
            symdata = f.read(sh_size)

            for i in range(0, len(symdata), sh_entsize):
                st_name = struct.unpack_from('<I', symdata, i)[0]
                st_value = struct.unpack_from('<Q', symdata, i + 8)[0]
                end = strtab.index(b'\x00', st_name)
                name = strtab[st_name:end].decode('ascii')
                if name == '_end':
                    image_end = st_value
                    break
            if image_end:
                break

    if image_end == 0:
        print("gen-pe-relocs: _end symbol not found", file=sys.stderr)
        sys.exit(1)

    patch_pe(pe_path, reloc_data, image_end)

    n64 = sum(1 for s in relocs.values() if s == 8)
    n32 = sum(1 for s in relocs.values() if s == 4)
    print(f"gen-pe-relocs: {len(relocs)} relocations ({n64} x64, {n32} x32), "
          f"reloc section at RVA 0x{(image_end + 0xfff) & ~0xfff:x}, "
          f"{len(reloc_data)} bytes",
          file=sys.stderr)


if __name__ == '__main__':
    main()
