# AGENTS.md

## What this repo is
- Zephyr (Nordic NCS) app: bring-up test for an external W25QXX SPI NOR flash on a
  Seeed Studio XIAO nRF52840 Sense over plain SPI (not QSPI).
- `docs/W25QXX_XIAO_SPI_HELLO_WORLD_TEST.md` is the authoritative spec (wiring,
  test logic, expected logs). Its sample overlay has known conflicts with the
  board DTS — see "Overlay gotchas" below before copying it blindly.
- No CI, no test suite, no linter. Verification = pristine `west build` + expected
  boot log lines on hardware.

## Persistence test = acceptance criteria are log lines
- On boot, read the reserved test sector at offset `0x1000`:
  - `Hello World` present -> print `Hello World sudah tersimpan` and do NOT rewrite.
  - Blank or different -> print `Hello World belum tersimpan; memasukkan Hello World`,
    erase exactly one 4 KB sector, page-program, read back, verify -> `PASS: ...`.
- After reset/power-cycle the next boot MUST print `sudah tersimpan` without
  rewriting. That proves non-volatile storage; SPI activity alone is not success.
- Keep the exact log strings (Indonesian); they are the pass/fail signal.
- Test sector is `0x001000`-`0x001FFF` only. Never erase offset `0x0`; never overlap
  future voice-asset / MCUboot partitions (doc section 7).

## Hardware facts (do not guess)
- Wiring: CS->D7/P1.12, SCK->D8/P1.13, DO(MISO)->D9/P1.14, DI(MOSI)->D10/P1.15.
  VCC = 3.3 V only, never 5 V.
- Standard SPI on `&spi3` with `cs-gpios = <&gpio1 12 GPIO_ACTIVE_LOW>`. The 6-pin
  module lacks IO2/WP# and IO3/HOLD#, so nRF QSPI is impossible.
- Flash node: `compatible = "jedec,spi-nor"`, alias `spi-flash0`. `size` unit is
  BITS (8 MiB -> `0x4000000`).
- The physical chip answers JEDEC `1C 30 17` = **eON EN25Q64 (8 MiB)**, NOT
  Winbond, despite the module's W25QXX silkscreen. It does NOT implement SFDP:
  `CONFIG_SPI_NOR_SFDP_RUNTIME=y` fails init with `SFDP magic ffffffff invalid`.
  Use `CONFIG_SPI_NOR_SFDP_MINIMAL=y` + `jedec-id = [1c 30 17]` in the overlay.
- Confirm capacity via JEDEC ID (cmd `0x9F`): `EF 40 17` = W25Q64 (8 MiB),
  `EF 40 18` = W25Q128 (16 MiB -> also change `size`). The driver validates
  `jedec-id` at boot and reports a mismatch, so a wrong guess shows in the log.

## Overlay gotchas (differs from the doc's sample)
- The board DTS already defines pinctrl nodes `spi3_default`/`spi3_sleep` for
  other P0.x pins (its own `spi3` and QSPI). Our overlay must use its own names
  (`w25qxx_spi3_default`/`_sleep`).
- The board enables `&uart0` with RX = P1.12 (D7) and `&spi2` on P1.13/14/15
  (D8-D10) — same pins as our flash. The overlay disables both. Logs go over
  USB CDC ACM anyway (`cdc_acm_serial.dtsi` is included by the board).

## Console
- `printk()` output is USB CDC ACM serial (115200, appears as a virtual COM port).
- NCS/Zephyr 4.4 use the NEW USB stack (`CONFIG_USB_DEVICE_STACK_NEXT=y` comes on
  automatically via the NCS fragment). Do NOT add the legacy
  `CONFIG_USB_DEVICE_STACK=y` / `CONFIG_USB_CDC_ACM=y` — both CDC ACM drivers
  then link and fail with `multiple definition of __device_dts_ord_*`.
- `CONFIG_UART_CONSOLE=y` binds the console to the CDC node chosen by the board
  DTS; USB stack config otherwise needs nothing in `prj.conf`.
- `CONFIG_LOG=y` + `CONFIG_LOG_MODE_DEFERRED=y` are required: `jedec,spi-nor`
  reports its init failure (SFDP read failed, JEDEC ID mismatch) through the LOG
  subsystem, not printk. Deferred mode buffers early lines until the USB CDC
  client attaches.

## Failure diagnostics (added to main.c)
- `device_is_ready(flash) == false` on a `jedec,spi-nor` node means DRIVER INIT
  failed, always before any flash API call. With SFDP_RUNTIME the `jedec-id`
  property is NOT validated at init — a wrong capacity guess does not cause
  this error; a dead bus does.
- `main.c` `jedec_diag()` reads JEDEC ID (0x9F) over raw SPI (independent of the
  failed driver). It sends cmd + 3 dummy bytes and uses `resp[1..3]`: `resp[0]`
  is full-duplex garbage, so a differing `resp[0]` is NOT instability. Real
  JEDEC instability = differing bytes 2..4 across reads.
- `spi-max-frequency` is 1 MHz for bring-up; raise to 8 MHz after wiring is proven.

## Build / flash / observe (verified on this machine)
- `west` is not on the default PATH. Export the NCS toolchain env first. Verified
  working pair: toolchain `~/ncs/toolchains/fbf7391cab` (Zephyr SDK 1.0.1,
  GCC 14.3.0) + workspace `~/ncs/v3.4.0` (west 1.5.0, Zephyr 4.4). NOTE: the
  older toolchain `911f4c5c26` (SDK 0.17.0) pairs with `~/ncs/v3.3.0` and FAILS
  against v3.4.0 (FindZephyr-sdk requires SDK >= 1.0):
  ```bash
  TC=~/ncs/toolchains/fbf7391cab
  export PATH="$TC/bin:$TC/usr/bin:$TC/usr/local/bin:$TC/opt/bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:$PATH"
  export PYTHONHOME="$TC/usr/local"
  export PYTHONPATH="$TC/usr/local/lib/python3.12:$TC/usr/local/lib/python3.12/site-packages"
  export LD_LIBRARY_PATH="$TC/lib:$TC/lib/x86_64-linux-gnu:$TC/usr/local/lib"
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr ZEPHYR_SDK_INSTALL_DIR="$TC/opt/zephyr-sdk"
  ```
- This repo has no `.west`, so run west from inside the workspace:
  ```bash
  cd ~/ncs/v3.4.0 && west build -b xiao_ble/nrf52840/sense -p always \
    -d /home/ibrohim/Documents/github/nrf52_w25qxx/build \
    /home/ibrohim/Documents/github/nrf52_w25qxx
  ```
  `-p always` matters: overlay/devicetree changes need pristine builds.
- The board config builds `build/zephyr/zephyr.uf2` automatically (Adafruit
  nRF52 bootloader): double-tap RESET, then copy the UF2 onto the `XIAO BLE`
  drive. Disk space is tight on this machine (~2 GB free) — keep one build dir.
- Alternative flash paths: `west flash --runner uf2` (same thing), OpenOCD via
  ST-Link SWD (`program zephyr.hex verify reset exit`), `nrfjprog`, or `pyocd`
  — all installed.
