/*
 * Where the panel's scan is, on the POCO X5 Pro (redwood): for iokit/Kernel/MiniIOMobileFramebuffer.cpp, which
 * puts every frame into the memory the panel is sent from and, without this, can only guess when the panel
 * reads it -- which is why the picture tore and why frames came at a pace of their own, beside the panel's.
 *
 * The panel is a DSI video panel, sent the framebuffer over and over by the display controller's timing engine
 * at the rate of its timing registers, which nobody touches after the firmware: 2432 lines a frame of which
 * 2400 are shown, 119.93 frames a second (seen: redwood_adreno.c, display_probe). The interface the panel hangs
 * on (INTF 1, dt: mdss_dsi0's mdp_intf_base) counts the frames and, within a frame, the line being sent; this
 * reads those two counters. To read them at all two bus clocks have to run, which the firmware leaves off: the
 * one of the display's clock controller (in GCC) and, in that controller, the one of the display block's
 * registers. They are turned on here, as Linux keeps them on and as display_probe did on this phone; that is all
 * that is ever written. The timing itself is only read.
 *
 * Boot-arg fb_scan=1 (on the phone; QEMU has no such block): on. Then the framebuffer driver
 *   - starts copying a frame into the panel's memory only where the scan lets the copy land whole in one
 *     refresh (redwood_display_window(): see MiniIOMobileFramebuffer.cpp, show), so that no refresh shows
 *     part of one frame and part of the next -- no tearing;
 *   - runs at the panel's own rate (120 Hz) and tells CoreAnimation that rate, and its refresh notifications
 *     come when the panel's frames begin, not on a clock of their own that drifts against them.
 *
 * Registers: Linux drivers/gpu/drm/msm/disp/dpu1/dpu_hw_intf.c (INTF), drivers/clk/qcom/dispcc-sc7280.c.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_display.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GCC_PHYS                0x00100000UL
#define DISPCC_PHYS             0x0af00000UL    /* dt: clock-controller@af00000 */
/*
 * Which phone: redwood's (SC7280: gcc-sc7280.c, dispcc-sc7280.c, mdss_dsi0's mdp_intf_base) or
 * miatoll's (SM7125, "atoll": gcc-atoll.c gcc_disp_ahb_clk, dispcc-atoll.c disp_cc_mdss_ahb_clk,
 * dt: qcom,gdsc@af03000 mdss_core_gdsc, qcom,mdss_mdp@ae00000 with qcom,sde-intf-off <0x6b000
 * 0x6b800>: INTF 1, the DSI panel's, at 0x6b800). The loader says which by where the peripherals begin.
 */
#define MIATOLL_SOC_BASE        0x00100000ul
extern vm_offset_t pe_arm_get_soc_base_phys(void);
static uint32_t GCC_DISP_AHB_CBCR = 0x27004, DISPCC_MDSS_CORE_GDSCR = 0x1004, DISPCC_MDSS_AHB_CBCR = 0x1050;
static vm_offset_t INTF1_PHYS = 0x0ae36000UL;
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_POWER_ON           (1u << 31)

#define INTF_TIMING_ENGINE_EN   0x000
#define INTF_HSYNC_CTL          0x008           /* clocks a line << 16 | of which the pulse */
#define INTF_VSYNC_PERIOD       0x00c           /* clocks a frame */
#define INTF_DISPLAY_V_START    0x01c
#define INTF_DISPLAY_V_END      0x024
#define INTF_FRAME_COUNT        0x0ac
#define INTF_LINE_COUNT         0x0b0

static vm_offset_t intf;
static struct redwood_display_scan scan;

static inline uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
wr(vm_offset_t base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
}

/* one bus clock on (and left on); 1 if it runs */
static int
clock_on(vm_offset_t base, uint32_t cbcr, const char *name)
{
	uint32_t v = rd(base, cbcr);

	if (!(v & CBCR_ENABLE)) {
		wr(base, cbcr, v | CBCR_ENABLE);
		delay_for_interval(10, NSEC_PER_MSEC);
		v = rd(base, cbcr);
	}
	if (!(v & CBCR_ENABLE)) {
		printf("display: the %s clock does not take (%#x): no scan to go by\n", name, v);
		return 0;
	}
	return 1;
}

uint32_t
redwood_display_line(uint32_t *frame)
{
	uint32_t f, line;

	if (intf == 0) {
		return 0;
	}
	/* the two counters together: a frame that begins between the two reads is read again */
	do {
		f = rd(intf, INTF_FRAME_COUNT);
		line = rd(intf, INTF_LINE_COUNT);
	} while (rd(intf, INTF_FRAME_COUNT) != f);
	if (frame != NULL) {
		*frame = f;
	}
	return line;
}

const struct redwood_display_scan *
redwood_display_scan(void)
{
	return scan.ok ? &scan : NULL;
}

/*
 * Wait until the scan is in [first, last) of a frame (lines, 0 = the first line shown), at most `limit_ns`;
 * the wait is a sleep for most of the way and a short spin for the end. 1 when it got there.
 */
int
redwood_display_wait_lines(uint32_t first, uint32_t last, uint64_t limit_ns)
{
	uint64_t start = mach_absolute_time(), now, waited;

	if (!scan.ok || first >= last || last > scan.lines) {
		return 0;
	}
	for (;;) {
		uint32_t line = redwood_display_line(NULL);
		uint64_t ahead_ns;

		if (line >= first && line < last) {
			return 1;
		}
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - start, &waited);
		if (waited >= limit_ns) {
			return 0;
		}
		/* lines to go to `first`, as time: sleep all but the last 200 us of it */
		ahead_ns = (uint64_t)((first + scan.lines - line) % scan.lines) * scan.line_ns;
		if (ahead_ns > 400000) {
			uint64_t nap = ahead_ns - 200000;

			if (waited + nap > limit_ns) {
				nap = limit_ns - waited;
			}
			delay_for_interval((uint32_t)(nap / 1000), NSEC_PER_USEC);
		}
	}
}

/* nanoseconds until the next frame begins (its first line shown), from where the scan is now */
uint64_t
redwood_display_next_frame_ns(void)
{
	uint32_t line;

	if (!scan.ok) {
		return 0;
	}
	line = redwood_display_line(NULL);
	return (uint64_t)(scan.lines - line) * scan.line_ns;
}

int
redwood_display_scan_start(void)
{
	uint32_t on = 0, hsync, period, v0, v1, f0, f1, l0, l1;
	uint64_t t0, t1, ns, gone;
	vm_offset_t gcc, dispcc;

	if (scan.ok) {
		return 1;
	}
	if (!PE_parse_boot_argn("fb_scan", &on, sizeof(on)) || on != 1) {
		return 0;
	}
	if (pe_arm_get_soc_base_phys() == MIATOLL_SOC_BASE) {
		GCC_DISP_AHB_CBCR = 0xb00c;
		DISPCC_MDSS_CORE_GDSCR = 0x3000;
		DISPCC_MDSS_AHB_CBCR = 0x2080;
		INTF1_PHYS = 0x0ae6b800UL;
	}
	gcc = ml_io_map(GCC_PHYS + (GCC_DISP_AHB_CBCR & ~0xfffu), 0x1000);
	dispcc = ml_io_map(DISPCC_PHYS, 0x4000);
	if (!clock_on(gcc, GCC_DISP_AHB_CBCR & 0xfffu, "display clock controller's bus")) {
		return 0;
	}
	if (!(rd(dispcc, DISPCC_MDSS_CORE_GDSCR) & GDSC_POWER_ON)) {
		printf("display: the display block has no power (the firmware did not leave the panel on?): no scan to go by\n");
		return 0;
	}
	if (!clock_on(dispcc, DISPCC_MDSS_AHB_CBCR, "display registers' bus")) {
		return 0;
	}
	intf = ml_io_map(INTF1_PHYS & ~(vm_offset_t)0xfff, 0x1000) + (INTF1_PHYS & 0xfff);   /* (miatoll's is at +0x800 of its page) */
	hsync = rd(intf, INTF_HSYNC_CTL) >> 16;
	period = rd(intf, INTF_VSYNC_PERIOD);
	v0 = rd(intf, INTF_DISPLAY_V_START);
	v1 = rd(intf, INTF_DISPLAY_V_END);
	if (hsync == 0 || period < hsync || v1 <= v0) {
		printf("display: the timing engine's registers make no sense (line %u, frame %u clocks): no scan to go by\n", hsync, period);
		intf = 0;
		return 0;
	}
	scan.lines = period / hsync;
	scan.shown = (v1 - v0 + 1) / hsync;
	/* the rate the panel is really sent at: the counters 100 ms apart */
	l0 = redwood_display_line(&f0);
	t0 = mach_absolute_time();
	delay_for_interval(100, NSEC_PER_MSEC);
	l1 = redwood_display_line(&f1);
	t1 = mach_absolute_time();
	absolutetime_to_nanoseconds(t1 - t0, &ns);
	gone = (uint64_t)(f1 - f0) * scan.lines + l1 - l0;
	if (f1 == f0 || gone == 0 || ns == 0 || scan.lines < scan.shown || scan.shown == 0) {
		printf("display: the scan counters do not move (frame %u -> %u, line %u -> %u): no scan to go by\n", f0, f1, l0, l1);
		intf = 0;
		return 0;
	}
	scan.line_ns = ns / gone;
	scan.frame_ns = scan.line_ns * scan.lines;
	scan.millihz = (uint32_t)(1000000000000ULL / scan.frame_ns);
	scan.ok = 1;
	printf("display: the panel's scan: %u lines a frame (%u shown), %u.%03u frames a second, %llu ns a line; "
	    "frames follow it (fb_scan=1)\n", scan.lines, scan.shown, scan.millihz / 1000, scan.millihz % 1000, scan.line_ns);
	return 1;
}
