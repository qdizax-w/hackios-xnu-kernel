/*
 * The touch screen of miatoll's Huaxing panel (Redmi Note 9S / 9 Pro, SM7125): a FocalTech FT8756 in the
 * panel itself (touch and display driver in one chip), as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg ft8756=1.
 *
 * What the chip needs, from Xiaomi's driver for this phone (MiCode Xiaomi_Kernel_OpenSource, branch
 * curtana-q-oss, drivers/input/touchscreen/ft8756_spi) and its device tree:
 *   power    none of ours: the chip is the panel's, powered with the display, which the UEFI has lit
 *   pins     SPI on TLMM GPIO 53 (MISO), 54 (MOSI), 55 (CLK), 56 (CS) -- the west tile -- driven by hand
 *            (mode 0, a few hundred kHz against the 8 MHz its device tree allows); the interrupt line on
 *            GPIO 9 (north tile), low while a report waits. Reset (GPIO 8) is never touched: the panel
 *            would go dark with it
 *   firmware the FT8756 keeps none: after power-up it waits in its boot ROM (chip ID reads ef ef, boot ID
 *            87 56, register 0xd0 = 1) for the host to load it -- Xiaomi's fts_fw_recovery(): code into
 *            PRAM and constants into DRAM, each checked with the ROM's ECC, then started (0x08), with no
 *            reset. The image is Xiaomi's fw_huaxing_v0e (tools/mk-fw-touch-miatoll.sh), fw/ft8756.bin
 *            in the small image
 *   frames   FTS SPI framing (focaltech_spi.c): address, 0x00 to write or 0xa0 to read with CRC, length,
 *            three dummy bytes, data; a read ends in a CRC-16/Kermit of the data; rx[3] bits 7 and 5 say
 *            busy or bad CRC
 *   reports  register 0x01: 0x9N, the number of points, then 6 bytes a point (event in bits 7..6 and x
 *            bits 11..8, x 7..0, id in bits 7..4 and y bits 11..8, y 7..0, pressure, area), in pixels
 *            of the 1080 x 2400 screen; "ef ef ef" there means the chip has lost its firmware
 * Nothing here takes an interrupt: a thread looks at the interrupt line every few milliseconds, which it
 * only reads; nothing else of the chip's or the panel's is written but the four SPI pins' configuration.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM, SC7180's layout: 0x1000 a pin, configuration at +0, in/out at +4) ---------------- */

#define TLMM_WEST_PHYS          0x03500000UL
#define TLMM_NORTH_PHYS         0x03900000UL
#define PIN_MISO                53      /* west */
#define PIN_MOSI                54
#define PIN_CLK                 55
#define PIN_CS                  56
#define PIN_INT                 9       /* north; low = the chip has a report */

static vm_offset_t spi_pins, int_pin;   /* GPIO 53's registers (54-56 follow), GPIO 9's */

#define PIN_REGS(pin)           (spi_pins + 0x1000UL * ((pin) - PIN_MISO))

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* function 0 (a GPIO), no pull, drive strength in mA, direction */
static void
pin_configure(vm_offset_t regs, unsigned ma, bool output)
{
	wr(regs, (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0));
}

static void
pin_set(unsigned pin, bool high)
{
	wr(PIN_REGS(pin) + 4, high ? 2 : 0);
}

static bool
pin_get(unsigned pin)
{
	return rd(PIN_REGS(pin) + 4) & 1;
}

/*
 * The chip says it has a report by pulling its line low, briefly: a look at the line every few
 * milliseconds misses most of those, and with them the reports that a finger went down or came
 * up. So the pin controller is to catch the falling edge itself and hold on to it in the pin's
 * interrupt status (TLMM, pinctrl-msm.c: msm_gpio_irq_set_type; raw status kept, the interrupt
 * itself left off: nothing reaches a processor), which is looked at and cleared here.
 */
#define TLMM_INTR_CFG           0x8
#define TLMM_INTR_STATUS        0xc
#define TLMM_INTR_FALLING       (1u << 1 | 2u << 2 | 1u << 4 | 3u << 5) /* polarity, edge, raw status, to the apps processors */

static void
interrupt_catch(void)
{
	wr(int_pin + TLMM_INTR_CFG, TLMM_INTR_FALLING);
	wr(int_pin + TLMM_INTR_STATUS, 0);
}

static bool
interrupt_pending(void)
{
	bool caught = rd(int_pin + TLMM_INTR_STATUS) & 1;

	if (caught) {
		wr(int_pin + TLMM_INTR_STATUS, 0);
	}
	return caught || !(rd(int_pin + 4) & 1);
}

/* ---- SPI by hand -------------------------------------------------------------------------------------- */

static void
spi_init(void)
{
	pin_set(PIN_CS, true);          /* the level first, so that a pin never drives the wrong one */
	pin_set(PIN_CLK, false);
	pin_set(PIN_MOSI, false);
	pin_configure(PIN_REGS(PIN_CS), 6, true);
	pin_configure(PIN_REGS(PIN_CLK), 6, true);
	pin_configure(PIN_REGS(PIN_MOSI), 6, true);
	pin_configure(PIN_REGS(PIN_MISO), 2, false);
	/* (the interrupt line is only read: its pull stays as the firmware set it up for the panel) */
}

/* mode 0: data out while the clock is low, sampled as it rises */
static void
spi_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	pin_set(PIN_CS, false);
	sleep_us(2);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx[i], in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(PIN_MOSI, out >> bit & 1);
			sleep_us(1);
			pin_set(PIN_CLK, true);
			sleep_us(1);
			in = (uint8_t)(in << 1 | pin_get(PIN_MISO));
			pin_set(PIN_CLK, false);
		}
		rx[i] = in;
	}
	sleep_us(2);
	pin_set(PIN_CS, true);
}

/* ---- FTS frames ------------------------------------------------------------------------------------- */

#define FTS_HEADER              4       /* address, control, length (2) */
#define FTS_DUMMY               3
#define FTS_WRITE               0x00
#define FTS_READ_CRC            0xa0
#define FTS_CS_HIGH_US          150     /* between frames (CS_HIGH_DELAY) */
#define FTS_PACKET              (32 * 1024 - 16)        /* FTS_FLASH_PACKET_LENGTH_SPI */

static uint8_t frame_tx[FTS_HEADER + FTS_DUMMY + 1 + FTS_PACKET + 2];
static uint8_t frame_rx[sizeof(frame_tx)];

/* CRC-16/Kermit, as the chip puts after the data it is read */
static uint16_t
fts_crc(const uint8_t *data, uint32_t length)
{
	uint16_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

/* fts_write(): buf[0] is the address, the rest data; one byte alone is a four-byte frame */
static bool
fts_write(const uint8_t *buf, uint32_t length)
{
	uint32_t data = length - 1, total = FTS_HEADER;

	if (length == 0 || data > 1 + FTS_PACKET) {
		return false;
	}
	memset(frame_tx, 0, FTS_HEADER + FTS_DUMMY + data);
	frame_tx[0] = buf[0];
	frame_tx[1] = FTS_WRITE;
	frame_tx[2] = (uint8_t)(data >> 8);
	frame_tx[3] = (uint8_t)data;
	if (data > 0) {
		total += FTS_DUMMY;
		memcpy(frame_tx + total, buf + 1, data);
		total += data;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		spi_transfer(frame_tx, frame_rx, total);
		sleep_us(FTS_CS_HIGH_US);
		if (!(frame_rx[3] & 0xa0)) {
			return true;
		}
	}
	return false;
}

static bool
fts_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t offset = FTS_HEADER + FTS_DUMMY;
	uint32_t total = offset + length + 2;

	if (length == 0 || total > sizeof(frame_tx)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(frame_tx, 0, total);
		frame_tx[0] = address;
		frame_tx[1] = FTS_READ_CRC;
		frame_tx[2] = (uint8_t)(length >> 8);
		frame_tx[3] = (uint8_t)length;
		spi_transfer(frame_tx, frame_rx, total);
		sleep_us(FTS_CS_HIGH_US);
		if (!(frame_rx[3] & 0xa0) &&
		    fts_crc(frame_rx + offset, length) == (uint16_t)(frame_rx[offset + length] | frame_rx[offset + length + 1] << 8)) {
			memcpy(data, frame_rx + offset, length);
			return true;
		}
	}
	return false;
}

static bool
fts_chip_id(uint8_t *high, uint8_t *low)
{
	return fts_read(0xa3, high, 1) && fts_read(0x9f, low, 1);
}

/* ---- loading the firmware (focaltech_flash.c, fts_fw_recovery with the FT8756's settings) ----------------- */

#define FW_APP_INFO             0x100   /* code length / 2 and its complement, then at +8 the constants' */
#define FW_DRAM_BASE            0xd00000
#define FW_ECC_LENGTH           32766   /* ecclen_max */
#define FW_ECC_OK               0xa5    /* eccok_val */

static uint16_t
fw_length_at(const uint8_t *fw, uint32_t offset)
{
	uint16_t length = (uint16_t)(fw[offset] << 8 | fw[offset + 1]);
	uint16_t complement = (uint16_t)(fw[offset + 2] << 8 | fw[offset + 3]);

	return (uint16_t)(length + complement) == 0xffff ? length : 0;
}

/* fts_ecc_cal_host(): over big-endian 16-bit words, polynomial 0x8408, from 0 */
static uint16_t
fw_ecc_host(const uint8_t *data, uint32_t length)
{
	uint16_t ecc = 0;

	for (uint32_t i = 0; i < length; i += 2) {
		ecc ^= (uint16_t)(data[i] << 8 | data[i + 1]);
		for (int bit = 0; bit < 16; bit++) {
			ecc = (ecc & 1) ? (uint16_t)((ecc >> 1) ^ 0x8408) : (uint16_t)(ecc >> 1);
		}
	}
	return ecc;
}

/* fts_ecc_cal_tp(): the ROM works out the ECC of what it holds (0xcc), says when (0xce), and what (0xcd) */
static bool
fw_ecc_chip(uint32_t address, uint32_t length, uint16_t *ecc)
{
	uint8_t cmd[7] = { 0xcc, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address,
		           (uint8_t)(length >> 16), (uint8_t)(length >> 8), (uint8_t)length };
	uint8_t value[2] = { 0, 0 };
	int i;

	if (!fts_write(cmd, sizeof(cmd))) {
		return false;
	}
	sleep_ms(2);
	for (i = 0; i < 100; i++) {
		if (!fts_read(0xce, value, 1)) {
			return false;
		}
		if (value[0] == FW_ECC_OK) {
			break;
		}
		sleep_ms(1);
	}
	if (i == 100 || !fts_read(0xcd, value, 2)) {
		return false;
	}
	*ecc = (uint16_t)(value[0] << 8 | value[1]);
	return true;
}

/* fts_dpram_write() then fts_ecc_check(): in packets to `base`, the check over what was written */
static bool
fw_write_checked(const char *what, uint32_t base, const uint8_t *data, uint32_t length)
{
	static uint8_t cmd[1 + FTS_PACKET];

	for (uint32_t offset = 0; offset < length; offset += FTS_PACKET) {
		uint32_t n = MIN(length - offset, FTS_PACKET), address = base + offset;
		uint8_t at[4] = { 0xad, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address };

		cmd[0] = 0xae;
		memcpy(cmd + 1, data + offset, n);
		if (!fts_write(at, sizeof(at)) || !fts_write(cmd, 1 + n)) {
			printf("ft8756: %s: writing at %#x failed\n", what, address);
			return false;
		}
	}
	/* (Xiaomi's driver checks the constants at address 0 too, not at their base: as it does) */
	for (uint32_t offset = 0; offset < length; offset += FW_ECC_LENGTH) {
		uint32_t n = MIN(length - offset, FW_ECC_LENGTH);
		uint16_t chip = 0, host = fw_ecc_host(data + offset, n);

		if (!fw_ecc_chip(offset, n, &chip)) {
			printf("ft8756: %s: the chip did not work out its ECC at %#x\n", what, offset);
			return false;
		}
		if (chip != host) {
			printf("ft8756: %s: ECC %04x, not %04x, at %#x\n", what, chip, host, offset);
			return false;
		}
	}
	return true;
}

static bool
fw_load(void)
{
	uint64_t size = 0;
	const uint8_t *fw = redwood_image_file("fw", "ft8756.bin", &size, NULL);
	uint8_t id[2] = { 0, 0 }, state = 0, start = 0x08;
	uint32_t code, constants;

	if (fw == NULL) {
		printf("ft8756: no fw/ft8756.bin in the small image (tools/mk-fw-touch-miatoll.sh, then userland/build.sh)\n");
		return false;
	}
	/* fts_fw_recovery(): the boot ROM's ID, and its state register saying it waits for a download */
	if (!fts_read(0x90, id, 2) || !fts_read(0xd0, &state, 1) || id[0] != 0x87 || id[1] != 0x56 || state != 1) {
		printf("ft8756: not the boot ROM waiting for firmware (boot ID %02x %02x, state %02x)\n", id[0], id[1], state);
		return false;
	}
	code = (uint32_t)fw_length_at(fw, FW_APP_INFO) * 2;             /* half_length */
	constants = (uint32_t)fw_length_at(fw, FW_APP_INFO + 8) * 2;     /* drwr_support */
	if (size < 0x120 || code == 0 || code + constants > size) {
		printf("ft8756: fw/ft8756.bin (%llu bytes) is not a firmware image for it\n", size);
		return false;
	}
	printf("ft8756: loading the firmware: %u bytes of code, %u of constants\n", code, constants);
	if (!fw_write_checked("code", 0, fw, code) ||
	    (constants != 0 && !fw_write_checked("constants", FW_DRAM_BASE, fw + code, constants))) {
		return false;
	}
	if (!fts_write(&start, 1)) {
		printf("ft8756: the start command was not taken\n");
		return false;
	}
	sleep_ms(10);
	return true;
}

/* the firmware running: the chip ID 87 56; in the boot ROM it reads ef ef, and then it is loaded */
static bool
chip_ready(void)
{
	uint8_t high = 0, low = 0;

	if (!fts_chip_id(&high, &low)) {
		printf("ft8756: no answer on SPI (GPIO %u-%u)\n", PIN_MISO, PIN_CS);
		return false;
	}
	if (high == 0x87 && low == 0x56) {
		return true;
	}
	if (high != 0xef || low != 0xef) {
		printf("ft8756: chip ID %02x %02x, neither the FT8756 (87 56) nor its boot ROM (ef ef)\n", high, low);
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		if (!fw_load()) {
			continue;
		}
		for (int i = 0; i < 40; i++) {
			if (fts_chip_id(&high, &low) && high == 0x87 && low == 0x56) {
				printf("ft8756: the firmware runs\n");
				return true;
			}
			sleep_ms(25);
		}
		printf("ft8756: loaded, but the chip ID is still %02x %02x\n", high, low);
	}
	return false;
}

/* ---- reports ---------------------------------------------------------------------------------------- */

#define MAX_POINTS              10
#define REPORT_LENGTH           (2 + 6 * MAX_POINTS)
#define SCREEN_WIDTH            1080
#define SCREEN_HEIGHT           2400
#define EVENT_UP                1

static struct {
	bool down;
	uint32_t x, y;
} contact[MAX_POINTS];

static uint64_t reports_read, reports_bad;

/* false: the chip has lost its firmware (reads ef ef ef) */
static bool
report_read(void)
{
	uint8_t r[REPORT_LENGTH];
	bool seen[MAX_POINTS] = { false };

	if (!fts_read(0x01, r, sizeof(r))) {
		reports_bad++;
		return true;
	}
	if (r[0] == 0xef && r[1] == 0xef && r[2] == 0xef) {
		return false;
	}
	if ((r[0] & 0xf0) != 0x90) {
		if (reports_bad++ < 5) {
			printf("ft8756: a report starting %02x %02x\n", r[0], r[1]);
		}
		return true;
	}
	reports_read++;
	for (unsigned i = 0; i < MAX_POINTS; i++) {
		const uint8_t *p = r + 2 + 6 * i;
		unsigned event = p[0] >> 6, id = p[2] >> 4;
		uint32_t x = (uint32_t)(p[0] & 0x0f) << 8 | p[1], y = (uint32_t)(p[2] & 0x0f) << 8 | p[3];

		if (id >= MAX_POINTS) {
			continue;               /* an unused slot (id 0xf) */
		}
		if (event == EVENT_UP) {
			continue;               /* lifted: not seen, released below */
		}
		if (x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) {
			continue;
		}
		x = x * 65535 / (SCREEN_WIDTH - 1);
		y = y * 65535 / (SCREEN_HEIGHT - 1);
		seen[id] = true;
		if (!contact[id].down) {
			contact[id].down = true;
			redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
		} else if (contact[id].x != x || contact[id].y != y) {
			redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
		}
		contact[id].x = x;
		contact[id].y = y;
	}
	for (unsigned id = 0; id < MAX_POINTS; id++) {
		if (contact[id].down && !seen[id]) {
			contact[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, contact[id].x, contact[id].y);
		}
	}
	return true;
}

static void
touch_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t last_line = 0;

	spi_init();
	if (!chip_ready()) {
		printf("ft8756: no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("ft8756: touch screen ready (1080 x 2400)\n");
	interrupt_catch();
	for (;;) {
		bool any_down = false;

		for (unsigned id = 0; id < MAX_POINTS; id++) {
			any_down |= contact[id].down;
		}
		/* (and while a finger is down, every time: a lift whose report was missed is not left standing) */
		if (interrupt_pending() || any_down) {
			if (!report_read()) {
				printf("ft8756: the chip lost its firmware: loading it again\n");
				if (!chip_ready()) {
					printf("ft8756: no touch screen any more\n");
					break;
				}
			}
		}
		if (reports_read >= last_line + 1000) {
			last_line = reports_read;
			printf("ft8756: %llu reports, %llu bad\n", reports_read, reports_bad);
		}
		sleep_ms(4);
	}
	thread_terminate(current_thread());
}

void
miatoll_touch_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("ft8756", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	spi_pins = ml_io_map(TLMM_WEST_PHYS + 0x1000UL * PIN_MISO, 0x1000UL * (PIN_CS - PIN_MISO + 1));
	int_pin = ml_io_map(TLMM_NORTH_PHYS + 0x1000UL * PIN_INT, 0x1000UL);
	if (spi_pins == 0 || int_pin == 0) {
		printf("ft8756: cannot map the pins\n");
		return;
	}
	if (kernel_thread_start(touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
