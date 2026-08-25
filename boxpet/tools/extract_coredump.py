# extract_coredump.py — 从 flash coredump 分区镜像中提取 ELF
# 结构（实测 v0x90102）：[0:4]=总长 [4:8]=版本 [8:24]=保留 [24:]=ELF
import struct, sys

src = sys.argv[1] if len(sys.argv) > 1 else "coredump_flash.bin"
dst = sys.argv[2] if len(sys.argv) > 2 else "coredump.elf"

data = open(src, "rb").read()
total, ver = struct.unpack("<II", data[0:8])
print(f"total={total} ver=0x{ver:X}")

# ELF 起始
off = data.find(b"\x7fELF")
if off < 0:
    print("no ELF magic found")
    sys.exit(1)
print(f"ELF at offset {off}")

# 从 ELF program header 计算实际结束位置
e_phoff, = struct.unpack("<I", data[off+28:off+32])
e_phentsize, e_phnum = struct.unpack("<HH", data[off+42:off+46])
end = e_phoff + e_phentsize * e_phnum
for i in range(e_phnum):
    ph = off + e_phoff + i * e_phentsize
    p_offset, p_filesz = struct.unpack("<II", data[ph+4:ph+8] + data[ph+16:ph+20])
    end = max(end, p_offset + p_filesz)
print(f"ELF computed size={end}")

elf = data[off:off+end]
open(dst, "wb").write(elf)
print(f"saved {len(elf)} bytes -> {dst}")
