#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
#
# Test that mkimage --rk-key produces a structurally valid RK3399
# secure-boot idblock (see doc/board/rockchip/rk3399-secure-boot.rst).
#
# Usage: tools/rk-sign-test.sh
# Requirements: openssl, python3, and a host toolchain for `make tools-only'.

set -u

UB=${UB:-$(cd "$(dirname "$0")/.." && pwd)}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rk-sign-test.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

# --- build tools (needed after source changes) ---
if [ ! -x "$UB/tools/mkimage" ] || \
   [ "$UB/tools/rkcommon.c" -nt "$UB/tools/mkimage" ]; then
	echo "building tools..."
	(cd "$UB" && make tools-only_defconfig >/dev/null 2>&1 &&
	 make tools-only -j"$(nproc)") > build.log 2>&1 || {
		echo "TOOLS BUILD FAILED"
		tail -20 build.log
		exit 2
	}
fi
MKIMAGE="$UB/tools/mkimage"

# --- test inputs: throwaway key and dummy stages ---
openssl genrsa -out private_key.pem 2048 2>/dev/null
openssl rsa -in private_key.pem -pubout -out public_key.pem 2>/dev/null
# rk3399's SRAM limit is 0x2e000; use a realistic TPL size
dd if=/dev/urandom of=tpl.bin bs=1024 count=51 2>/dev/null
dd if=/dev/urandom of=spl.bin bs=1024 count=16 2>/dev/null

# --- THE BEHAVIOR UNDER TEST ---
"$MKIMAGE" -T rksd -n rk3399 --rk-key private_key.pem \
	-d tpl.bin:spl.bin signed.img 2> mkimage.err
rc=$?
if [ $rc -ne 0 ]; then
	echo "RED: mkimage failed (rc=$rc):"
	cat mkimage.err
	exit 1
fi
# the efuse provisioning hash must be printed
grep -q "Rockchip key blob SHA256" mkimage.err || {
	echo "RED: missing efuse hash output"; exit 1; }

# --- structural assertions (BootROM-verified format) ---
python3 <<'EOF'
import hashlib, struct, subprocess, sys
KEY = bytes([124,78,3,4,85,5,9,7,45,44,123,56,23,13,23,17])
def rc4(data):
    s = list(range(256)); j = 0
    for i in range(256):
        j = (j + s[i] + KEY[i % 16]) & 0xff
        s[i], s[j] = s[j], s[i]
    i = j = 0; out = bytearray()
    for b in data:
        i = (i + 1) & 0xff
        j = (j + s[i]) & 0xff
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xff])
    return bytes(out)
def blk(data, off):
    return rc4(data[off:off+0x200])
def blocks(data, start, size):
    return b''.join(blk(data, o) for o in range(start, start+size, 0x200))

fails = []
def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond: fails.append(name)

img = open('signed.img','rb').read()
tpl = open('tpl.bin','rb').read()
spl = open('spl.bin','rb').read()

# header0
h0 = blk(img, 0)
check('header0 magic', struct.unpack('<I', h0[:4])[0] == 0x0ff0aa55)
check('header0 init_offset=4', struct.unpack('<H', h0[12:14])[0] == 4)
check('header0 disable_rc4=0 (signed payload is RC4-encoded)',
      struct.unpack('<I', h0[8:12])[0] == 0)
# rkcommon aligns stage sizes to 2KB
tpl_size = (len(tpl) + 0x7ff) & ~0x7ff
spl_size = (len(spl) + 0x7ff) & ~0x7ff
init_size = struct.unpack('<H', h0[0x1fa:0x1fc])[0]
check('header0 init_size covers RSAK+TPL+SPL',
      init_size == (0x800 + tpl_size + spl_size) // 0x200)
check('header0 init_boot_size=0', struct.unpack('<H', h0[0x1fc:0x1fe])[0] == 0)

# RSAK block at payload start (0x800)
rk = blocks(img, 0x800, 0x800)
check('RSAK magic', rk[:4] == b'RSAK')
check('flags 0x323', struct.unpack('<I', rk[8:12])[0] == 0x323)
check('data length 0xda', struct.unpack('<I', rk[12:16])[0] == 0xda)

# key blob: modulus (LE) + exponent 65537 LE + padding
n_hex = subprocess.run('openssl rsa -in public_key.pem -pubin -modulus -noout | sed s/Modulus=//',
                       shell=True, capture_output=True).stdout.decode().strip()
modulus = bytes.fromhex(n_hex)
check('key blob modulus matches key (LE)', rk[0x20:0x120] == modulus[::-1])
check('key blob exponent 010001 LE', rk[0x120:0x124] == b'\x01\x00\x01\x00')
check('key blob padding zero', rk[0x124:0x220] == bytes(0xfc))

# payload hashes
tpl_off = 0x1000
spl_off = tpl_off + tpl_size
tpl_img = blocks(img, tpl_off, tpl_size)[:len(tpl)]
spl_img = blocks(img, spl_off, spl_size)[:len(spl)]
check('TPL content preserved', tpl_img == tpl)
check('SPL content preserved', spl_img == spl)
check('hash1 == SHA256(TPL)', rk[0x320:0x340] == hashlib.sha256(tpl).digest())
check('hash2 == SHA256(SPL)', rk[0x344:0x364] == hashlib.sha256(spl).digest())
check('separators ffffffff', rk[0x340:0x344] == b'\xff'*4 and rk[0x364:0x368] == b'\xff'*4)

# signature: self-consistent raw RSA (LE PKA operands)
n_le = int.from_bytes(rk[0x20:0x120], 'little')
S_le = int.from_bytes(rk[0x220:0x320], 'little')
msg_le = int.from_bytes(rk[0x368:0x468], 'little')
check('signature raw-RSA verifies vs message', pow(S_le, 65537, n_le) == msg_le)

print()
print('FAILURES:', len(fails))
sys.exit(1 if fails else 0)
EOF
