/*
 * The phone's USB port as the link behind en0 (sys/redwood_net.h): plugged into a computer, the
 * phone is a USB Ethernet adapter (CDC-ECM, which Linux's cdc_ether and macOS drive with no driver
 * of their own), and the computer can share its connection over it. Boot-arg usbnet=1, miatoll only.
 *
 * The controller is a Synopsys DWC3 at 0x0a600000 behind Qualcomm's wrapper. The firmware leaves it
 * powered, clocked and in device mode, with its PHY set up: it is the port the loader's fastboot
 * runs on. The loader leaves it alone for a run with usbnet=1 (xnuloader/loader.c: run_on_other_cpu),
 * so the kernel restarts only the device controller: soft reset, an event buffer of its own, endpoint
 * 0, run. Events are polled by a thread of its own (no interrupt). Its DMA is not cache coherent:
 * buffers are cleaned and invalidated around every transfer. Registers and the control transfer's
 * state machine follow Linux's drivers/usb/dwc3 (gadget.c, ep0.c); the descriptors, its f_ecm.c.
 * The first version of this ran on the same phone in the ios-miatoll port (sweet_usb.c).
 *
 * After such a run the loader cannot take the port back: put the phone into fastboot by hand.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <net/ethernet.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/redwood_net.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);   /* osfmk/arm64/caches_asm.s */

#define MIATOLL_SOC_BASE        0x00100000ul

#define GCC_PA                  0x00100000
#define GCC_SIZE                0x00090000
#define GCC_USB30_PRIM_GDSCR    0x0f004         /* bit 31: the power domain is on */
#define GCC_USB30_MASTER_CBCR   0x0f010
#define GCC_CFG_NOC_USB3_AXI_CBCR 0x0502c
#define CBCR_CLK_OFF            (1u << 31)

#define DWC3_PA                 0x0a600000
#define DWC3_SIZE               0x0000d000
/* the wrapper's registers ("QSCRATCH"): the controller connects only while it sees a VBUS session */
#define QSCRATCH_PA             0x0a6f8800
#define QSCRATCH_HS_PHY_CTRL    0x10
#define   UTMI_OTG_VBUS_VALID   (1u << 20)
#define   SW_SESSVLD_SEL        (1u << 28)
#define QSCRATCH_SS_PHY_CTRL    0x30
#define   LANE0_PWR_PRESENT     (1u << 24)

#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)   (0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)   (0xc808 + 0x10 * (n))
#define DEPCMD(n)       (0xc80c + 0x10 * (n))

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)

#define DEPCMD_SETEPCONFIG      1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL         4
#define DEPCMD_STARTTRANSFER    6
#define DEPCMD_ENDTRANSFER      8
#define DEPCMD_DEPSTARTCFG      9
#define DEPCMD_CMDACT           (1u << 10)
#define DEPCMD_FORCERM          (1u << 11)

#define TRB_HWO         (1u << 0)
#define TRB_LST         (1u << 1)
#define TRB_ISP_IMI     (1u << 10)
#define TRB_IOC         (1u << 11)
#define TRBCTL_NORMAL   (1u << 4)
#define TRBCTL_SETUP    (2u << 4)
#define TRBCTL_STATUS2  (3u << 4)
#define TRBCTL_STATUS3  (4u << 4)
#define TRBCTL_DATA     (5u << 4)

/* the DMA area */
#define UD_EVT          0x0000          /* event buffer */
#define UD_EVT_SIZE     0x1000
#define UD_TRB_OUT      0x1000          /* ep0 OUT */
#define UD_TRB_IN       0x1010          /* ep0 IN */
#define UD_TRB_RX       0x1020          /* bulk OUT: computer -> phone */
#define UD_TRB_TX       0x1030          /* bulk IN: phone -> computer */
#define UD_SETUP        0x1040          /* the 8 bytes of a setup packet */
#define UD_TRB_INT      0x1050          /* interrupt IN: notifications */
#define UD_DATA         0x1100          /* ep0's data stage */
#define UD_NOTIFY       0x1300
#define UD_RX           0x2000          /* one frame received */
#define UD_TX           0x2800          /* one frame to send */
#define UD_FRAME_MAX    0x800
#define UD_SIZE         0x4000

/* physical endpoints: 2n OUT, 2n + 1 IN */
#define EP_RX           2               /* 0x01 bulk OUT */
#define EP_TX           3               /* 0x81 bulk IN */
#define EP_INT          5               /* 0x82 interrupt IN */

#define UD_TXQ          32              /* frames waiting for the cable */

static struct {
	vm_offset_t     dwc, dma;
	uint64_t        dma_pa;
	uint32_t        evt_pos;
	int             sleep_chan;
	enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS } state;
	bool            configured, link;
	uint8_t         data_alt;
	uint8_t         resource[8];    /* each endpoint's transfer resource, for ending its transfer */
	bool            active[8];      /* a transfer is started on it */
	uint32_t        setups, events;
	uint64_t        received, sent, dropped;
	/* frames to send: copies, so that the network stack has its own back at once */
	uint8_t         txq[UD_TXQ][REDWOOD_NET_FRAME_MAX];
	uint16_t        txq_len[UD_TXQ];
	unsigned        txq_head, txq_count;
	bool            tx_busy;
} ud;

static LCK_GRP_DECLARE(usbnet_lck_grp, "redwood-usbnet");
static LCK_MTX_DECLARE(usbnet_lock, &usbnet_lck_grp);        /* the send queue and the IN endpoint */

static inline uint32_t
dwc_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(ud.dwc + off);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static inline void
dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(ud.dwc + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
ud_sync(uint32_t off, uint32_t len)
{
	FlushPoC_DcacheRegion(ud.dma + off, len);
}

static void
ud_sleep_ms(unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&ud.sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static int
ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));

		if (!(v & DEPCMD_CMDACT)) {
			if ((v >> 12) & 0xf) {
				printf("usbnet: endpoint %u: command 0x%x failed, status %u\n", ep, cmd & 0xf, (v >> 12) & 0xf);
				return -1;
			}
			if ((cmd & 0xf) == DEPCMD_STARTTRANSFER && ep < 8) {
				ud.resource[ep] = (uint8_t)((v >> 16) & 0x7f);
				ud.active[ep] = true;
			}
			return 0;
		}
		delay(1);
	}
	printf("usbnet: endpoint %u: command 0x%x did not finish\n", ep, cmd & 0xf);
	return -1;
}

/* One TRB at DMA offset `trb` on physical endpoint `ep`. */
static void
ud_start_trb(unsigned ep, uint32_t trb, uint32_t buf, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);
	uint64_t bpa = ud.dma_pa + buf, tpa = ud.dma_pa + trb;

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	ud_sync(trb, 16);
	if (len) {
		ud_sync(buf, len);
	}
	ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0);
}

static void
ud_end_transfer(unsigned ep)
{
	if (ud.active[ep]) {
		ud_epcmd(ep, DEPCMD_ENDTRANSFER | DEPCMD_FORCERM | ((uint32_t)ud.resource[ep] << 16), 0, 0, 0);
		ud.active[ep] = false;
	}
}

static void
ud_start_setup(void)
{
	ud.state = EP0_SETUP;
	ud_start_trb(0, UD_TRB_OUT, UD_SETUP, 8, TRBCTL_SETUP);
}

static void
ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/*
 * The descriptors: one CDC-ECM function. VID 0x1209 / PID 0x0001 is pid.codes' test ID.
 * The computer's end gets the address in string 4, the phone's is the next one.
 */
static const uint8_t ud_dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64,
	0x09, 0x12, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t ud_cfg_desc[80] = {
	9, 2, 80, 0, 2, 1, 0, 0x80, 250,                /* configuration: 2 interfaces, 500 mA */
	9, 4, 0, 0, 1, 0x02, 0x06, 0x00, 0,             /* interface 0: communications, ECM, 1 endpoint */
	5, 0x24, 0x00, 0x10, 0x01,                      /* header, CDC 1.10 */
	5, 0x24, 0x06, 0, 1,                            /* union: control 0, data 1 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0, /* Ethernet: address in string 4, frames up to 1514 */
	7, 5, 0x82, 0x03, 16, 0, 9,                     /* 0x82 interrupt IN, 16 bytes */
	9, 4, 1, 0, 0, 0x0a, 0x00, 0x00, 0,             /* interface 1 setting 0: no endpoints (link off) */
	9, 4, 1, 1, 2, 0x0a, 0x00, 0x00, 0,             /* interface 1 setting 1: the two bulk endpoints */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* 0x81 bulk IN, 512 */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* 0x01 bulk OUT, 512 */
};
static const char *const ud_strings[] = {
	NULL, "HackiOS", "HackiOS (USB Ethernet)", "0001", "02005E77EE01",
};

static uint32_t
ud_string(unsigned idx, uint8_t *out)
{
	const char *str;
	uint32_t n = 2;

	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;   /* en-US */
		return 4;
	}
	if (idx >= sizeof(ud_strings) / sizeof(ud_strings[0])) {
		return 0;
	}
	for (str = ud_strings[idx]; *str && n < 126; str++) {
		out[n++] = (uint8_t)*str;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

static void
ud_link(bool up)
{
	if (up != ud.link) {
		ud.link = up;
		printf("usbnet: the cable's link is %s\n", up ? "up" : "down");
		redwood_net_link_state(up);
	}
}

static void
ud_rx_arm(void)
{
	ud_start_trb(EP_RX, UD_TRB_RX, UD_RX, UD_FRAME_MAX, TRBCTL_NORMAL);
}

/* NETWORK_CONNECTION: the computer brings its end up on it */
static void
ud_notify_connected(void)
{
	uint8_t *n = (uint8_t *)(ud.dma + UD_NOTIFY);

	n[0] = 0xa1; n[1] = 0x00;
	n[2] = 1; n[3] = 0;
	n[4] = 0; n[5] = 0;
	n[6] = 0; n[7] = 0;
	ud_start_trb(EP_INT, UD_TRB_INT, UD_NOTIFY, 8, TRBCTL_NORMAL);
}

/* SET_CONFIGURATION(1): the bulk endpoints and the interrupt one */
static void
ud_configure(void)
{
	if (ud.configured) {
		return;
	}
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | (2u << 16), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETEPCONFIG, (3u << 1) | (16u << 3) | (2u << 17),
	    (1u << 8) | (8u << 16) | ((uint32_t)EP_INT << 25), 0) ||
	    ud_epcmd(EP_INT, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("usbnet: the data endpoints could not be set up\n");
		return;
	}
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX) | (1u << EP_INT));
	ud.configured = true;
	ud_rx_arm();
	printf("usbnet: configured\n");
}

/* a bus reset or the cable gone: back to how enumeration finds it */
static void
ud_unconfigure(void)
{
	ud_link(false);
	lck_mtx_lock(&usbnet_lock);
	ud_end_transfer(EP_RX);
	ud_end_transfer(EP_TX);
	ud_end_transfer(EP_INT);
	dwc_wr(DALEPENA, 0x3);
	ud.configured = false;
	ud.data_alt = 0;
	ud.tx_busy = false;
	ud.txq_count = 0;
	lck_mtx_unlock(&usbnet_lock);
}

static void
ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + UD_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + UD_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	bool data_in = (type & 0x80) != 0, ok = true;
	uint32_t n = 0;

	if (ud.setups++ < 40) {
		printf("usbnet: setup %02x %02x %04x %04x length %u\n", type, req, value, index, length);
	}
	if ((type & 0x60) == 0x20) {    /* class requests (SET_ETHERNET_PACKET_FILTER): taken, not followed */
		ok = length == 0;
	} else if ((type & 0x60) != 0) {
		ok = false;
	} else if (req == 6) {          /* GET_DESCRIPTOR */
		switch (value >> 8) {
		case 1: memcpy(data, ud_dev_desc, sizeof(ud_dev_desc)); n = sizeof(ud_dev_desc); break;
		case 2: memcpy(data, ud_cfg_desc, sizeof(ud_cfg_desc)); n = sizeof(ud_cfg_desc); break;
		case 3: n = ud_string(value & 0xff, data); ok = n != 0; break;
		default: ok = false; break;
		}
	} else if (req == 5) {          /* SET_ADDRESS: at once (the controller sees to the timing) */
		dwc_wr(DCFG, (dwc_rd(DCFG) & ~(0x7fu << 3)) | ((uint32_t)(value & 0x7f) << 3));
	} else if (req == 9) {          /* SET_CONFIGURATION */
		if (value == 1) {
			ud_configure();
		}
	} else if (req == 8) {          /* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0; n = 1;
	} else if (req == 0) {          /* GET_STATUS */
		data[0] = 0; data[1] = 0; n = 2;
	} else if (req == 11) {         /* SET_INTERFACE: setting 1 of the data interface is the link on */
		if (index == 1) {
			ud.data_alt = (uint8_t)value;
			if (value == 1 && ud.configured) {
				ud_notify_connected();
			}
			ud_link(value == 1 && ud.configured);
		}
	} else if (req == 10) {         /* GET_INTERFACE */
		data[0] = index == 1 ? ud.data_alt : 0; n = 1;
	} else if (req == 1 || req == 3) { /* CLEAR/SET_FEATURE */
	} else {
		ok = false;
	}
	if (!ok) {
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.state = EP0_WAIT_STATUS2;    /* the status stage on XferNotReady */
		return;
	}
	ud.state = EP0_DATA;
	if (data_in) {
		ud_start_trb(1, UD_TRB_IN, UD_DATA, n < length ? n : length, TRBCTL_DATA);
	} else {
		ud_start_trb(0, UD_TRB_OUT, UD_DATA, 64, TRBCTL_DATA);  /* (what comes is not used) */
	}
}

static void
ud_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + UD_TRB_RX);
	uint32_t len;

	ud.active[EP_RX] = false;
	ud_sync(UD_TRB_RX, 16);
	len = UD_FRAME_MAX - (t[2] & 0xffffff);
	ud_sync(UD_RX, UD_FRAME_MAX);
	if (len >= ETHER_HDR_LEN) {
		/* (a frame of a multiple of 512 bytes comes with a byte more, instead of a packet of none) */
		ud.received++;
		redwood_net_input((const uint8_t *)(ud.dma + UD_RX), len > REDWOOD_NET_FRAME_MAX ? REDWOOD_NET_FRAME_MAX : len);
	}
	if (ud.configured) {
		ud_rx_arm();
	}
}

/* The next frame in the queue, if the IN endpoint is free. usbnet_lock held. */
static void
ud_tx_kick_locked(void)
{
	uint32_t len;

	if (ud.tx_busy || ud.txq_count == 0 || !ud.configured) {
		return;
	}
	len = ud.txq_len[ud.txq_head];
	memcpy((void *)(ud.dma + UD_TX), ud.txq[ud.txq_head], len);
	ud.txq_head = (ud.txq_head + 1) % UD_TXQ;
	ud.txq_count--;
	/* a transfer of a multiple of 512 bytes would need a packet of none to end it: one byte more does it */
	if (len % 512 == 0) {
		((uint8_t *)(ud.dma + UD_TX))[len++] = 0;
	}
	ud.tx_busy = true;
	ud.sent++;
	ud_start_trb(EP_TX, UD_TRB_TX, UD_TX, len, TRBCTL_NORMAL);
}

/* en0's transmit (bsd/dev/redwood_net.c) */
static bool
ud_transmit(mbuf_t packet, size_t length)
{
	unsigned slot;

	if (length > REDWOOD_NET_FRAME_MAX) {
		return false;
	}
	lck_mtx_lock(&usbnet_lock);
	if (!ud.configured || ud.txq_count == UD_TXQ) {
		ud.dropped++;
		lck_mtx_unlock(&usbnet_lock);
		return false;
	}
	slot = (ud.txq_head + ud.txq_count) % UD_TXQ;
	if (mbuf_copydata(packet, 0, length, ud.txq[slot]) != 0) {
		lck_mtx_unlock(&usbnet_lock);
		return false;
	}
	ud.txq_len[slot] = (uint16_t)length;
	ud.txq_count++;
	ud_tx_kick_locked();
	lck_mtx_unlock(&usbnet_lock);
	return true;
}

static void
ud_event(uint32_t ev)
{
	unsigned ep, type, status;

	if (ev & 1) {                   /* a device event */
		type = (ev >> 8) & 0xf;
		if (type == 1) {        /* bus reset */
			dwc_wr(DCFG, dwc_rd(DCFG) & ~(0x7fu << 3));
			ud_unconfigure();
			printf("usbnet: bus reset\n");
		} else if (type == 2) { /* connect done: ep0's packets are 64 bytes at high and full speed */
			ud_epcmd(0, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10), 0);
			ud_epcmd(1, DEPCMD_SETEPCONFIG, (64u << 3) | (2u << 30), (1u << 8) | (1u << 10) | (1u << 25), 0);
			printf("usbnet: connected, speed %u\n", dwc_rd(DSTS) & 7);
		} else if (type == 0) {
			ud_unconfigure();
			printf("usbnet: disconnected\n");
		}
		return;
	}
	ep = (ev >> 1) & 0x1f;
	type = (ev >> 6) & 0xf;
	status = (ev >> 12) & 0xf;
	if (ep == EP_RX && type == 1) {
		ud_rx_done();
		return;
	}
	if (ep == EP_TX && type == 1) {
		lck_mtx_lock(&usbnet_lock);
		ud.active[EP_TX] = false;
		ud.tx_busy = false;
		ud_tx_kick_locked();
		lck_mtx_unlock(&usbnet_lock);
		return;
	}
	if (ep == EP_INT && type == 1) {
		ud.active[EP_INT] = false;
		return;
	}
	if (ep > 1) {
		return;
	}
	/*
	 * The control transfer (Linux's ep0.c): SETUP done -> the request is handled and its data stage
	 * started; the status stage is started when the controller says XferNotReady, "control status";
	 * when that is done, SETUP again.
	 */
	if (type == 1) {                /* XferComplete */
		ud.active[ep] = false;
		switch (ud.state) {
		case EP0_SETUP:
			ud_sync(UD_SETUP, 8);
			ud_setup();
			break;
		case EP0_DATA:
			ud.state = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
	} else if (type == 3 && status == 2) {  /* XferNotReady, control status */
		if (ud.state == EP0_WAIT_STATUS2 || ud.state == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.state == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;

			ud.state = EP0_STATUS;
			ud_start_trb(ep, ep ? UD_TRB_IN : UD_TRB_OUT, UD_DATA, 0, ctl);
		}
	}
}

/*
 * The events, as they come: while frames flow it looks again at once (a pause of a millisecond
 * after every frame would hold the link to a frame a millisecond), when they stop it sleeps.
 */
static void
ud_thread(void *arg __unused, wait_result_t wr __unused)
{
	uint64_t said = 0, now, minute;
	unsigned idle = 0;

	nanoseconds_to_absolutetime(60 * NSEC_PER_SEC, &minute);

	for (;;) {
		uint32_t count = dwc_rd(GEVNTCOUNT) & 0xffff;

		if (count) {
			ud_sync(UD_EVT, UD_EVT_SIZE);
			for (uint32_t done = 0; done < count; done += 4) {
				uint32_t ev = *(volatile uint32_t *)(ud.dma + UD_EVT + ud.evt_pos);

				ud.evt_pos = (ud.evt_pos + 4) % UD_EVT_SIZE;
				if (ud.events++ < 64) {
					printf("usbnet: event 0x%08x\n", ev);
				}
				ud_event(ev);
			}
			dwc_wr(GEVNTCOUNT, count);
			idle = 0;
			continue;
		}
		if (idle++ < 400) {
			delay(5);
			continue;
		}
		now = mach_absolute_time();
		if (ud.link && now - said > minute) {
			printf("usbnet: %llu frames in, %llu out, %llu dropped\n", ud.received, ud.sent, ud.dropped);
			said = now;
		}
		ud_sleep_ms(1);
	}
}

static const struct redwood_net_link ud_net_link = {
	.name = "USB (CDC-ECM)",
	.address = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 },
	.wireless = false,
	.up = false,
	.transmit = ud_transmit,
};

static bool
ud_start(void)
{
	vm_offset_t gcc, qs;
	uint32_t gdscr, hs, ss;
	thread_t thread;
	int i;

	/* the controller's registers only with its power domain and clocks on: a gated block hangs the core */
	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	if (gcc == 0) {
		return false;
	}
	gdscr = *(volatile uint32_t *)(gcc + GCC_USB30_PRIM_GDSCR);
	if (!(gdscr & (1u << 31)) || (*(volatile uint32_t *)(gcc + GCC_USB30_MASTER_CBCR) & CBCR_CLK_OFF) ||
	    (*(volatile uint32_t *)(gcc + GCC_CFG_NOC_USB3_AXI_CBCR) & CBCR_CLK_OFF)) {
		printf("usbnet: the USB controller is off (GDSC 0x%08x): the firmware did not leave it on\n", gdscr);
		return false;
	}
	ud.dwc = ml_io_map(DWC3_PA, DWC3_SIZE);
	if (ud.dwc == 0 || kmem_alloc_contig(kernel_map, &ud.dma, UD_SIZE, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("usbnet: no memory for the controller\n");
		return false;
	}
	ud.dma_pa = kvtophys(ud.dma);
	printf("usbnet: DWC3 0x%08x, GCTL 0x%08x DCTL 0x%08x DSTS 0x%08x; DMA at 0x%llx\n", dwc_rd(0xc120), dwc_rd(GCTL),
	    dwc_rd(DCTL), dwc_rd(DSTS), ud.dma_pa);

	/* the firmware's device is stopped, then the device controller reset (the PHY stays as it is) */
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	for (i = 0; i < 100 && !(dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		delay(1000);
	}
	dwc_wr(DCTL, DCTL_CSFTRST);
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++) {
		delay(1000);
	}
	if (dwc_rd(DCTL) & DCTL_CSFTRST) {
		printf("usbnet: the controller's reset did not finish\n");
		return false;
	}

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma_pa + UD_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma_pa + UD_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | UD_EVT_SIZE);     /* its interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;
	/* high speed (not SuperSpeed: one PHY less to depend on), address 0 */
	dwc_wr(DCFG, dwc_rd(DCFG) & ~(7u | (0x7fu << 3)));
	dwc_wr(DEVTEN, 0x7);                            /* disconnect, bus reset, connect done */

	/* endpoint 0, both ways: control, 512-byte packets until the connection's speed is known */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, 512u << 3, (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
		printf("usbnet: endpoint 0 could not be set up\n");
		return false;
	}
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	/* a VBUS session as far as the controller can tell (Linux: dwc3_qcom_vbus_override_enable) */
	qs = ml_io_map(QSCRATCH_PA, 0x100);
	if (qs != 0) {
		hs = *(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL);
		ss = *(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL);
		*(volatile uint32_t *)(qs + QSCRATCH_HS_PHY_CTRL) = hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL;
		*(volatile uint32_t *)(qs + QSCRATCH_SS_PHY_CTRL) = ss | LANE0_PWR_PRESENT;
		__asm__ volatile ("dsb sy" ::: "memory");
	}

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++) {
		delay(1000);
	}
	printf("usbnet: running (DSTS 0x%08x after %d ms): the computer sees a USB Ethernet adapter\n", dwc_rd(DSTS), i);
	if (kernel_thread_start(ud_thread, NULL, &thread) != KERN_SUCCESS) {
		return false;
	}
	thread_deallocate(thread);
	return true;
}

void
redwood_usbnet_init(void)
{
	int enabled = 0;

	if (!PE_parse_boot_argn("usbnet", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	if (pe_arm_get_soc_base_phys() != MIATOLL_SOC_BASE) {
		printf("usbnet: usbnet=1 is for miatoll's USB controller only\n");
		return;
	}
	if (!redwood_net_attach(&ud_net_link)) {
		printf("usbnet: en0 is taken by another link\n");
		return;
	}
	if (!ud_start()) {
		printf("usbnet: the USB port could not be started\n");
	}
}
