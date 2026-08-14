// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2015 Google,  Inc
 * Written by Simon Glass <sjg@chromium.org>
 *
 * (C) 2017 Theobroma Systems Design und Consulting GmbH
 *
 * Helper functions for Rockchip images
 */

#include "imagetool.h"
#include <image.h>
#include <u-boot/sha256.h>
#include <rc4.h>
#include "mkimage.h"
#include "rkcommon.h"
#if CONFIG_IS_ENABLED(LIBCRYPTO)
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#endif

enum {
	RK_MAGIC		= 0x0ff0aa55,
	RK_MAGIC_V2		= 0x534E4B52,
};

enum {
	RK_HEADER_V1	= 1,
	RK_HEADER_V2	= 2,
};

/*
 * Secure-boot (signed) idblock constants, from the RK3399 BootROM's
 * rump_roast()/rsa_verify() code paths and the rk_sign_tool output format:
 *
 * The payload (at init_offset * 512 = 0x800) starts with an 0x800-byte
 * "RSAK" block containing the public key, the RSA signature and the
 * SHA256 hashes of the "init" (TPL) and "boot" (SPL) stages.
 */
enum {
	RK_RSAK_HDR_START	= RK_SPL_HDR_START,	/* 0x800 */
	RK_RSAK_SIZE		= 0x800,
	RK_SIGNED_HDR_SIZE	= RK_SPL_HDR_START + RK_RSAK_SIZE, /* 0x1000 */
	RK_RSAK_FLAGS_OFF	= 0x08,
	RK_RSAK_LEN_OFF		= 0x0c,
	RK_RSAK_KEY_OFF		= 0x20,	/* modulus (0x100) + e + pad */
	RK_RSAK_KEY_SIZE	= 0x200,
	RK_RSAK_EXP_OFF		= 0x120,	/* exponent slot */
	RK_RSAK_SIG_OFF		= 0x220,
	RK_RSAK_SIG_SIZE	= 0x100,
	RK_RSAK_HASH1_OFF	= 0x320,
	RK_RSAK_HASH2_OFF	= 0x344,
	RK_RSAK_MSG_OFF		= 0x368,	/* RSA message (raw, no padding) */
	RK_RSAK_MSG_SIZE	= 0x100,
	RK_RSAK_FLAGS		= 0x323,
	RK_RSAK_DATA_LEN	= 0xda,	/* in 4-byte words: covers 0..0x368 */
};

enum hash_type {
	HASH_NONE	= 0,
	HASH_SHA256	= 1,
	HASH_SHA512	= 2,
};

/**
 * struct image_entry
 *
 * @size_and_off:	[31:16]image size;[15:0]image offset
 * @address:	default as 0xFFFFFFFF
 * @flag:	no use
 * @counter:	no use
 * @hash:	hash of image
 *
 */
struct image_entry {
	uint32_t size_and_off;
	uint32_t address;
	uint32_t flag;
	uint32_t counter;
	uint8_t reserved[8];
	uint8_t hash[64];
};

/**
 * struct header0_info_v2 - v2 header block for rockchip BootRom
 *
 * This is stored at SD card block 64 (where each block is 512 bytes)
 *
 * @magic:	Magic (must be RK_MAGIC_V2)
 * @size_and_nimage:	[31:16]number of images;[15:0]
 *			offset to hash field of header(unit as 4Byte)
 * @boot_flag:	[3:0]hash type(0:none,1:sha256,2:sha512)
 * @signature:	hash or signature for header info
 *
 */
struct header0_info_v2 {
	uint32_t magic;
	uint8_t reserved[4];
	uint32_t size_and_nimage;
	uint32_t boot_flag;
	uint8_t reserved1[104];
	struct image_entry images[4];
	uint8_t reserved2[1064];
	uint8_t hash[512];
};

/**
 * struct header0_info - header block for boot ROM
 *
 * This is stored at MMC block 64 (where each block is 512 bytes), or at
 * the start of SPI flash. It is encoded with RC4 with the below rc4_key.
 *
 * In Rockchip terminology:
 *
 * "init" means the stage that is loaded into SRAM. TPL if there is one, SPL
 * otherwise.
 *
 * "boot" means the next stages after "init" stage that are loaded by the
 * BootROM into DRAM. Only applicable if "init" stage returns to BootROM (via
 * the appropriate ROCKCHIP_BACK_TO_BROM symbol, BOOT_DEVICE_BOOTROM is used as
 * boot device for the next stage and the "init" stage successfully booted) and
 * if "init_boot_size" > "init_size".
 * Basically, it is the content of "init" plus the SPL or even U-Boot proper if
 * relevant.
 *
 * @magic:		Magic (must be RK_MAGIC)
 * @disable_rc4:	0 to use rc4 for boot image,  1 to use plain binary
 * @init_offset:	Offset in 512-byte blocks of the "init" code from the
 *			start of this header. For instance, 4 means 2KiB.
 * @init_size:		Size (in blocks) of the "init" code.
 * @init_boot_size:	Size (in blocks) of the "init" and "boot" code combined.
 *
 * Other fields are not used by U-Boot
 */
struct header0_info {
	uint32_t magic;
	uint8_t reserved[4];
	uint32_t disable_rc4;
	uint16_t init_offset;
	uint8_t reserved1[492];
	uint16_t init_size;
	uint16_t init_boot_size;
	uint8_t reserved2[2];
};

/**
 * struct header1_info
 */
struct header1_info {
	uint32_t magic;
};

/**
 * struct spl_info - spl info for each chip
 *
 * @imagename:		Image name(passed by "mkimage -n")
 * @spl_hdr:		Boot ROM requires a 4-bytes spl header
 * @spl_size:		Spl size(include extra 4-bytes spl header)
 * @spl_rc4:		RC4 encode the SPL binary (same key as header)
 * @header_ver:		header block version
 */
struct spl_info {
	const char *imagename;
	const char *spl_hdr;
	const uint32_t spl_size;
	const bool spl_rc4;
	const uint32_t header_ver;
};

static struct spl_info spl_infos[] = {
	{ "px30", "RK33", 0x2800, false, RK_HEADER_V1 },
	{ "rk3036", "RK30", 0x1000, false, RK_HEADER_V1 },
	{ "rk3066", "RK30", 0x8000 - 0x800, true, RK_HEADER_V1 },
	{ "rk3128", "RK31", 0x1800, false, RK_HEADER_V1 },
	{ "rk3188", "RK31", 0x8000 - 0x800, true, RK_HEADER_V1 },
	{ "rk322x", "RK32", 0x8000 - 0x1000, false, RK_HEADER_V1 },
	{ "rk3288", "RK32", 0x8000, false, RK_HEADER_V1 },
	{ "rk3308", "RK33", 0x40000 - 0x1000, false, RK_HEADER_V1 },
	{ "rk3328", "RK32", 0x8000 - 0x800, false, RK_HEADER_V1 },
	{ "rk3368", "RK33", 0x8000 - 0x1000, false, RK_HEADER_V1 },
	{ "rk3399", "RK33", 0x30000 - 0x2000, false, RK_HEADER_V1 },
	{ "rk3506", "RK35", 0xC000 - 0x1000, false, RK_HEADER_V2 },
	{ "rk3528", "RK35", 0x10000 - 0x1000, false, RK_HEADER_V2 },
	{ "rk3568", "RK35", 0x10000 - 0x1000, false, RK_HEADER_V2 },
	{ "rk3576", "RK35", 0x80000 - 0x1000, false, RK_HEADER_V2 },
	{ "rk3588", "RK35", 0x100000 - 0x1000, false, RK_HEADER_V2 },
	{ "rv1108", "RK11", 0x1800, false, RK_HEADER_V1 },
	{ "rv1126", "110B", 0x10000 - 0x1000, false, RK_HEADER_V1 },
};

/**
 * struct spl_params - spl params parsed in check_params()
 *
 * @init_file:		Init data file path
 * @init_size:		Aligned size of init data in bytes
 * @boot_file:		Boot data file path
 * @boot_size:		Aligned size of boot data in bytes
 */

struct spl_params {
	char *init_file;
	uint32_t init_size;
	char *boot_file;
	uint32_t boot_size;
};

static struct spl_params spl_params = { 0 };

#if CONFIG_IS_ENABLED(LIBCRYPTO)
static uint8_t *rk_signed_read_file(struct image_tool_params *params,
				    const char *fname, size_t *sizep)
{
	struct stat sbuf;
	uint8_t *buf;
	int fd, ret;

	fd = open(fname, O_RDONLY | O_BINARY);
	if (fd < 0) {
		fprintf(stderr, "%s: Can't open %s: %s\n",
			params->cmdname, fname, strerror(errno));
		return NULL;
	}
	if (fstat(fd, &sbuf) < 0) {
		fprintf(stderr, "%s: Can't stat %s: %s\n",
			params->cmdname, fname, strerror(errno));
		close(fd);
		return NULL;
	}
	buf = malloc(sbuf.st_size);
	if (!buf) {
		fprintf(stderr, "%s: Out of memory reading %s\n",
			params->cmdname, fname);
		close(fd);
		return NULL;
	}
	ret = read(fd, buf, sbuf.st_size);
	close(fd);
	if (ret != sbuf.st_size) {
		fprintf(stderr, "%s: Can't read %s\n", params->cmdname, fname);
		free(buf);
		return NULL;
	}
	*sizep = sbuf.st_size;

	return buf;
}

static int rk_sha256(const void *data, size_t len, uint8_t *out)
{
	size_t outlen = 0;

	if (EVP_Q_digest(NULL, "SHA256", NULL, data, len, out, &outlen) != 1)
		return -1;

	return 0;
}

/*
 * Load the RSA private key and store its modulus big-endian into `modulus`
 * (must be 256 bytes for RSA-2048).
 */
static EVP_PKEY *rk_signed_load_key(struct image_tool_params *params,
				    uint8_t *modulus)
{
	BIGNUM *n = NULL;
	EVP_PKEY *pkey;
	FILE *f;
	int i;

	f = fopen(params->rk_key_file, "rb");
	if (!f) {
		fprintf(stderr, "%s: Can't open key %s: %s\n",
			params->cmdname, params->rk_key_file, strerror(errno));
		return NULL;
	}
	pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
	fclose(f);
	if (!pkey) {
		fprintf(stderr, "%s: Can't parse key %s (PEM RSA private key expected)\n",
			params->cmdname, params->rk_key_file);
		return NULL;
	}
	if (!EVP_PKEY_get_bn_param(pkey, "n", &n) || !n ||
	    BN_bn2binpad(n, modulus, 256) != 256) {
		fprintf(stderr, "%s: Key %s is not a 2048-bit RSA key\n",
			params->cmdname, params->rk_key_file);
		BN_free(n);
		EVP_PKEY_free(pkey);
		return NULL;
	}
	BN_free(n);
	/* The PKA engine reads operands little-endian (like the exponent
	 * "01 00 01 00"), so store the modulus byte-reversed.
	 */
	for (i = 0; i < 128; i++) {
		uint8_t tmp = modulus[i];

		modulus[i] = modulus[255 - i];
		modulus[255 - i] = tmp;
	}

	return pkey;
}

/*
 * Raw (unpadded) RSA sign, little-endian operands, matching the Rockchip
 * PKA engine convention (the exponent is stored "01 00 01 00" i.e. 65537
 * little-endian).
 */
static int rk_signed_raw_rsa_sign(EVP_PKEY *pkey, const uint8_t *msg_le,
				  uint8_t *sig_le)
{
	EVP_PKEY_CTX *ctx;
	uint8_t msg_be[256], sig_be[256];
	size_t siglen = sizeof(sig_be);
	int i, ret;

	for (i = 0; i < 256; i++)
		msg_be[i] = msg_le[255 - i];

	ctx = EVP_PKEY_CTX_new(pkey, NULL);
	if (!ctx)
		return -1;
	ret = EVP_PKEY_sign_init(ctx);
	if (!ret)
		goto out;
	ret = EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_NO_PADDING);
	if (ret > 0)
		ret = EVP_PKEY_sign(ctx, sig_be, &siglen, msg_be, 256);
out:
	EVP_PKEY_CTX_free(ctx);
	if (ret != 1 || siglen != sizeof(sig_be))
		return -1;

	for (i = 0; i < 256; i++)
		sig_le[i] = sig_be[255 - i];

	return 0;
}

/*
 * Build the 0x800-byte RSAK block that the BootROM's rump_roast() expects:
 *
 *   +0x00: "RSAK" magic, flags (0x323), data length (0xda)
 *   +0x20: 0x200-byte key blob (modulus || 65537-le || zeros); the SHA256
 *          of this blob is the value fused into efuse for provisioning
 *   +0x220: 256-byte raw RSA signature over the message at +0x368
 *   +0x320: hash1 = SHA256(init stage), 0xffffffff, hash2 = SHA256(boot stage)
 *   +0x368: 256-byte message: SHA256(hash1 || hash2) followed by zeros
 *           (exact message layout TBD, see FORMAT.md notes)
 */
static int rk_signed_build_rsak(struct image_tool_params *params, void *buf)
{
	uint8_t *blk = buf;
	uint8_t *init, *boot;
	size_t init_size, boot_size;
	uint8_t hash1[32], hash2[32], keyblob[RK_RSAK_KEY_SIZE];
	EVP_PKEY *pkey;
	int i;

	init = rk_signed_read_file(params, spl_params.init_file, &init_size);
	if (!init)
		return -1;
	boot = rk_signed_read_file(params, spl_params.boot_file, &boot_size);
	if (!boot) {
		free(init);
		return -1;
	}

	if (rk_sha256(init, init_size, hash1) ||
	    rk_sha256(boot, boot_size, hash2)) {
		fprintf(stderr, "%s: SHA256 failed\n", params->cmdname);
		free(init);
		free(boot);
		return -1;
	}
	free(init);
	free(boot);

	memset(blk, 0, RK_RSAK_SIZE);
	memcpy(blk, "RSAK", 4);
	*(uint32_t *)(blk + RK_RSAK_FLAGS_OFF) = cpu_to_le32(RK_RSAK_FLAGS);
	*(uint32_t *)(blk + RK_RSAK_LEN_OFF) = cpu_to_le32(RK_RSAK_DATA_LEN);

	/* key blob: modulus || exponent 65537 (LE) || zeros */
	memset(keyblob, 0, sizeof(keyblob));
	pkey = rk_signed_load_key(params, keyblob);
	if (!pkey)
		return -1;
	memcpy(keyblob + RK_RSAK_EXP_OFF - RK_RSAK_KEY_OFF, "\x01\x00\x01\x00",
	       4);
	memcpy(blk + RK_RSAK_KEY_OFF, keyblob, sizeof(keyblob));

	/* payload hashes */
	memcpy(blk + RK_RSAK_HASH1_OFF, hash1, 32);
	memset(blk + RK_RSAK_HASH1_OFF + 32, 0xff, 4);
	memcpy(blk + RK_RSAK_HASH2_OFF, hash2, 32);
	memset(blk + RK_RSAK_HASH2_OFF + 32, 0xff, 4);

	/* message: SHA256(hash1 || hash2) || zeros (exact layout TBD) */
	{
		uint8_t hcat[64];

		memcpy(hcat, hash1, 32);
		memcpy(hcat + 32, hash2, 32);
		if (rk_sha256(hcat, sizeof(hcat), blk + RK_RSAK_MSG_OFF)) {
			fprintf(stderr, "%s: SHA256 failed\n", params->cmdname);
			EVP_PKEY_free(pkey);
			return -1;
		}
	}

	if (rk_signed_raw_rsa_sign(pkey, blk + RK_RSAK_MSG_OFF,
				   blk + RK_RSAK_SIG_OFF)) {
		fprintf(stderr, "%s: RSA signing failed\n", params->cmdname);
		EVP_PKEY_free(pkey);
		return -1;
	}
	EVP_PKEY_free(pkey);

	/* The efuse value to provision for this key */
	if (rk_sha256(blk + RK_RSAK_KEY_OFF, RK_RSAK_KEY_SIZE, keyblob))
		return -1;
	fprintf(stderr, "Rockchip key blob SHA256 (for efuse provisioning): ");
	for (i = 0; i < 32; i++)
		fprintf(stderr, "%02x", keyblob[i]);
	fprintf(stderr, "\n");

	return 0;
}
#endif /* CONFIG_IS_ENABLED(LIBCRYPTO) */

static const unsigned char rc4_key[16] = {
	124, 78, 3, 4, 85, 5, 9, 7,
	45, 44, 123, 56, 23, 13, 23, 17
};

static struct spl_info *rkcommon_get_spl_info(char *imagename)
{
	int i;

	if (!imagename)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(spl_infos); i++)
		if (!strncmp(imagename, spl_infos[i].imagename, 6))
			return spl_infos + i;

	return NULL;
}

static int rkcommon_get_aligned_size(struct image_tool_params *params,
				     const char *fname)
{
	int size;

	size = imagetool_get_filesize(params, fname);
	if (size < 0)
		return -1;

	/*
	 * Pad to a 2KB alignment, as required for init/boot size by the ROM
	 * (see https://patch.msgid.link/CANbgqAQPw5Dxp7Qm_aZoL1nggg9Mm1SQoKvx=V5a8p-9+DTDNw@mail.gmail.com/)
	 */
	return ROUND(size, RK_SIZE_ALIGN);
}

static bool rkcommon_is_header_v2(struct image_tool_params *params)
{
	struct spl_info *info = rkcommon_get_spl_info(params->imagename);

	return (info->header_ver == RK_HEADER_V2);
}

/**
 * rkcommon_is_signed() - check if secure-boot signing was requested
 *
 * Signing is opt-in via the --rk-key option and is only implemented
 * for the v1-header SoCs (rk3399 family).
 */
bool rkcommon_is_signed(struct image_tool_params *params)
{
	return params->rk_key_file && !rkcommon_is_header_v2(params);
}

int rkcommon_check_params(struct image_tool_params *params)
{
	int i, size;

	/*
	 * If this is a operation (list or extract), the don't require
	 * imagename to be set.
	 */
	if (params->lflag || params->iflag)
		return EXIT_SUCCESS;

	if (!rkcommon_get_spl_info(params->imagename))
		goto err_spl_info;

	spl_params.init_file = params->datafile;

	spl_params.boot_file = strchr(spl_params.init_file, ':');
	if (spl_params.boot_file) {
		*spl_params.boot_file = '\0';
		spl_params.boot_file += 1;
	}

	size = rkcommon_get_aligned_size(params, spl_params.init_file);
	if (size < 0)
		return EXIT_FAILURE;
	spl_params.init_size = size;

	/* Boot file is optional, and only for back-to-bootrom functionality. */
	if (spl_params.boot_file) {
		size = rkcommon_get_aligned_size(params, spl_params.boot_file);
		if (size < 0)
			return EXIT_FAILURE;
		spl_params.boot_size = size;
	}

	if (params->rk_key_file) {
		if (rkcommon_is_header_v2(params)) {
			fprintf(stderr, "ERROR: --rk-key only supports v1-header SoCs (rk3399 family)\n");
			return EXIT_FAILURE;
		}
		if (!spl_params.boot_file) {
			fprintf(stderr, "ERROR: --rk-key requires an init:boot pair (-d tpl.bin:spl.bin)\n");
			return EXIT_FAILURE;
		}
	}

	if (spl_params.init_size > rkcommon_get_spl_size(params)) {
		fprintf(stderr,
			"Error: SPL image is too large (size %#x than %#x)\n",
			spl_params.init_size, rkcommon_get_spl_size(params));
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;

err_spl_info:
	fprintf(stderr, "ERROR: imagename (%s) is not supported!\n",
		params->imagename ? params->imagename : "NULL");

	fprintf(stderr, "Available imagename:");
	for (i = 0; i < ARRAY_SIZE(spl_infos); i++)
		fprintf(stderr, "\t%s", spl_infos[i].imagename);
	fprintf(stderr, "\n");

	return EXIT_FAILURE;
}

const char *rkcommon_get_spl_hdr(struct image_tool_params *params)
{
	struct spl_info *info = rkcommon_get_spl_info(params->imagename);

	/*
	 * info would not be NULL, because of we checked params before.
	 */
	return info->spl_hdr;
}

int rkcommon_get_spl_size(struct image_tool_params *params)
{
	struct spl_info *info = rkcommon_get_spl_info(params->imagename);

	/*
	 * info would not be NULL, because of we checked params before.
	 */
	return info->spl_size;
}

bool rkcommon_need_rc4_spl(struct image_tool_params *params)
{
	struct spl_info *info = rkcommon_get_spl_info(params->imagename);

	/*
	 * info would not be NULL, because of we checked params before.
	 */
	return info->spl_rc4;
}

static void do_sha256_hash(uint8_t *buf, uint32_t size, uint8_t *out)
{
	sha256_context ctx;

	sha256_starts(&ctx);
	sha256_update(&ctx, buf, size);
	sha256_finish(&ctx, out);
}

static void rkcommon_set_header0(void *buf, struct image_tool_params *params)
{
	struct header0_info *hdr = buf;
	uint32_t init_boot_size;

	memset(buf, '\0', RK_INIT_OFFSET * RK_BLK_SIZE);
	hdr->magic = cpu_to_le32(RK_MAGIC);
	hdr->disable_rc4 = cpu_to_le32(!rkcommon_need_rc4_spl(params));
	hdr->init_offset = cpu_to_le16(RK_INIT_OFFSET);
	if (rkcommon_is_signed(params)) {
		/*
		 * Signed idblock: the payload is RC4-encoded like the ROM
		 * expects (disable_rc4 = 0) and holds the RSAK block plus
		 * both the init and boot stages, matching the rk_sign_tool
		 * output format.
		 */
		hdr->disable_rc4 = 0;
		hdr->init_size = cpu_to_le16((RK_RSAK_SIZE +
					      spl_params.init_size +
					      spl_params.boot_size) /
					     RK_BLK_SIZE);
		hdr->init_boot_size = 0;
	} else {
		hdr->init_size = cpu_to_le16(spl_params.init_size /
					     RK_BLK_SIZE);
	}

	if (!rkcommon_is_signed(params)) {
		/*
		 * init_boot_size needs to be set, as it is read by the BootROM
		 * to determine the size of the next-stage bootloader (e.g. U-Boot
		 * proper), when used with the back-to-bootrom functionality.
		 *
		 * see https://patch.msgid.link/CANbgqAQ5p-fE4T-Ye9UAMs-nRGU8AP_yxDf+5SbZbP8bxLa0dg@mail.gmail.com/
		 * for a more detailed explanation by Andy Yan
		 */
		if (spl_params.boot_file)
			init_boot_size = spl_params.init_size +
					 spl_params.boot_size;
		else
			init_boot_size = spl_params.init_size + RK_MAX_BOOT_SIZE;
		hdr->init_boot_size = cpu_to_le16(init_boot_size / RK_BLK_SIZE);
	}

	rc4_encode(buf, RK_BLK_SIZE, rc4_key);
}

static void rkcommon_set_header0_v2(void *buf, struct image_tool_params *params)
{
	struct header0_info_v2 *hdr = buf;
	uint32_t sector_offset, image_sector_count;
	uint32_t image_size_array[2];
	uint8_t *image_ptr = NULL;
	int i;

	printf("Image Type:   Rockchip %s boot image\n",
		rkcommon_get_spl_hdr(params));
	memset(buf, '\0', RK_INIT_OFFSET * RK_BLK_SIZE);
	hdr->magic   = cpu_to_le32(RK_MAGIC_V2);
	hdr->size_and_nimage = cpu_to_le32((2 << 16) + 384);
	hdr->boot_flag = cpu_to_le32(HASH_SHA256);
	sector_offset = 4;
	image_size_array[0] = spl_params.init_size;
	image_size_array[1] = spl_params.boot_size;

	for (i = 0; i < 2; i++) {
		image_sector_count = image_size_array[i] / RK_BLK_SIZE;
		hdr->images[i].size_and_off = cpu_to_le32((image_sector_count
							<< 16) + sector_offset);
		hdr->images[i].address = 0xFFFFFFFF;
		hdr->images[i].counter = cpu_to_le32(i + 1);
		image_ptr = buf + sector_offset * RK_BLK_SIZE;
		do_sha256_hash(image_ptr, image_size_array[i],
			       hdr->images[i].hash);
		sector_offset = sector_offset + image_sector_count;
	}

	do_sha256_hash(buf, (void *)hdr->hash - buf, hdr->hash);
}

void rkcommon_set_header(void *buf,  struct stat *sbuf,  int ifd,
			 struct image_tool_params *params)
{
	struct header1_info *hdr = buf + RK_SPL_HDR_START;

	if (rkcommon_is_header_v2(params)) {
		rkcommon_set_header0_v2(buf, params);
	} else {
		rkcommon_set_header0(buf, params);

		/* Set up the SPL name (i.e. copy spl_hdr over) */
		if (memcmp(&hdr->magic, "RSAK", 4))
			memcpy(&hdr->magic, rkcommon_get_spl_hdr(params), RK_SPL_HDR_SIZE);

		if (rkcommon_need_rc4_spl(params))
			rkcommon_rc4_encode_spl(buf, RK_SPL_HDR_START,
						spl_params.init_size);

		if (spl_params.boot_file) {
			if (rkcommon_need_rc4_spl(params))
				rkcommon_rc4_encode_spl(buf + RK_SPL_HDR_START,
							spl_params.init_size,
							spl_params.boot_size);
		}

		if (rkcommon_is_signed(params)) {
			/*
			 * Signed idblock: RC4-encode the RSAK block and the
			 * payloads, 512 bytes at a time with a fresh cipher
			 * state per block, as the BootROM decodes them.
			 */
			off_t off;

			for (off = RK_SPL_HDR_START; off < sbuf->st_size;
			     off += RK_BLK_SIZE)
				rc4_encode(buf + off, RK_BLK_SIZE, rc4_key);
		}
	}
}

static inline unsigned int rkcommon_offset_to_spi(unsigned int offset)
{
	/*
	 * While SD/MMC images use a flat addressing, SPI images are padded
	 * to use the first 2K of every 4K sector only.
	 */
	return ((offset & ~0x7ff) << 1) + (offset & 0x7ff);
}

static int rkcommon_parse_header(const void *buf, struct header0_info *header0,
				 struct spl_info **spl_info)
{
	unsigned int hdr1_offset;
	struct header1_info *hdr1_sdmmc, *hdr1_spi;
	uint8_t dec_sd[RK_BLK_SIZE], dec_spi[RK_BLK_SIZE];
	int i;

	if (spl_info)
		*spl_info = NULL;

	/*
	 * The first header (hdr0) is always RC4 encoded, so try to decrypt
	 * with the well-known key.
	 */
	memcpy((void *)header0, buf, sizeof(struct header0_info));
	rc4_encode((void *)header0, sizeof(struct header0_info), rc4_key);

	if (le32_to_cpu(header0->magic) != RK_MAGIC)
		return -EPROTO;

	hdr1_offset = le16_to_cpu(header0->init_offset) * RK_BLK_SIZE;
	hdr1_sdmmc = (struct header1_info *)(buf + hdr1_offset);
	hdr1_spi = (struct header1_info *)(buf +
					   rkcommon_offset_to_spi(hdr1_offset));

	/*
	 * Signed images carry an RC4-encoded payload (disable_rc4 == 0),
	 * decode the first block of both layouts to find the payload magic.
	 */
	if (le32_to_cpu(header0->disable_rc4) == 0) {
		memcpy(dec_sd, buf + hdr1_offset, RK_BLK_SIZE);
		rc4_encode(dec_sd, RK_BLK_SIZE, rc4_key);
		memcpy(dec_spi, buf + rkcommon_offset_to_spi(hdr1_offset),
		       RK_BLK_SIZE);
		rc4_encode(dec_spi, RK_BLK_SIZE, rc4_key);
		hdr1_sdmmc = (struct header1_info *)dec_sd;
		hdr1_spi = (struct header1_info *)dec_spi;
	}

	for (i = 0; i < ARRAY_SIZE(spl_infos); i++) {
		if (!memcmp(&hdr1_sdmmc->magic, spl_infos[i].spl_hdr,
			    RK_SPL_HDR_SIZE)) {
			if (spl_info)
				*spl_info = &spl_infos[i];
			return IH_TYPE_RKSD;
		} else if (!memcmp(&hdr1_spi->magic, spl_infos[i].spl_hdr,
				   RK_SPL_HDR_SIZE)) {
			if (spl_info)
				*spl_info = &spl_infos[i];
			return IH_TYPE_RKSPI;
		}
	}

	/* Signed images: the payload starts with the RSAK block */
	if (!memcmp(&hdr1_sdmmc->magic, "RSAK", RK_SPL_HDR_SIZE))
		return IH_TYPE_RKSD;
	if (!memcmp(&hdr1_spi->magic, "RSAK", RK_SPL_HDR_SIZE))
		return IH_TYPE_RKSPI;

	return -1;
}

static int rkcommon_parse_header_v2(const void *buf, struct header0_info_v2 *header)
{
	memcpy((void *)header, buf, sizeof(struct header0_info_v2));

	if (le32_to_cpu(header->magic) != RK_MAGIC_V2)
		return -EPROTO;

	return 0;
}

int rkcommon_verify_header(unsigned char *buf, int size,
			   struct image_tool_params *params)
{
	struct header0_info header0;
	struct spl_info *img_spl_info, *spl_info;
	int ret;

	/* spl_hdr is abandon on header_v2 */
	if ((*(uint32_t *)buf) == RK_MAGIC_V2)
		return 0;

	ret = rkcommon_parse_header(buf, &header0, &img_spl_info);

	/* If this is the (unimplemented) RC4 case, then rewrite the result */
	if (ret == -ENOSYS)
		return 0;

	if (ret < 0)
		return ret;

	/*
	 * If no 'imagename' is specified via the commandline (e.g. if this is
	 * 'dumpimage -l' w/o any further constraints), we accept any spl_info.
	 */
	if (params->imagename == NULL || !strlen(params->imagename))
		return 0;

	/* Match the 'imagename' against the 'spl_hdr' found */
	if (img_spl_info) {
		spl_info = rkcommon_get_spl_info(params->imagename);
		if (!spl_info)
			return -ENOENT;
		return strcmp(spl_info->spl_hdr, img_spl_info->spl_hdr);
	}

	/* Signed images ("RSAK") carry no chip tag: accept */
	return 0;
}

void rkcommon_print_header(const void *buf, struct image_tool_params *params)
{
	struct header0_info header0;
	struct header0_info_v2 header0_v2;
	struct spl_info *spl_info;
	uint8_t image_type;
	int ret, boot_size, init_size;

	if ((*(uint32_t *)buf) == RK_MAGIC_V2) {
		ret = rkcommon_parse_header_v2(buf, &header0_v2);

		if (ret < 0) {
			fprintf(stderr, "Error: image verification failed\n");
			return;
		}

		init_size = header0_v2.images[0].size_and_off >> 16;
		init_size = init_size * RK_BLK_SIZE;
		boot_size = header0_v2.images[1].size_and_off >> 16;
		boot_size = boot_size * RK_BLK_SIZE;
	} else {
		ret = rkcommon_parse_header(buf, &header0, &spl_info);

		/* If this is the (unimplemented) RC4 case, then fail silently */
		if (ret == -ENOSYS)
			return;

		if (ret < 0) {
			fprintf(stderr, "Error: image verification failed\n");
			return;
		}

		image_type = ret;
		init_size = header0.init_size * RK_BLK_SIZE;
		boot_size = header0.init_boot_size * RK_BLK_SIZE - init_size;

		if (spl_info) {
			printf("Image Type:   Rockchip %s (%s) boot image\n",
			       spl_info->spl_hdr,
			       (image_type == IH_TYPE_RKSD) ? "SD/MMC" : "SPI");
		} else {
			printf("Image Type:   Rockchip RSAK (%s) signed boot image\n",
			       (image_type == IH_TYPE_RKSD) ? "SD/MMC" : "SPI");
		}
	}

	printf("Init Data Size: %d bytes\n", init_size);

	if (boot_size != RK_MAX_BOOT_SIZE)
		printf("Boot Data Size: %d bytes\n", boot_size);
}

void rkcommon_rc4_encode_spl(void *buf, unsigned int offset, unsigned int size)
{
	unsigned int remaining = size;

	while (remaining > 0) {
		int step = (remaining > RK_BLK_SIZE) ? RK_BLK_SIZE : remaining;

		rc4_encode(buf + offset, step, rc4_key);
		offset += RK_BLK_SIZE;
		remaining -= step;
	}
}

int rkcommon_vrec_header(struct image_tool_params *params,
			 struct image_type_params *tparams)
{
	/*
	 * The SPL image looks as follows:
	 *
	 * 0x0    header0 (see rkcommon.c)
	 * 0x800  spl_name ('RK30', ..., 'RK33')
	 *        (start of the payload for AArch64 payloads: we expect the
	 *        first 4 bytes to be available for overwriting with our
	 *        spl_name)
	 * 0x804  first instruction to be executed
	 *        (start of the image/payload for 32bit payloads)
	 *
	 * For AArch64 (ARMv8) payloads, natural alignment (8-bytes) is
	 * required for its sections (so the image we receive needs to
	 * have the first 4 bytes reserved for the spl_name).  Reserving
	 * these 4 bytes is done using the BOOT0_HOOK infrastructure.
	 *
	 * The header is always at 0x800 (as we now use a payload
	 * prepadded using the boot0 hook for all targets): the first
	 * 4 bytes of these images can safely be overwritten using the
	 * boot magic.
	 */
	tparams->header_size = rkcommon_is_signed(params) ?
		RK_SIGNED_HDR_SIZE : RK_SPL_HDR_START;

	/* Allocate, clear and install the header */
	tparams->hdr = malloc(tparams->header_size);
	if (!tparams->hdr) {
		fprintf(stderr, "%s: Can't alloc header: %s\n",
			params->cmdname, strerror(errno));
		exit(EXIT_FAILURE);
	}
	memset(tparams->hdr, 0, tparams->header_size);

	if (rkcommon_is_signed(params)) {
#if CONFIG_IS_ENABLED(LIBCRYPTO)
		/* Build the RSAK signature block right after the header */
		if (rk_signed_build_rsak(params,
					 tparams->hdr + RK_SPL_HDR_START)) {
			fprintf(stderr, "%s: signing failed\n",
				params->cmdname);
			exit(EXIT_FAILURE);
		}
#else
		fprintf(stderr, "%s: signing requires CONFIG_TOOLS_LIBCRYPTO\n",
			params->cmdname);
		exit(EXIT_FAILURE);
#endif
	}

	/*
	 * We need to store the original file-size (i.e. before padding), as
	 * imagetool does not set this during its adjustment of file_size.
	 */
	params->orig_file_size = tparams->header_size +
		spl_params.init_size + spl_params.boot_size;

	params->file_size = ROUND(params->orig_file_size, RK_SIZE_ALIGN);

	/* Ignoring pad len, since we are using our own copy_image() */
	return 0;
}

static int pad_file(struct image_tool_params *params, int ifd, int pad)
{
	uint8_t zeros[4096];

	memset(zeros, 0, sizeof(zeros));

	while (pad > 0) {
		int todo = sizeof(zeros);

		if (todo > pad)
			todo = pad;
		if (write(ifd, (char *)&zeros, todo) != todo) {
			fprintf(stderr, "%s: Write error on %s: %s\n",
				params->cmdname, params->imagefile,
				strerror(errno));
			return -1;
		}
		pad -= todo;
	}

	return 0;
}

static int copy_file(struct image_tool_params *params, int ifd,
		     const char *file, int padded_size)
{
	int dfd;
	struct stat sbuf;
	unsigned char *ptr;
	int size;

	if (params->vflag)
		fprintf(stderr, "Adding Image %s\n", file);

	dfd = open(file, O_RDONLY | O_BINARY);
	if (dfd < 0) {
		fprintf(stderr, "%s: Can't open %s: %s\n",
			params->cmdname, file, strerror(errno));
		return -1;
	}

	if (fstat(dfd, &sbuf) < 0) {
		fprintf(stderr, "%s: Can't stat %s: %s\n",
			params->cmdname, file, strerror(errno));
		goto err_close;
	}

	if (params->vflag)
		fprintf(stderr, "Size %u(pad to %u)\n",
			(int)sbuf.st_size, padded_size);

	ptr = mmap(0, sbuf.st_size, PROT_READ, MAP_SHARED, dfd, 0);
	if (ptr == MAP_FAILED) {
		fprintf(stderr, "%s: Can't read %s: %s\n",
			params->cmdname, file, strerror(errno));
		goto err_munmap;
	}

	size = sbuf.st_size;
	if (write(ifd, ptr, size) != size) {
		fprintf(stderr, "%s: Write error on %s: %s\n",
			params->cmdname, params->imagefile, strerror(errno));
		goto err_munmap;
	}

	munmap((void *)ptr, sbuf.st_size);
	close(dfd);
	return pad_file(params, ifd, padded_size - size);

err_munmap:
	munmap((void *)ptr, sbuf.st_size);
err_close:
	close(dfd);
	return -1;
}

int rockchip_copy_image(int ifd, struct image_tool_params *params)
{
	int ret;

	ret = copy_file(params, ifd, spl_params.init_file,
			spl_params.init_size);
	if (ret)
		return ret;

	if (spl_params.boot_file) {
		ret = copy_file(params, ifd, spl_params.boot_file,
				spl_params.boot_size);
		if (ret)
			return ret;
	}

	return pad_file(params, ifd,
			params->file_size - params->orig_file_size);
}
