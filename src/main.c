/*
 * W25QXX SPI bring-up / persistence test
 * See docs/W25QXX_XIAO_SPI_HELLO_WORLD_TEST.md for the full spec.
 */
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define TEST_OFFSET      0x001000 /* dedicated sector; 4 KB aligned */
#define TEST_SECTOR_SIZE 0x001000

/*
 * Raw SPI handle to the flash node (bus + CS from cs-gpios). Independent of
 * the jedec,spi-nor driver instance, so it works even when driver init
 * failed - used to diagnose wiring.
 */
static const struct spi_dt_spec w25qxx_spi =
	SPI_DT_SPEC_GET(DT_ALIAS(spi_flash0),
			SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
			0);

static int spi_rd(const uint8_t *cmd, size_t cmd_len, uint8_t *resp, size_t resp_len)
{
	const struct spi_buf tx_buf = {.buf = cmd, .len = cmd_len};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	const struct spi_buf rx_buf = {.buf = resp, .len = resp_len};
	const struct spi_buf_set rx = {.buffers = &rx_buf, .count = 1};

	return spi_transceive_dt(&w25qxx_spi, &tx, &rx);
}

static void jedec_diag(void)
{
	static const uint8_t rdid[4] = {0x9F, 0, 0, 0}; /* cmd + 3 dummy clocks */
	uint8_t a[4] = {0}, b[4] = {0}, c[4] = {0};

	if (!device_is_ready(w25qxx_spi.bus)) {
		printk("DIAG: SPI bus not ready\n");
		return;
	}

	if (spi_rd(rdid, 4, a, 4) != 0 || spi_rd(rdid, 4, b, 4) != 0 ||
	    spi_rd(rdid, 4, c, 4) != 0) {
		printk("DIAG: JEDEC (0x9F) transfer failed\n");
		return;
	}

	/* Full duplex: resp[0] is garbage (clocked out while the command is
	 * sent), the 3-byte JEDEC ID is resp[1..3].
	 */
	const uint8_t *ia = &a[1];
	const uint8_t *ib = &b[1];
	const uint8_t *ic = &c[1];

	printk("DIAG: JEDEC ID (3x): %02X %02X %02X | %02X %02X %02X | %02X %02X %02X\n",
	       ia[0], ia[1], ia[2], ib[0], ib[1], ib[2], ic[0], ic[1], ic[2]);

	if (memcmp(ia, ib, 3) != 0 || memcmp(ib, ic, 3) != 0) {
		printk("DIAG: Hasil berubah-ubah tiap baca -> kabel longgar, CS tidak\n");
		printk("DIAG: stabil, atau VCC/GND lemah. Tekan semua jumper, cek 3V3.\n");
		return;
	}

	if ((ia[0] == 0xFF && ia[1] == 0xFF && ia[2] == 0xFF) ||
	    (ia[0] == 0x00 && ia[1] == 0x00 && ia[2] == 0x00)) {
		printk("DIAG: W25QXX tidak merespons. Cek hardware:\n");
		printk("DIAG: VCC ke 3V3 (bukan 5V), GND sama, CS->D7, SCK->D8,\n");
		printk("DIAG: DO->D9 (MISO), DI->D10 (MOSI), jumper pendek\n");
		return;
	}

	if (ia[0] == 0xEF || ia[0] == 0x1C) {
		printk("DIAG: Manufacturer: %s (0x%02X)\n",
		       ia[0] == 0xEF ? "Winbond" : "eON", ia[0]);
		switch (ia[2]) {
		case 0x15:
			printk("DIAG: 2 MiB chip - overlay: jedec-id=[%02x %02x 15], size=<0x1000000>\n",
			       ia[0], ia[1]);
			break;
		case 0x16:
			printk("DIAG: 4 MiB chip - overlay: jedec-id=[%02x %02x 16], size=<0x2000000>\n",
			       ia[0], ia[1]);
			break;
		case 0x17:
			printk("DIAG: 8 MiB chip - sesuai overlay (size=<0x4000000>)\n");
			break;
		case 0x18:
			printk("DIAG: 16 MiB chip - overlay: jedec-id=[%02x %02x 18], size=<0x8000000>\n",
			       ia[0], ia[1]);
			break;
		default:
			printk("DIAG: Density byte 0x%02X tidak dikenal\n", ia[2]);
			break;
		}
		return;
	}

	printk("DIAG: Manufacturer 0x%02X tidak dikenal (ID: %02X %02X %02X)\n",
	       ia[0], ia[0], ia[1], ia[2]);
}

static const char expected[] = "Hello World";
static char readback[sizeof(expected)];

static bool is_blank(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != 0xFF) {
			return false;
		}
	}
	return true;
}

int main(void)
{
	const struct device *flash = DEVICE_DT_GET(DT_ALIAS(spi_flash0));
	int err;

	jedec_diag();

	if (!device_is_ready(flash)) {
		printk("ERROR: W25QXX device is not ready\n");
		return 0;
	}

	err = flash_read(flash, TEST_OFFSET, readback, sizeof(readback));
	if (err) {
		printk("ERROR: flash_read failed: %d\n", err);
		return 0;
	}

	if (memcmp(readback, expected, sizeof(expected)) == 0) {
		printk("Hello World sudah tersimpan\n");
		return 0;
	}

	if (is_blank((const uint8_t *)readback, sizeof(readback))) {
		printk("Hello World belum tersimpan; memasukkan Hello World\n");
	} else {
		printk("Test sector contains different data; replacing only test sector\n");
	}

	err = flash_erase(flash, TEST_OFFSET, TEST_SECTOR_SIZE);
	if (err) {
		printk("ERROR: sector erase failed: %d\n", err);
		return 0;
	}

	err = flash_write(flash, TEST_OFFSET, expected, sizeof(expected));
	if (err) {
		printk("ERROR: write failed: %d\n", err);
		return 0;
	}

	memset(readback, 0, sizeof(readback));
	err = flash_read(flash, TEST_OFFSET, readback, sizeof(readback));
	if (err || memcmp(readback, expected, sizeof(expected)) != 0) {
		printk("ERROR: readback verification failed: %d\n", err);
		return 0;
	}

	printk("PASS: Hello World berhasil ditulis dan diverifikasi\n");
	printk("Reset/power-cycle board: boot berikutnya harus mengatakan tersimpan\n");
	return 0;
}
