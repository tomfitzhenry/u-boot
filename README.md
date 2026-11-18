# `tomfitzhenry/u-boot` — `git-loom` U-Boot Integration Tree

This repository maintains all custom U-Boot patches across lab and fleet boards
on top of mainline U-Boot (`https://github.com/u-boot/u-boot`) as independent,
upstreamable feature branches woven into a single deployable integration branch
(`lab/integration`) using [`git-loom`](https://github.com/narnaud/git-loom).

---

## 1. Active Feature Branches (Base: `v2026.10`)

All woven branches are declared in [`loom/loom.toml`](loom/loom.toml):

| Board / SoC | Branch | Stacked On | Upstreamable | Commits | Summary |
| :--- | :--- | :--- | :---: | :---: | :--- |
| **Generic HDMI / Binman** | `drv/video/dw-hdmi-avi-infoframe` | `v2026.10` | Yes | 1 | Send CEA-861 AVI infoframe when driving an HDMI sink to prevent overscan |
| | `doc/binman/tee-os-elf` | `v2026.10` | Yes | 1 | Document that OP-TEE 3.8+ requires flat `tee.bin` rather than `tee.elf` |
| **Rockchip RK3399** (RockPro64, Pinebook Pro, PinePhone Pro) | `drv/video/rk3399-vop-max-output` | `v2026.10` | Yes | 1 | Validate EDID modes against `vop-lit` (2560x1600) and `vop-big` (4096x2160) limits |
| | `soc/rk3399/optee-reserved-memory` | `v2026.10` | Yes | 1 | Declare OP-TEE TZDRAM/SHMEM `/reserved-memory` and `/firmware/optee` at OS boot |
| | `local-rk3399-sd-first-boot` | `soc/rk3399/optee-reserved-memory` | No | 1 | Prefer SD recovery FIT over eMMC boot partition on split-boot RK3399 boards |
| | `soc/rk3399/signed-idblock` | `v2026.10` | Yes | 2 | Add RK3399 RSA-2048 secure-boot idblock signing to `mkimage` + self-test |
| | `soc/rockchip/spl-emmc-boot-partition` | `v2026.10` | Yes | 1 | Return `MMCSD_MODE_EMMCBOOT` when SPI-booted SPL reads an eMMC payload |
| **Pine64 QuartzPro64** (Rockchip RK3588, `arm64`) | `dts/rk3588-quartzpro64-usb-otg` | `v2026.10` | Yes | 1 | Enable USB2.0 OTG0 DOWNLOAD port in peripheral mode for fastboot/UMS |
| **Pine64 Quartz64 Zero** (Rockchip RK3566, `arm64`) | `drv/power/rk817-quartz64-zero-input-limit` | `v2026.10` | Yes | 1 | Raise RK817 charger input current limit on Quartz64 Zero before USB VBUS enable |
| | `dts/rk3566-quartz64-zero` | `drv/power/rk817-quartz64-zero-input-limit` | Yes | 1 | Quartz64 Zero DTS, `-u-boot.dtsi`, and `quartz64-zero-rk3566_defconfig` |
| **Anbernic RG40XX-H / RG35XX** (Allwinner H700, `arm64`) | `soc/sunxi/console-usbacm` | `v2026.10` | Yes | 1 | Include `usbacm` CDC-ACM gadget in default sunxi console settings when enabled |
| | `fix/sunxi/musb-udc-probe` | `v2026.10` | Yes | 1 | Probe legacy MUSB UDC device from `board_usb_init()` when `!DM_USB_GADGET` |
| | `board/anbernic/rg35xx-h700-lpddr3` | `fix/sunxi/musb-udc-probe` | Yes | 1 | Add `anbernic_rg35xx_h700_lpddr3_defconfig` with LPDDR3 DRAM parameters |
| **Pine64 Oz64** (Sophgo SG2000 / CV1800B, `arm64`) | `fix/mmc/cv1800b-sdhci` | `v2026.10` | Yes | 2 | Honour DT `max-frequency`/`bus-width` + program CV1800B vendor PHY registers |
| | `drv/phy/cv1800b-usb2` | `v2026.10` | Yes | 1 | Add `cvitek,cv1800-usb-phy` USB2 PHY driver and `cvitek,cv1810-usb` DWC2 match |
| | `board/sophgo/oz64` | `drv/phy/cv1800b-usb2` | Yes | 4 | Oz64 ARM64 port (`v2026.10`), USB-A host, microSD 3.3V power, eMMC/SD0 reset release |
| **NETGEAR ReadyNAS 102** (Marvell Armada 370, `arm`) | `soc/mvebu/armada-370` | `v2026.10` | Yes | 3 | `ARMADA_370` Kconfig, 16-bit DDR3 bus width, static training, `config.h` guard |
| | `board/netgear/rn102` | `soc/mvebu/armada-370` | Yes | 2 | ReadyNAS RN102 board support, static 600 MHz DDR3 MC init, and `rn102_defconfig` |
| **QEMU `virt` SPL Harness** (`aarch64`) | `local-qemu-arm64-spl` | `v2026.10` | No | 1 | Synthetic `qemu_arm64_spl_defconfig` booting SPL from `-bios` and loading FIT from eMMC `boot0` |
| **CI & `git-loom` Manifest** | `local-ci` | `v2026.10` | No | 1 | Declarative `loom/loom.toml` manifest, Nix verification flake, and GitHub Actions workflows |

---

## 2. Branch Taxonomy & Hygiene Rules

- **Upstreamable topic branches (`upstream = true` in [`loom/loom.toml`](loom/loom.toml)):**
  - `dts/<board>`: Device tree additions and `-u-boot.dtsi` overrides
  - `soc/<vendor>/<topic>`: SoC SPL, Kconfig, environment, and platform support
  - `drv/<subsystem>/<topic>`: Subsystem drivers (video, PHY, PMIC, DDR)
  - `fix/<subsystem>/<topic>`: Targeted driver/platform bugfixes
  - `board/<vendor>/<board>`: Full board bring-up directories and defconfigs
  - `doc/<subsystem>/<topic>`: Upstream documentation improvements
  - Every commit on an `upstream = true` branch must be strictly linear, carry a `Signed-off-by:` trailer, and **never** touch CI/Nix/loom/agent files (`README.md`, `flake.nix`, `loom/`, `.github/`, `CLAUDE.md`, `PLAN.md`, `DESIGN.md`).
- **Non-upstream local branches (`upstream = false` in [`loom/loom.toml`](loom/loom.toml)):**
  - `local-<topic>`: Lab-specific policy overrides and synthetic QEMU test targets (`local-rk3399-sd-first-boot`, `local-qemu-arm64-spl`)
  - `local-ci`: Owns `README.md`, [`loom/`](loom/), [`flake.nix`](flake.nix), and `.github/workflows/`
- **Woven integration branch (generated by `git-loom`):**
  - `lab/integration`: First-parent `--no-ff` merge weave of all leaf branches declared in [`loom/loom.toml`](loom/loom.toml) on top of `base`.

---

## 3. Day-to-Day Workflows

Enter the development shell (`path:./loom` evaluates in `<0.1s` without copying the U-Boot tree into `/nix/store`):

```bash
nix develop path:./loom
```

### A. Consuming the Single U-Boot Tree in `row-boot` & Downstream Repos
`row-boot` consumes `lab/integration` directly as its single U-Boot `src` (with `extraPatches = []`), composing board/SoC/role Kconfig scopes, ATF/OP-TEE/FIP packaging, and NixOS VM tests on top of the woven git tree instead of carrying flat `.patch` files.

### B. Hacking on an Existing Feature Branch

```bash
git checkout board/sophgo/oz64
# ... edit and commit with `git commit -s` ...

./loom/weave.sh apply
./loom/verify-uboot.sh
git push --force-with-lease origin board/sophgo/oz64
```

### C. Sending a Feature Branch Upstream with `b4` / `patman`

```bash
git checkout drv/video/dw-hdmi-avi-infoframe
b4 prep --enroll-base v2026.10
b4 send
```

### D. Rebasing Onto a Newer U-Boot Release (`u-boot/u-boot`)

```bash
./loom/weave.sh rebase v2027.01-rc1
```

`loom/weave.sh rebase` checks `git cherry` against the new base, automatically drops any branch whose patches have all landed upstream (re-parenting stacked child branches), rebases remaining branches in topological order, updates `loom/loom.toml` on `local-ci`, and re-weaves `lab/integration`.

### E. Running the Verification Suite Locally

- **Fast `git-loom` unit & integration tests (~15s):**
  ```bash
  nix flake check path:./loom -L
  ```
- **Full U-Boot multi-board build, signing, and QEMU SPL boot verification (~3 min):**
  ```bash
  nix develop path:./loom --command ./loom/verify-uboot.sh
  ```
  1. Cross-compiles `oz64_arm64_defconfig`, `anbernic_rg35xx_h700_lpddr3_defconfig` (with `usbacm` MUSB gadget), `rockpro64-rk3399_defconfig` (with `OPTEE` + `SUPPORT_EMMC_BOOT`), `quartzpro64-rk3588_defconfig`, `quartz64-zero-rk3566_defconfig`, and `rn102_defconfig` (`arm32`), verifying compiled `u-boot.dtb` properties with `fdtget`.
  2. Exercises `mkimage --rk-key` RSA-2048 RK3399 idblock signing.
  3. Builds `qemu_arm64_spl_defconfig` and boots both U-Boot proper (`U-Boot 2026.10` + PSCI poweroff) and SPL loading a FIT payload from byte 0 of an emulated PCI SDHCI eMMC boot partition (`MMCSD_MODE_EMMCBOOT`) under `qemu-system-aarch64 -machine virt`.
