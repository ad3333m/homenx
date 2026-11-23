#!/usr/bin/env python3
import struct
import sys

TARGETS = {
    b"ff_tx_tab_8192_float",
    b"ff_tx_tab_16384_float",
    b"ff_tx_tab_32768_float",
    b"ff_tx_tab_65536_float",
    b"ff_tx_tab_131072_float",
    b"ff_tx_tab_262144_float",
    b"ff_tx_tab_524288_float",
    b"ff_tx_tab_1048576_float",
    b"ff_tx_tab_2097152_float",
}
SECTION_TARGETS = {b".bss." + name for name in TARGETS}

def slim_elf64(in_path: str, out_path: str) -> None:
    data = bytearray(open(in_path, "rb").read())
    e_shoff, = struct.unpack_from("<Q", data, 40)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 58)

    def get_shdr(idx: int):
        off = e_shoff + idx * e_shentsize
        return off, list(struct.unpack_from("<IIQQQQIIQQ", data, off))

    _, shstr_hdr = get_shdr(e_shstrndx)
    shstr_off, shstr_size = shstr_hdr[4], shstr_hdr[5]
    shstrtab = bytes(data[shstr_off : shstr_off + shstr_size])

    def get_str(blob: bytes, idx: int) -> bytes:
        end = blob.find(b"\x00", idx)
        return blob[idx:end] if end != -1 else b""

    for i in range(e_shnum):
        off, shdr = get_shdr(i)
        sec_name = get_str(shstrtab, shdr[0])
        if sec_name in SECTION_TARGETS:
            shdr[5] = 16  # sh_size = 16 bytes
            struct.pack_into("<IIQQQQIIQQ", data, off, *shdr)
        elif shdr[1] == 2:  # SHT_SYMTAB
            sym_off, sym_size, str_idx, entsize = shdr[4], shdr[5], shdr[6], shdr[9]
            _, str_hdr = get_shdr(str_idx)
            strtab = bytes(data[str_hdr[4] : str_hdr[4] + str_hdr[5]])
            for s in range(0, sym_size, entsize):
                soff = sym_off + s
                st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
                    "<IBBHQQ", data, soff
                )
                if get_str(strtab, st_name) in TARGETS:
                    st_size = 16
                    struct.pack_into("<IBBHQQ", data, soff, st_name, st_info, st_other, st_shndx, st_value, st_size)

    open(out_path, "wb").write(data)

if __name__ == "__main__":
    slim_elf64(sys.argv[1], sys.argv[2])
