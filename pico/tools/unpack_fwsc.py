# Unpack a PICO .fwsc (the app decrypted): python3 tools/unpack_fwsc.py X.fwsc -> flash.bin, ota.bin, app.bin in the current folder. Used to pull the first PICO t127 text (docs/t127-strings.txt).
import sys, struct
sys.path.insert(0, '/home/claude/syrup/pico/tools')
from fm1pkg_make import enc, sfc, KEY
raw = open(sys.argv[1], 'rb').read()
log = bytearray()
for i in range(20): log += raw[i*0x30:i*0x30+0x2F]
log += raw[20*0x30:]
hdr = bytearray(log[0:0x40]); enc(hdr, 0, 0x40)
n = struct.unpack_from('<H', hdr, 8)[0]
for i in range(n):
    e = bytearray(log[0x40+i*0x50:0x40+(i+1)*0x50]); enc(e, 0, 0x50)
    typ, idx, c, _, off, ln, _ = struct.unpack_from('<HHHHIII', e, 0)
    name = e[0x40:].split(b'\0')[0].decode()
    open(name, 'wb').write(log[off:off+ln]); print(name, off, ln)
f = open('flash.bin','rb').read()
reg = bytearray(f[0x4000:0x5C000+0x37000])
sfc(reg, 0, len(reg), 0, KEY)
ent = reg[32:64]
print(reg[32+14:32+30])
app = reg[0x120:0x120+0x56FBC]
open('app.bin','wb').write(app); print('app', len(app), app[:16].hex())
