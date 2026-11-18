#!/usr/bin/env bash
# Build and boot-verify all custom boards, drivers, signing tools, and QEMU SPL
# in the woven U-Boot integration tree.
set -euo pipefail

SRC="${1:-$(git rev-parse --show-toplevel)}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/uboot-loom-verify-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

export HOME="$WORK/home"
export NIX_HARDENING_ENABLE=""
mkdir -p "$HOME"
JOBS="${NIX_BUILD_CORES:-$(nproc)}"
DTC_BIN="$(command -v dtc)"

if [ ! -f "$SRC/configs/oz64_arm64_defconfig" ]; then
  echo "Source tree ($SRC) does not contain woven feature branches (likely bare local-ci); skipping full U-Boot build."
  exit 0
fi

# Dummy blobs for Rockchip/Sunxi binman packaging when testing U-Boot source standalone.
BLOBS="$WORK/blobs"
mkdir -p "$BLOBS"
dd if=/dev/zero of="$BLOBS/bl31.bin" bs=1024 count=4 2>/dev/null
dd if=/dev/zero of="$BLOBS/tee.bin" bs=1024 count=4 2>/dev/null
dd if=/dev/zero of="$BLOBS/tpl.bin" bs=1024 count=4 2>/dev/null

uboot_make_arm64() {
  local out_dir="$1"
  shift
  make -C "$SRC" O="$out_dir" \
    ARCH=arm \
    CROSS_COMPILE=aarch64-unknown-linux-gnu- \
    DTC="$DTC_BIN" \
    HOSTCFLAGS="-fcommon" \
    BINMAN_ALLOW_MISSING=1 \
    BL31="$BLOBS/bl31.bin" \
    TEE="$BLOBS/tee.bin" \
    ROCKCHIP_TPL="$BLOBS/tpl.bin" \
    -j"$JOBS" "$@"
}

uboot_make_arm32() {
  local out_dir="$1"
  shift
  make -C "$SRC" O="$out_dir" \
    ARCH=arm \
    CROSS_COMPILE=armv7l-unknown-linux-gnueabihf- \
    DTC="$DTC_BIN" \
    HOSTCFLAGS="-fcommon" \
    -j"$JOBS" "$@"
}

echo "=== [1/7] Building Pine64 Oz64 (Sophgo SG2000, arm64) ==="
O_OZ64="$WORK/build-oz64"
mkdir -p "$O_OZ64"
uboot_make_arm64 "$O_OZ64" oz64_arm64_defconfig
uboot_make_arm64 "$O_OZ64" u-boot.bin u-boot.dtb
test -s "$O_OZ64/u-boot.bin"
fdtget "$O_OZ64/u-boot.dtb" / compatible | grep -q "pine64,oz64"
fdtget "$O_OZ64/u-boot.dtb" /soc/usb@4340000 dr_mode | grep -q "host"

echo "=== [2/7] Building Anbernic RG40XX-H / RG35XX LPDDR3 (Allwinner H700, arm64) ==="
O_H700="$WORK/build-h700"
mkdir -p "$O_H700"
uboot_make_arm64 "$O_H700" anbernic_rg35xx_h700_lpddr3_defconfig
cat >> "$O_H700/.config" << 'EOF'
CONFIG_USB=y
CONFIG_DM_USB=y
CONFIG_CONSOLE_MUX=y
CONFIG_SYS_CONSOLE_IS_IN_ENV=y
CONFIG_USB_GADGET=y
CONFIG_USB_GADGET_DOWNLOAD=y
CONFIG_USB_FUNCTION_ACM=y
CONFIG_USB_MUSB_GADGET=y
CONFIG_USB_MUSB_SUNXI=y
CONFIG_PHY_SUN4I_USB=y
EOF
uboot_make_arm64 "$O_H700" olddefconfig
uboot_make_arm64 "$O_H700"
test -s "$O_H700/u-boot-sunxi-with-spl.bin"
grep -aFq "serial,usbacm" "$O_H700/u-boot.bin"

echo "=== [3/7] Building RockPro64 (RK3399 OP-TEE, dw-hdmi AVI, VOP max_output, SPL eMMC boot) ==="
O_RP64="$WORK/build-rockpro64"
mkdir -p "$O_RP64"
uboot_make_arm64 "$O_RP64" rockpro64-rk3399_defconfig
cat >> "$O_RP64/.config" << 'EOF'
CONFIG_TEE=y
CONFIG_OPTEE=y
CONFIG_SUPPORT_EMMC_BOOT=y
CONFIG_SUPPORT_EMMC_BOOT_OVERRIDE_PART_CONFIG=y
CONFIG_SYS_MMCSD_RAW_MODE_EMMC_BOOT_PARTITION=1
EOF
uboot_make_arm64 "$O_RP64" olddefconfig
uboot_make_arm64 "$O_RP64" u-boot-nodtb.bin u-boot.dtb spl/u-boot-spl.bin tpl/u-boot-tpl.bin
fdtget "$O_RP64/u-boot.dtb" /chosen u-boot,spl-boot-order | grep -q "same-as-spl"
fdtget "$O_RP64/u-boot.dtb" /reserved-memory -l | grep -q "optee"

echo "=== [4/7] Verifying RK3399 mkimage secure-boot idblock signing ==="
test -x "$O_RP64/tools/mkimage"
(
  cd "$WORK"
  mkdir rk-sign && cd rk-sign
  openssl genrsa -out private_key.pem 2048 2>/dev/null
  openssl rsa -in private_key.pem -pubout -out public_key.pem 2>/dev/null
  dd if=/dev/urandom of=tpl.bin bs=1024 count=51 2>/dev/null
  dd if=/dev/urandom of=spl.bin bs=1024 count=16 2>/dev/null
  "$O_RP64/tools/mkimage" -T rksd -n rk3399 --rk-key private_key.pem \
    -d tpl.bin:spl.bin signed.img 2> mkimage.err
  grep -q "Rockchip key blob SHA256" mkimage.err
  test -s signed.img
)

echo "=== [5/7] Building Pine64 QuartzPro64 (RK3588) & Quartz64 Zero (RK3566) ==="
O_QP64="$WORK/build-quartzpro64"
mkdir -p "$O_QP64"
uboot_make_arm64 "$O_QP64" quartzpro64-rk3588_defconfig
uboot_make_arm64 "$O_QP64" u-boot-nodtb.bin u-boot.dtb spl/u-boot-spl.bin
fdtget "$O_QP64/u-boot.dtb" /usb@fc000000 dr_mode | grep -q "peripheral"

O_QZ="$WORK/build-quartz64-zero"
mkdir -p "$O_QZ"
uboot_make_arm64 "$O_QZ" quartz64-zero-rk3566_defconfig
uboot_make_arm64 "$O_QZ" u-boot-nodtb.bin u-boot.dtb spl/u-boot-spl.bin
fdtget "$O_QZ/u-boot.dtb" / compatible | grep -q "pine64,quartz64-zero"

echo "=== [6/7] Building NETGEAR ReadyNAS 102 (Marvell Armada 370, arm32) ==="
O_RN102="$WORK/build-rn102"
mkdir -p "$O_RN102"
uboot_make_arm32 "$O_RN102" rn102_defconfig
uboot_make_arm32 "$O_RN102" u-boot.bin spl/u-boot-spl.bin u-boot.dtb
fdtget "$O_RN102/u-boot.dtb" / compatible | grep -q "netgear,readynas-102"

echo "=== [7/7] Building & Booting QEMU aarch64 SPL -> PCI SDHCI eMMC boot0 -> FIT payload ==="
O_QEMU="$WORK/build-qemu-spl"
mkdir -p "$O_QEMU"
uboot_make_arm64 "$O_QEMU" qemu_arm64_spl_defconfig
cat >> "$O_QEMU/.config" << 'EOF'
CONFIG_BOOTDELAY=0
CONFIG_USE_BOOTCOMMAND=y
CONFIG_BOOTCOMMAND="echo LOOM_UBOOT_PROPER_OK; poweroff"
EOF
uboot_make_arm64 "$O_QEMU" olddefconfig
uboot_make_arm64 "$O_QEMU" u-boot.bin spl/u-boot-spl.bin

# Stage 7a: Boot woven U-Boot proper directly in QEMU virt and assert clean banner + PSCI poweroff.
PROPER_LOG="$WORK/qemu-proper.log"
timeout 60 qemu-system-aarch64 \
  -machine virt -cpu cortex-a57 -m 1024M \
  -display none -serial "file:$PROPER_LOG" -no-reboot \
  -bios "$O_QEMU/u-boot.bin"
grep -q "U-Boot 2026.10" "$PROPER_LOG"
grep -q "LOOM_UBOOT_PROPER_OK" "$PROPER_LOG"

# Stage 7b: Boot woven SPL from -bios and verify it loads a FIT payload from byte 0
# of the emulated PCI SDHCI eMMC boot partition (MMCSD_MODE_EMMCBOOT).
MARKER="LOOM_EMMC_BOOT_PARTITION_OK"
cat > "$WORK/stage2.S" << EOF
.section .text
.global _start
_start:
    ldr x5, =0x09000000
    adr x4, msg
1:  ldrb w1, [x4], #1
    cbz w1, 2f
3:  ldr w2, [x5, #0x18]
    tbnz w2, #5, 3b
    strb w1, [x5]
    b 1b
2:  ldr x0, =0x84000008 /* PSCI_0_2_FN_SYSTEM_OFF */
    hvc #0
    wfi
    b 2b
msg: .asciz "${MARKER}\n"
EOF
aarch64-unknown-linux-gnu-gcc -nostdlib -ffreestanding -Wl,-Ttext=0x50000000 \
  -o "$WORK/stage2.elf" "$WORK/stage2.S"
aarch64-unknown-linux-gnu-objcopy -O binary --only-section=.text "$WORK/stage2.elf" "$WORK/stage2.bin"

cat > "$WORK/payload.its" << 'EOF'
/dts-v1/;
/ {
    description = "loom qemu-spl eMMC boot0 verification payload";
    #address-cells = <1>;
    images {
        firmware {
            description = "stage2 marker";
            data = /incbin/("stage2.bin");
            type = "firmware";
            arch = "arm64";
            os = "standalone";
            load = <0x50000000>;
            entry = <0x50000000>;
            compression = "none";
        };
    };
    configurations {
        default = "conf";
        conf {
            description = "boot stage2";
            firmware = "firmware";
        };
    };
};
EOF
(cd "$WORK" && "$O_QEMU/tools/mkimage" -f payload.its payload.itb >/dev/null)
truncate -s 4G "$WORK/emmc.img"
dd if="$WORK/payload.itb" of="$WORK/emmc.img" conv=notrunc status=none

BOOT_LOG="${BOOT_LOG_OUT:-$WORK/qemu-spl-boot.log}"
timeout 60 qemu-system-aarch64 \
  -machine virt -cpu cortex-a57 -m 1024M \
  -display none -serial "file:$BOOT_LOG" -no-reboot \
  -bios "$O_QEMU/spl/u-boot-spl.bin" \
  -drive "file=$WORK/emmc.img,if=none,format=raw,id=emmc0" \
  -device sdhci-pci \
  -device emmc,drive=emmc0,boot-partition-size=131072 || true

cat "$BOOT_LOG"
grep -q "$MARKER" "$BOOT_LOG"
echo "ALL U-BOOT BUILD, SIGNING, AND QEMU BOOT CHECKS PASSED"
