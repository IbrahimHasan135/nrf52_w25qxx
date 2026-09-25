# W25QXX `device is not ready` Analysis

## Error

Serial log:

```text
*** Booting nRF Connect SDK v3.4.0-99553055607b ***
*** Using Zephyr OS v4.4.0-bf801e4e3d19 ***
ERROR: W25QXX device is not ready
```

## Short Conclusion

This error means the firmware is running, but Zephyr failed to initialize the external W25QXX flash device.

The code reaches this line:

```c
const struct device *flash = DEVICE_DT_GET(DT_ALIAS(spi_flash0));

if (!device_is_ready(flash)) {
	printk("ERROR: W25QXX device is not ready\n");
	return 0;
}
```

So the problem is **before** `flash_read()`, `flash_erase()`, or `flash_write()`.

Most likely causes:

1. SPI wiring is wrong or one signal is not connected.
2. `jedec-id` in `app.overlay` does not match the real W25QXX chip.
3. The overlay is not actually applied because the project was not rebuilt cleanly.
4. The SPI clock is too high for jumper wires / module board.
5. Wrong XIAO board target or wrong pin mapping.

## Current Code State

### `main.c`

The test app is correct structurally.

It does:

1. Get external flash device from devicetree alias `spi-flash0`.
2. Check `device_is_ready(flash)`.
3. Read offset `0x1000`.
4. If `Hello World` already exists, print stored message.
5. If not, erase one 4 KB sector and write `Hello World`.

Because the error happens at `device_is_ready()`, the storage test never starts.

### `app.overlay`

Current flash node:

```dts
&spi3 {
	status = "okay";
	pinctrl-0 = <&w25qxx_spi3_default>;
	pinctrl-1 = <&w25qxx_spi3_sleep>;
	pinctrl-names = "default", "sleep";
	cs-gpios = <&gpio1 12 GPIO_ACTIVE_LOW>;

	w25qxx: flash@0 {
		compatible = "jedec,spi-nor";
		reg = <0>;
		spi-max-frequency = <8000000>;
		jedec-id = [ef 40 17];
		size = <0x4000000>;
	};
};
```

This means the firmware expects:

| Signal | W25QXX Pin | XIAO Pin | nRF52840 Pin |
| --- | --- | --- | --- |
| CS | CS | D7 | P1.12 |
| SCK | SLK / CLK | D8 | P1.13 |
| MISO | DO | D9 | P1.14 |
| MOSI | DI | D10 | P1.15 |
| VCC | VCC | 3V3 | 3.3 V only |
| GND | GND | GND | Common ground |

Important: the 6-pin W25QXX module is **plain SPI**, not QSPI, because it only exposes `CS`, `CLK`, `DO`, and `DI`. It does not expose IO2/WP# and IO3/HOLD#.

## Why This Error Happens

For `compatible = "jedec,spi-nor"`, Zephyr initializes the chip at boot.

During init, the driver communicates over SPI and reads the chip identity. If the driver cannot communicate or the returned ID does not match the devicetree configuration, the device init fails. After that, `device_is_ready(flash)` returns false.

That is why this log appears:

```text
ERROR: W25QXX device is not ready
```

## Priority Debug Checklist

### 1. Confirm Overlay Is Applied

Build must be pristine after changing `app.overlay`.

Use:

```bash
cd ~/ncs/v3.4.0

west build -b xiao_ble/nrf52840/sense -p always \
  -d /home/ibrohim/Documents/github/nrf52_w25qxx/build \
  /home/ibrohim/Documents/github/nrf52_w25qxx
```

After build, check generated devicetree:

```bash
grep -n "spi-flash0\\|w25qxx\\|spi3\\|gpio1 12" \
  /home/ibrohim/Documents/github/nrf52_w25qxx/build/zephyr/zephyr.dts
```

Expected:

```dts
spi-flash0 = &w25qxx;
```

and the flash node must be under `spi3`.

If `w25qxx` does not appear in `zephyr.dts`, the overlay is not being used.

### 2. Confirm Wiring

Use this wiring exactly:

| W25QXX Module | XIAO nRF52840 Sense |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| CS | D7 |
| SLK / CLK | D8 / SCK |
| DO | D9 / MISO |
| DI | D10 / MOSI |

Common mistake:

| Wrong assumption | Correct meaning |
| --- | --- |
| `DO` goes to MOSI | Wrong. `DO` is Data Out from flash, so it goes to MCU MISO |
| `DI` goes to MISO | Wrong. `DI` is Data In to flash, so it goes to MCU MOSI |
| VCC can use 5 V | Wrong. Use 3.3 V only |
| SWD pins are used for W25QXX | Wrong. W25QXX uses SPI pins, not SWD |

### 3. Lower SPI Clock First

Current overlay uses:

```dts
spi-max-frequency = <8000000>;
```

For breadboard / jumper wire testing, lower it first:

```dts
spi-max-frequency = <1000000>;
```

Then rebuild with `-p always` and flash again.

This is a safe first debug step because long jumper wires and cheap W25QXX modules can fail at higher clock even when wiring is conceptually correct.

### 4. Confirm JEDEC ID / Capacity

Current overlay assumes:

```dts
jedec-id = [ef 40 17];
size = <0x4000000>; /* 64 Mbit = 8 MiB */
```

This is for W25Q64.

Common Winbond IDs:

| Chip | JEDEC ID | Capacity | Devicetree `size` |
| --- | --- | --- | --- |
| W25Q16 | `ef 40 15` | 2 MiB | `<0x1000000>` |
| W25Q32 | `ef 40 16` | 4 MiB | `<0x2000000>` |
| W25Q64 | `ef 40 17` | 8 MiB | `<0x4000000>` |
| W25Q128 | `ef 40 18` | 16 MiB | `<0x8000000>` |

Important: Zephyr devicetree `size` is in **bits**, not bytes.

If the real module is not W25Q64, this line is wrong:

```dts
jedec-id = [ef 40 17];
```

If the chip is W25Q128, change to:

```dts
jedec-id = [ef 40 18];
size = <0x8000000>;
```

If the chip marking says W25Q64, keep:

```dts
jedec-id = [ef 40 17];
size = <0x4000000>;
```

### 5. Temporarily Remove `jedec-id` For Bring-up

If the chip marking is unclear, temporarily remove this line:

```dts
jedec-id = [ef 40 17];
```

Keep:

```dts
compatible = "jedec,spi-nor";
size = <0x4000000>;
```

Then rebuild and flash.

This can help separate:

| Result | Meaning |
| --- | --- |
| Device becomes ready | Wiring/SPI is probably OK, previous `jedec-id` was wrong |
| Device still not ready | Likely wiring, power, SPI pin, or bus problem |

After the real JEDEC ID is confirmed, put the correct `jedec-id` back.

### 6. Check Pin Conflict

The overlay already disables likely conflicts:

```dts
&uart0 {
	status = "disabled";
};

&spi2 {
	status = "disabled";
};
```

This is good because:

| Peripheral | Conflict |
| --- | --- |
| `uart0` | RX uses P1.12 / D7, same as flash CS |
| `spi2` | Uses D8-D10, same as flash SCK/MISO/MOSI |

If these are not disabled in the generated `zephyr.dts`, the overlay was not applied correctly.

## Recommended First Fix Attempt

Change only this first:

```dts
spi-max-frequency = <1000000>;
```

Then:

```bash
cd ~/ncs/v3.4.0

west build -b xiao_ble/nrf52840/sense -p always \
  -d /home/ibrohim/Documents/github/nrf52_w25qxx/build \
  /home/ibrohim/Documents/github/nrf52_w25qxx
```

Flash:

```bash
cd /home/ibrohim/Documents/github/nrf52_w25qxx/build/zephyr

openocd \
  -f interface/stlink.cfg \
  -c "transport select swd" \
  -f target/nordic/nrf52.cfg \
  -c "adapter speed 100; program zephyr.hex verify reset exit"
```

Expected first successful boot:

```text
Hello World belum tersimpan; memasukkan Hello World
PASS: Hello World berhasil ditulis dan diverifikasi
Reset/power-cycle board: boot berikutnya harus mengatakan tersimpan
```

Expected second boot:

```text
Hello World sudah tersimpan
```

## If It Still Fails

Do this order:

1. Re-check VCC with multimeter: must be around 3.3 V.
2. Re-check GND continuity between XIAO and W25QXX module.
3. Re-check `DO -> D9/MISO` and `DI -> D10/MOSI`.
4. Re-check CS continuity from module CS to XIAO D7.
5. Lower SPI frequency to `500000` or `100000`.
6. Remove `jedec-id` temporarily.
7. Confirm the generated `zephyr.dts` has the correct flash node.

## Most Likely Root Cause For This Case

Because the firmware already boots and prints over serial, the XIAO itself is alive.

Because the error is exactly:

```text
ERROR: W25QXX device is not ready
```

the highest-probability causes are:

1. The real flash chip is not `EF 40 17` / W25Q64.
2. `DO` and `DI` are swapped.
3. CS is not actually on D7 / P1.12.
4. SPI clock `8 MHz` is too high for the current wiring.
5. The build did not use the updated overlay.

## Note For Future Voice Flashing Test

Do not start voice/audio upload testing until this `Hello World` persistence test passes.

Voice flashing depends on the same lower-level requirement:

```text
MCU can reliably read/write external W25QXX over SPI
```

If `Hello World` cannot survive reset/power-cycle, the voice asset loader will also fail or corrupt data.

