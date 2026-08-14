.. SPDX-License-Identifier: GPL-2.0+

RK3399 secure-boot idblock signing
==================================

When the secure-boot efuse is provisioned, the RK3399 BootROM no longer
accepts a plain idblock: the payload must start with an ``RSAK`` block
containing the RSA public key, an unpadded RSA-2048 signature and the
SHA256 hashes of the two boot stages.  mkimage can now build such an
image directly.

Usage
-----

.. code-block:: console

    $ openssl genrsa -out key.pem 2048
    $ mkimage -T rksd -n rk3399 --rk-key key.pem -d tpl.bin:spl.bin idbloader.img

The command prints the SHA256 of the key blob; fuse this value (the
"OTP hash") into the secure-boot efuse when provisioning the device.
With no ``--rk-key`` the image is built as before.

Format
------

The signed image keeps the block-64 layout (``header0`` at offset 0,
payload at offset 0x800) and inserts an 0x800-byte ``RSAK`` block ahead
of the TPL::

    0x000   header0 (RC4-encoded; disable_rc4 = 0, init_size covers
            the RSAK block plus both stages)
    0x800   RSAK block (see below), then the TPL, then the SPL

The payload is RC4-encoded in 512-byte blocks (fresh cipher state per
block), as the BootROM decodes it.  The RSAK block layout follows the
BootROM's ``rump_roast()``/``rsa_verify()`` expectations::

    0x00   "RSAK" magic
    0x08   flags (0x323)
    0x0c   data length in words (0xda, covers 0..0x368)
    0x20   key blob (0x200 bytes): RSA-2048 modulus (little-endian,
           like the PKA engine's operands) + exponent 65537 + zeros;
           its SHA256 is the fused efuse hash
    0x220  raw RSA-2048 signature (no padding) over the message
    0x320  hash1 = SHA256(init stage, i.e. the TPL), 0xffffffff,
           hash2 = SHA256(boot stage, i.e. the SPL)
    0x368  256-byte RSA message

Known limitations
-----------------

* The exact construction of the 256-byte RSA message region used by
  Rockchip's proprietary ``rk_sign_tool`` is not fully documented; this
  implementation uses ``SHA256(hash1 || hash2)`` followed by zeros and
  signs it raw with the PKA-style little-endian byte order.  The
  BootROM-side checks (key hash vs. efuse, raw RSA, both stage hashes)
  are matched, but the image has not been validated on hardware with a
  fused secure-boot efuse.
* ``rk_sign_tool`` itself always signs with an embedded development key
  (it ignores ``kk``/``lk``/certificate keys for the idblock path), so
  byte-for-byte comparison against its output is only possible with
  that key.
* SPI images (``-T rkspi``) are not covered yet.

References
----------

* ``tools/rk-sign-test.sh`` — self-contained test that builds a signed
  image with a throwaway key and checks the whole structure.
* The BootROM behaviour was reverse-engineered from the RK3399 BootROM
  disassembly (``rump_roast()``, ``rsa_verify()``, ``try_abc()``) and
  from black-box experiments with ``rk_sign_tool``/``boot_merger`` from
  the rkbin repository.
