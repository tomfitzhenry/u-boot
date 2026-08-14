// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2026 Tom Fitzhenry <tom@tom-fitzhenry.me.uk>
 *
 * On RK3399, U-Boot proper boots OP-TEE from its own FIT.  The TEE's
 * memory carve-out is declared statically in rk3399-u-boot.dtsi so
 * that U-Boot's LMB and hence the EFI memory map exclude it.  The
 * /firmware/optee node, however, must only appear in the device tree
 * handed to the next OS: a static node would make U-Boot's own DM bind
 * its OP-TEE driver to it, interfering with the TEE's boot.
 */

#include <fdt_support.h>

#if CONFIG_IS_ENABLED(OPTEE)

int ft_board_setup(void *blob, struct bd_info *bd)
{
	int off, ret;

	/* If the OS's fdt already declares OP-TEE, assume it knows better. */
	if (fdt_path_offset(blob, "/firmware/optee") >= 0)
		return 0;

	off = fdt_path_offset(blob, "/firmware");
	if (off < 0) {
		off = fdt_path_offset(blob, "/");
		off = fdt_add_subnode(blob, off, "firmware");
		if (off < 0)
			return off;
	}

	off = fdt_add_subnode(blob, off, "optee");
	if (off < 0)
		return off;

	ret = fdt_setprop_string(blob, off, "compatible", "linaro,optee-tz");
	if (ret)
		return ret;

	return fdt_setprop_string(blob, off, "method", "smc");
}

#endif /* CONFIG_IS_ENABLED(OPTEE) */
