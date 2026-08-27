# extract_coredump.py - coredump 分区镜像 → ELF
# 格式: [0:4]=total_len [4:8]=version [8:24]=reserved [24:]=ELF
import struct, sys

raw = open('coredump_raw.bin', 'rb').read()
total_len = struct.unpack('<I', raw[0:4])[0]
version = struct.unpack('<I', raw[4:8])[0]
print('total_len=%d (0x%x)  version=0x%x  raw_size=%d' % (total_len, total_len, version, len(raw)))

if total_len == 0 or total_len > len(raw):
    print('ERROR: invalid total_len (partition may be empty)')
    sys.exit(1)

# 查找 ELF magic
idx = raw.find(b'\x7fELF', 0, 64)
if idx < 0:
    print('ERROR: ELF magic not found in first 64 bytes')
    sys.exit(1)
print('ELF starts at offset %d' % idx)

elf = raw[idx:total_len]
open('coredump.elf', 'wb').write(elf)
print('Wrote coredump.elf (%d bytes)' % len(elf))
