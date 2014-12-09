/*
 */

#include <common.h>
#include <malloc.h>
#include <miiphy.h>
#include <net.h>
#include <linux/mii.h>

#include "zynq_gem.h"

#if defined(CONFIG_ZYNQ_GEM0_EMIO) && !defined(CONFIG_ZYNQ_GEM0_FPGA_CLK_REG)
#error CONFIG_ZYNQ_GEM0_FPGA_CLK_REG must be defined in EMIO mode
#endif
#if defined(CONFIG_ZYNQ_GEM1_EMIO) && !defined(CONFIG_ZYNQ_GEM1_FPGA_CLK_REG)
#error CONFIG_ZYNQ_GEM1_FPGA_CLK_REG must be defined in EMIO mode
#endif

/************************ Forward function declaration **********************/

static int Xgmac_process_rx(XEmacPss *EmacPssInstancePtr);
static int Xgmac_init_rxq(XEmacPss *EmacPssInstancePtr,
			void *bd_start, int num_elem);
static int Xgmac_make_rxbuff_mem(XEmacPss *EmacPssInstancePtr,
			void *rx_buf_start, u32 rx_buffsize);
static int Xgmac_next_rx_buf(XEmacPss *EmacPssInstancePtr);
static int Xgmac_phy_mgmt_idle(XEmacPss *EmacPssInstancePtr);

#ifdef CONFIG_EP107
static void Xgmac_set_eth_advertise(XEmacPss *EmacPssInstancePtr,
			int link_speed);
#endif

/*************************** Constant Definitions ***************************/

#define RXBD_CNT       8	/* Number of RxBDs to use */
#define TXBD_CNT       8	/* Number of TxBDs to use */

#define phy_spinwait(e) do { while (!Xgmac_phy_mgmt_idle(e)); } while (0)

#define dmb() __asm__ __volatile__ ("dmb" : : : "memory")

/*************************** Variable Definitions ***************************/

/*
 * Aligned memory segments to be used for buffer descriptors
 */
//#define BRAM_BUFFERS
#ifdef BRAM_BUFFERS
static XEmacPss_Bd RxBdSpace[RXBD_CNT] __attribute__ ((section (".bram_buffers")));
static XEmacPss_Bd TxBdSpace[TXBD_CNT] __attribute__ ((section (".bram_buffers")));
static char RxBuffers[RXBD_CNT * XEMACPSS_RX_BUF_SIZE] __attribute__ ((section (".bram_buffers")));
static uchar data_buffer[XEMACPSS_RX_BUF_SIZE] __attribute__ ((section (".bram_buffers")));
#else
static XEmacPss_Bd RxBdSpace[RXBD_CNT];
static XEmacPss_Bd TxBdSpace[TXBD_CNT];
static char RxBuffers[RXBD_CNT * XEMACPSS_RX_BUF_SIZE];
static uchar data_buffer[XEMACPSS_RX_BUF_SIZE];
#endif

#define XEMACPSS_DRIVER_NAME "zynq_gem"

XEmacPss EmacPssInstances[CONFIG_ZYNQ_GEM_COUNT];

/*****************************************************************************/
/*
*	Following are the supporting functions to read and write GEM PHY registers.
*/
static int Xgmac_phy_mgmt_idle(XEmacPss * EmacPssInstancePtr)
{
	return ((XEmacPss_ReadReg
		 (EmacPssInstancePtr->Config.BaseAddress, XEMACPSS_NWSR_OFFSET)
		 & XEMACPSS_NWSR_MDIOIDLE_MASK) == XEMACPSS_NWSR_MDIOIDLE_MASK);
}

#if defined(CONFIG_MII) || defined(CONFIG_CMD_MII)
static int Xgmac_mii_read(const char *devname, unsigned char addr,
		unsigned char reg, unsigned short *value)
{
	struct eth_device *dev;
	XEmacPss *EmacPssInstancePtr;

	dev = eth_get_dev_by_name(devname);
	EmacPssInstancePtr = (XEmacPss *)dev->priv;

	phy_spinwait(EmacPssInstancePtr);
	XEmacPss_PhyRead(EmacPssInstancePtr, addr, reg, value);
	phy_spinwait(EmacPssInstancePtr);
	return 0;
}

static int Xgmac_mii_write(const char *devname, unsigned char addr,
		unsigned char reg, unsigned short value)
{
	struct eth_device *dev;
	XEmacPss *EmacPssInstancePtr;

	dev = eth_get_dev_by_name(devname);
	EmacPssInstancePtr = (XEmacPss *)dev->priv;

	phy_spinwait(EmacPssInstancePtr);
	XEmacPss_PhyWrite(EmacPssInstancePtr, addr, reg, value);
	phy_spinwait(EmacPssInstancePtr);
	return 0;
}
#endif

static u32 phy_rd(XEmacPss * e, u32 a)
{
	u16 PhyData;

	phy_spinwait(&EmacPssInstances[e->Config.MiiGem]);
	XEmacPss_PhyRead(&EmacPssInstances[e->Config.MiiGem],
		e->Config.PhyAddress, a, &PhyData);
	phy_spinwait(&EmacPssInstances[e->Config.MiiGem]);
	return PhyData;
}

static void phy_wr(XEmacPss * e, u32 a, u32 v)
{
	phy_spinwait(&EmacPssInstances[e->Config.MiiGem]);
	XEmacPss_PhyWrite(&EmacPssInstances[e->Config.MiiGem],
		e->Config.PhyAddress, a, v);
	phy_spinwait(&EmacPssInstances[e->Config.MiiGem]);
}

static void phy_rst(XEmacPss * e)
{
	int tmp;

	puts("Resetting PHY...\n");
	tmp = phy_rd(e, MII_BMCR);
	tmp |= BMCR_RESET;
	phy_wr(e, MII_BMCR, tmp);

	while (phy_rd(e, MII_BMCR) & BMCR_RESET) {
		udelay(10000);
		tmp++;
		if (tmp > 1000) { /* stalled if reset unfinished after 10 seconds */
			puts("***Error: Reset stalled...\n");
			return;
		}
	}
	puts("\nPHY reset complete.\n");
}

static void Out32(u32 OutAddress, u32 Value)
{
	*(volatile u32 *) OutAddress = Value;
	dmb();
}

/*****************************************************************************/

int Xgmac_one_time_init(void)
{
	int i;
	int tmp;
	int Status;
	XEmacPss_Config *Config;
	XEmacPss *EmacPssInstancePtr;
	XEmacPss_Bd BdTemplate;

	for (i = 0; i < CONFIG_ZYNQ_GEM_COUNT; ++i) {
		Config = XEmacPss_LookupConfig(i);
		EmacPssInstancePtr = &EmacPssInstances[i];

		Status =
		    XEmacPss_CfgInitialize(EmacPssInstancePtr, Config,
					   Config->BaseAddress);
		if (Status != 0) {
			puts("Error in initialize");
			return -1;
		}

		/*
		 * Setup RxBD space.
		 */

		if (Xgmac_init_rxq(EmacPssInstancePtr, &RxBdSpace, RXBD_CNT)) {
			puts("Xgmac_init_rxq failed!\n");
			return -1;
		}

		/*
		 * Create the RxBD ring
		 */
		tmp =
		    Xgmac_make_rxbuff_mem(EmacPssInstancePtr, &RxBuffers,
					  sizeof(RxBuffers));
		if (tmp == 0 || tmp == -1) {
			printf("Xgmac_make_rxbuff_mem failed! (%i)\n", tmp);
			return -1;
		}

		/*
		 * Setup TxBD space.
		 */

		XEmacPss_BdClear(&BdTemplate);
		XEmacPss_BdSetStatus(&BdTemplate, XEMACPSS_TXBUF_USED_MASK);

		/*
		 * Create the TxBD ring
		 */
		Status = XEmacPss_BdRingCreate(
			&(XEmacPss_GetTxRing(EmacPssInstancePtr)),
			(u32)&TxBdSpace, (u32)&TxBdSpace,
			XEMACPSS_BD_ALIGNMENT, TXBD_CNT);
		if (Status != 0) {
			puts("Error setting up TxBD space, BdRingCreate");
			return -1;
		}

		Status = XEmacPss_BdRingClone(
			&(XEmacPss_GetTxRing(EmacPssInstancePtr)),
			&BdTemplate, XEMACPSS_SEND);
		if (Status != 0) {
			puts("Error setting up TxBD space, BdRingClone");
			return -1;
		}

		XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
				  XEMACPSS_TXQBASE_OFFSET,
				  EmacPssInstancePtr->TxBdRing.BaseBdAddr);

		/************************* MAC Setup *************************/
		tmp = (3 << 18); /* MDC clock division (48 for up to 120MHz) */
		tmp |= (1 << 17); /* set for FCS removal */
		tmp |= (1 << 10); /* enable gigabit */
		tmp |= (1 << 4); /* copy all frames */
		tmp |= (1 << 1); /* enable full duplex */

		XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
				  XEMACPSS_NWCFG_OFFSET, tmp);

		/* MDIO enable */
		tmp =
		    XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
				     XEMACPSS_NWCTRL_OFFSET);
		tmp |= XEMACPSS_NWCTRL_MDEN_MASK;
		XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
				  XEMACPSS_NWCTRL_OFFSET, tmp);
	}
	return 0;
}

int Xgmac_init(struct eth_device *dev, bd_t * bis)
{
	int tmp;
	int link_speed;
	XEmacPss *EmacPssInstancePtr = (XEmacPss *)dev->priv;
	u32 slcr_gem_rx_clk;
	u32 slcr_gem_tx_clk;
	u32 slcr_gem_emio_clk = 0;

	if (EmacPssInstancePtr->Initialized)
		return 1;

	/*
	 * Setup the ethernet.
	 */
	printf("Trying to set up link on %s\n", dev->name);

	/* Configure DMA */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_DMACR_OFFSET, 0x00180704);

	/* Disable all the MAC Interrupts */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_IDR_OFFSET, 0xFFFFFFFF);

	/* Rx and Tx enable */
	tmp =
	    XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
			     XEMACPSS_NWCTRL_OFFSET);
	tmp |= XEMACPSS_NWCTRL_RXEN_MASK | XEMACPSS_NWCTRL_TXEN_MASK;
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_NWCTRL_OFFSET, tmp);

	/*************************** PHY Setup ***************************/

#ifdef CONFIG_EP107
	phy_wr(EmacPssInstancePtr, 22, 0);	/* page 0 */
#endif

	tmp = phy_rd(EmacPssInstancePtr, MII_PHYSID1);
	printf("Phy ID: %04X", tmp);
	tmp = phy_rd(EmacPssInstancePtr, MII_PHYSID2);
	printf("%04X\n", tmp);

#ifdef CONFIG_EP107
	/* Auto-negotiation advertisement register */
	tmp = phy_rd(EmacPssInstancePtr, MII_ADVERTISE);
	tmp |= ADVERTISE_PAUSE_ASYM;	/* asymmetric pause */
	tmp |= ADVERTISE_PAUSE_CAP;	/* MAC pause implemented */
	phy_wr(EmacPssInstancePtr, MII_ADVERTISE, tmp);
#endif

#ifdef CONFIG_EP107
	/* Extended PHY specific control register */
	tmp = phy_rd(EmacPssInstancePtr, 20);
	tmp |= (7 << 9);	/* max number of gigabit attempts */
	tmp |= (1 << 8);	/* enable downshift */
	tmp |= (1 << 7);	/* RGMII receive timing internally delayed */
	tmp |= (1 << 1);	/* RGMII transmit clock internally delayed */
	phy_wr(EmacPssInstancePtr, 20, tmp);
#endif

	/* Control register */
	tmp = phy_rd(EmacPssInstancePtr, MII_BMCR);
	tmp |= BMCR_ANENABLE;	/* auto-negotiation enable */
#ifdef CONFIG_EP107
	tmp |= BMCR_FULLDPLX;	/* enable full duplex */
#endif
	phy_wr(EmacPssInstancePtr, MII_BMCR, tmp);

#ifdef CONFIG_EP107
	/***** Try to establish a link at the highest speed possible  *****/
	/* CR-659040 */
	Xgmac_set_eth_advertise(EmacPssInstancePtr, 1000);
#endif
	phy_rst(EmacPssInstancePtr);

	/* Attempt auto-negotiation */
	puts("Waiting for PHY to complete auto-negotiation...\n");
	tmp = 0; /* delay counter */
	while (!(phy_rd(EmacPssInstancePtr, MII_BMSR) & BMSR_ANEGCOMPLETE)) {
		udelay(10000);
		tmp++;
		if (tmp > 1000) { /* stalled if no link after 10 seconds */
			puts("***Error: Auto-negotiation stalled...\n");
			return -1;
		}
	}

	/* Check if the link is up */
	tmp = phy_rd(EmacPssInstancePtr, MII_BMSR);
	/* Read twice to ensure valid current link status */
	tmp = phy_rd(EmacPssInstancePtr, MII_BMSR);
	if (tmp & BMSR_LSTATUS) {
		/* Link is up */
	} else {
		puts("***Error: Link is not up.\n");
		return -1;
	}

	/********************** Determine link speed **********************/

	/* Same method used by Linux kernel generic PHY driver: mask our
	   capabilities with the link partner capabilities and pick the
	   highest common denominator. */

	/* 1000BASE-T Status */
	tmp = phy_rd(EmacPssInstancePtr, MII_STAT1000);
	/* 1000BASE-T Control */
	tmp &= (phy_rd(EmacPssInstancePtr, MII_CTRL1000) << 2);
	if ((LPA_1000FULL | LPA_1000HALF) & tmp)
		link_speed = 1000;
	else {
		/* Link Partner Ability */
		tmp = phy_rd(EmacPssInstancePtr, MII_LPA);
		/* Auto-Negotiation Advertisement */
		tmp &= phy_rd(EmacPssInstancePtr, MII_ADVERTISE);

		if ((LPA_100FULL | LPA_100HALF) & tmp)
			link_speed = 100;
		else
			link_speed = 10;
	}

	/*************************** MAC Setup ***************************/
	tmp = XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_NWCFG_OFFSET);
	if (link_speed == 10)
		tmp &= ~(0x1);		/* enable 10Mbps */
	else
		tmp |= 0x1;		/* enable 100Mbps */
	if (link_speed == 1000)
		tmp |= 0x400;		/* enable 1000Mbps */
	else
		tmp &= ~(0x400);	/* disable gigabit */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_NWCFG_OFFSET, tmp);

	if (EmacPssInstancePtr->Config.BaseAddress == XPSS_GEM0_BASEADDR) {
		slcr_gem_rx_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM0_RCLK_CTRL;
#ifdef CONFIG_ZYNQ_GEM0_EMIO
		slcr_gem_tx_clk =
			XPSS_SYS_CTRL_BASEADDR + CONFIG_ZYNQ_GEM0_FPGA_CLK_REG;
		slcr_gem_emio_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM0_CLK_CTRL;
#else
		slcr_gem_tx_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM0_CLK_CTRL;
#endif
	} else {
		slcr_gem_rx_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM1_RCLK_CTRL;
#ifdef CONFIG_ZYNQ_GEM1_EMIO
		slcr_gem_tx_clk =
			XPSS_SYS_CTRL_BASEADDR + CONFIG_ZYNQ_GEM1_FPGA_CLK_REG;
		slcr_gem_emio_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM1_CLK_CTRL;
#else
		slcr_gem_tx_clk =
			XPSS_SYS_CTRL_BASEADDR + XPSS_SLCR_GEM1_CLK_CTRL;
#endif
	}

	/************************* GEM0_CLK Setup *************************/
	/* SLCR unlock */
	Out32(XPSS_SYS_CTRL_BASEADDR | XPSS_SLCR_UNLOCK, XPSS_SLCR_UNLOCK_KEY);

	/* Configure Rx and Tx clock control */
	if (slcr_gem_emio_clk != 0) {
		Out32(slcr_gem_rx_clk, XPSS_SLCR_GEMn_RCLK_CTRL_EMIO);
		Out32(slcr_gem_emio_clk, XPSS_SLCR_GEMn_CLK_CTRL_EMIO);
	} else
		Out32(slcr_gem_rx_clk, XPSS_SLCR_GEMn_RCLK_CTRL_MIO);

	/* Set divisors for appropriate Tx frequency */
#ifdef CONFIG_EP107
	if (link_speed == 1000)		/* 125MHz */
		Out32(slcr_gem_tx_clk,
			((1 << 20) | (48 << 8) | (1 << 4) | (1 << 0)));
	else if (link_speed == 100)	/* 25 MHz */
		Out32(slcr_gem_tx_clk,
			((1 << 20) | (48 << 8) | (0 << 4) | (1 << 0)));
	else				/* 2.5 MHz */
		Out32(slcr_gem_tx_clk,
			((1 << 20) | (48 << 8) | (3 << 4) | (1 << 0)));
#else
	/* Assumes IO PLL clock is 1000 MHz */
	if (link_speed == 1000)		/* 125MHz */
		Out32(slcr_gem_tx_clk,
			((1 << 20) | (8 << 8) | (0 << 4) | (1 << 0)));
	else if (link_speed == 100)	/* 25 MHz */
		Out32(slcr_gem_tx_clk,
			((1 << 20) | (40 << 8) | (0 << 4) | (1 << 0)));
	else				/* 2.5 MHz */
		Out32(slcr_gem_tx_clk,
			((10 << 20) | (40 << 8) | (0 << 4) | (1 << 0)));
#endif

	/* SLCR lock */
	Out32(XPSS_SYS_CTRL_BASEADDR | XPSS_SLCR_LOCK, XPSS_SLCR_LOCK_KEY);

	printf("Link is now at %dMbps!\n", link_speed);

	EmacPssInstancePtr->Initialized = 1;
	return 0;
}

void Xgmac_halt(struct eth_device *dev)
{
	return;
}

int Xgmac_send(struct eth_device *dev, void *packet, int length)
{
	volatile int Status;
	XEmacPss_Bd *BdPtr;
	XEmacPss *EmacPssInstancePtr = (XEmacPss *)dev->priv;

	if (!EmacPssInstancePtr->Initialized) {
		puts("Error GMAC not initialized");
		return 0;
	}

	Status =
	    XEmacPss_BdRingAlloc(&(XEmacPss_GetTxRing(EmacPssInstancePtr)), 1,
				 &BdPtr);
	if (Status != 0) {
		puts("Error allocating TxBD");
		return 0;
	}

	/*
	 * Setup TxBD
	 */
	XEmacPss_BdSetAddressTx(BdPtr, (u32)packet);
	XEmacPss_BdSetLength(BdPtr, length);
	XEmacPss_BdClearTxUsed(BdPtr);
	XEmacPss_BdSetLast(BdPtr);

	/*
	 * Enqueue to HW
	 */
	Status =
	    XEmacPss_BdRingToHw(&(XEmacPss_GetTxRing(EmacPssInstancePtr)), 1,
				BdPtr);
	if (Status != 0) {
		puts("Error committing TxBD to HW");
		return 0;
	}

	/* Start transmit */
	XEmacPss_Transmit(EmacPssInstancePtr);

	/* Read the status register to know if the packet has been Transmitted. */
	Status = XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
			     XEMACPSS_TXSR_OFFSET);
	if (Status &
	    (XEMACPSS_TXSR_HRESPNOK_MASK | XEMACPSS_TXSR_URUN_MASK |
	     XEMACPSS_TXSR_BUFEXH_MASK)) {
		printf("Something has gone wrong here!? Status is 0x%x.\n",
		       Status);
	}

	if (Status & XEMACPSS_TXSR_TXCOMPL_MASK) {

//		printf("tx packet sent\n");

		/*
		 * Now that the frame has been sent, post process our TxBDs.
		 */
		if (XEmacPss_BdRingFromHwTx(
		    &(XEmacPss_GetTxRing(EmacPssInstancePtr)),
		    1, &BdPtr) == 0) {
			puts("TxBDs were not ready for post processing");
			return 0;
		}

		/*
		 * Free the TxBD.
		 */
		Status = XEmacPss_BdRingFree(
			&(XEmacPss_GetTxRing(EmacPssInstancePtr)), 1, BdPtr);
		if (Status != 0) {
			puts("Error freeing up TxBDs");
			return 0;
		}
	}
	/* Clear Tx status register before leaving . */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_TXSR_OFFSET, Status);
	return 1;

}

int Xgmac_rx(struct eth_device *dev)
{
	u32 status, retval;
	XEmacPss *EmacPssInstancePtr = (XEmacPss *)dev->priv;

	status =
	    XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
			     XEMACPSS_RXSR_OFFSET);
	if (status & XEMACPSS_RXSR_FRAMERX_MASK) {

//		printf("rx packet received\n");
	
		do {
			retval = Xgmac_process_rx(EmacPssInstancePtr);
		} while (retval == 0) ;
	}

	/* Clear interrupt status.
	 */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
	                  XEMACPSS_RXSR_OFFSET, status);
	
	return 1;
}

static int Xgmac_write_hwaddr(struct eth_device *dev)
{
	/* Initialize the first MAC filter with our address */
	XEmacPss_SetMacAddress((XEmacPss *)dev->priv, dev->enetaddr, 1);

	return 0;
}

int zynq_gem_get_phyaddr(const char *devname)
{
	struct eth_device *dev;
	XEmacPss *EmacPssInstancePtr;

	dev = eth_get_dev_by_name(devname);
	EmacPssInstancePtr = (XEmacPss *)dev->priv;

	return EmacPssInstancePtr->Config.PhyAddress;
}

int zynq_gem_initialize(bd_t *bis)
{
	int i;
	struct eth_device *dev;

	if (Xgmac_one_time_init() < 0) {
		printf("zynq_gem init failed!");
		return -1;
	}

	for (i = 0; i < CONFIG_ZYNQ_GEM_COUNT; ++i) {
		dev = malloc(sizeof(*dev));
		if (dev == NULL)
			return 1;

		memset(dev, 0, sizeof(*dev));
		sprintf(dev->name, XEMACPSS_DRIVER_NAME "%d", i);

		dev->iobase = EmacPssInstances[i].Config.BaseAddress;
		dev->priv = &EmacPssInstances[i];
		dev->init = Xgmac_init;
		dev->halt = Xgmac_halt;
		dev->send = Xgmac_send;
		dev->recv = Xgmac_rx;
		dev->write_hwaddr = Xgmac_write_hwaddr;

		eth_register(dev);

#if defined(CONFIG_MII) || defined(CONFIG_CMD_MII)
		if (EmacPssInstances[i].Config.CreateMii)
			miiphy_register(dev->name,
				Xgmac_mii_read, Xgmac_mii_write);
#endif
	}
	return 0;
}

/*=============================================================================
 *
 * Xgmac_process_rx- process the next incoming packet
 *
 * return's 0 if OK, -1 on error
 */
int Xgmac_process_rx(XEmacPss * EmacPssInstancePtr)
{
	uchar *buffer = data_buffer;
	u32 rx_status, hwbuf;
	int frame_len;
	u32 *bd_addr;

    bd_addr = (u32 *) & EmacPssInstancePtr->RxBdRing.
	    RxBD_start[EmacPssInstancePtr->RxBdRing.RxBD_current];

	rx_status = XEmacPss_BdRead((bd_addr), XEMACPSS_BD_ADDR_OFFSET);
	if (! (rx_status & XEMACPSS_RXBUF_NEW_MASK)) {
		return (-1);
	}

	rx_status = XEmacPss_BdIsRxSOF(bd_addr);
	if (!rx_status) {
		printf("GEM: SOF not set for last buffer received!\n");
		return (-1);
	}
	rx_status = XEmacPss_BdIsRxEOF(bd_addr);
	if (!rx_status) {
		printf("GEM: EOF not set for last buffer received!\n");
		return (-1);
	}

	frame_len = XEmacPss_BdGetLength(bd_addr);
	if (frame_len == 0) {
		printf("GEM: Hardware reported 0 length frame!\n");
		return (-1);
	}

	hwbuf = (u32) (*bd_addr & XEMACPSS_RXBUF_ADD_MASK);
	if (hwbuf == (u32) NULL) {
		printf("GEM: Error swapping out buffer!\n");
		return (-1);
	}
	memcpy(buffer, (void *)hwbuf, frame_len);
	Xgmac_next_rx_buf(EmacPssInstancePtr);
	NetReceive(buffer, frame_len);

	return (0);
}

int Xgmac_init_rxq(XEmacPss * EmacPssInstancePtr, void *bd_start, int num_elem)
{
	XEmacPss_BdRing *r;
	int loop = 0;

	if ((num_elem <= 0) || (num_elem > RXBD_CNT)) {
		return (-1);
	}

	for (; loop < 2 * (num_elem);) {
		*(((u32 *) bd_start) + loop) = 0x00000000;
		*(((u32 *) bd_start) + loop + 1) = 0xF0000000;
		loop += 2;
	}

	r = & EmacPssInstancePtr->RxBdRing;
	r->RxBD_start = (XEmacPss_Bd *) bd_start;
	r->Length = num_elem;
	r->RxBD_current = 0;
	r->RxBD_end = 0;
	r->Rx_first_buf = 0;

	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_RXQBASE_OFFSET, (u32) bd_start);

	return 0;
}

int Xgmac_make_rxbuff_mem(XEmacPss * EmacPssInstancePtr, void *rx_buf_start,
			  u32 rx_buffsize)
{
	XEmacPss_BdRing *r;
	int num_bufs;
	int assigned_bufs;
	int i;
	u32 *bd_addr;

	if ((EmacPssInstancePtr == NULL) || (rx_buf_start == NULL)) {
		return (-1);
	}

	r = & EmacPssInstancePtr->RxBdRing;

	assigned_bufs = 0;

	if ((num_bufs = rx_buffsize / XEMACPSS_RX_BUF_SIZE) == 0) {
		return 0;
	}
	for (i = 0; i < num_bufs; i++) {
		if (r->RxBD_end < r->Length) {
			memset((char *)(rx_buf_start +
					(i * XEMACPSS_RX_BUF_SIZE)), 0, XEMACPSS_RX_BUF_SIZE);

			bd_addr = (u32 *) & r->RxBD_start[r->RxBD_end];

			XEmacPss_BdSetAddressRx(bd_addr,
						(u32) (((char *)
							rx_buf_start) + (i * XEMACPSS_RX_BUF_SIZE)));

			r->RxBD_end++;
			assigned_bufs++;
		} else {
			return assigned_bufs;
		}
	}
	bd_addr = (u32 *) & r->RxBD_start[r->RxBD_end - 1];
	XEmacPss_BdSetRxWrap(bd_addr);

	return assigned_bufs;
}

int Xgmac_next_rx_buf(XEmacPss * EmacPssInstancePtr)
{
	XEmacPss_BdRing *r;
	u32 prev_stat = 0;
	u32 *bd_addr = NULL;

	if (EmacPssInstancePtr == NULL) {
		printf
		    ("\ngem_clr_rx_buf with EmacPssInstancePtr as !!NULL!! \n");
		return -1;
	}

	r = & EmacPssInstancePtr->RxBdRing;

	bd_addr = (u32 *) & r->RxBD_start[r->RxBD_current];
	prev_stat = XEmacPss_BdIsRxSOF(bd_addr);
	if (prev_stat) {
		r->Rx_first_buf = r->RxBD_current;
	} else {
		XEmacPss_BdClearRxNew(bd_addr);
		XIo_Out32((u32) (bd_addr + 1), 0xF0000000);
	}

	if (XEmacPss_BdIsRxEOF(bd_addr)) {
		bd_addr = (u32 *) & r->RxBD_start[r->Rx_first_buf];
		XEmacPss_BdClearRxNew(bd_addr);
		XIo_Out32((u32) (bd_addr + 1), 0xF0000000);
	}

	if ((++r->RxBD_current) > r->Length - 1) {
		r->RxBD_current = 0;
	}

	return 0;
}

#ifdef CONFIG_EP107
void Xgmac_set_eth_advertise(XEmacPss *EmacPssInstancePtr, int link_speed)
{
	int tmp;

	/* MAC setup */
	tmp = XEmacPss_ReadReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_NWCFG_OFFSET);
	if (link_speed == 10)
		tmp &= ~(1 << 0);	/* enable 10Mbps */
	else if (link_speed == 100)
		tmp |= (1 << 0);	/* enable 100Mbps */
	XEmacPss_WriteReg(EmacPssInstancePtr->Config.BaseAddress,
			  XEMACPSS_NWCFG_OFFSET, tmp);

	phy_wr(EmacPssInstancePtr, 22, 0);	/* page 0 */

	/* Auto-negotiation advertisement register */
	tmp = phy_rd(EmacPssInstancePtr, 4);
	if (link_speed >= 100) {
		tmp |= (1 << 8);	/* advertise 100Mbps F */
		tmp |= (1 << 7);	/* advertise 100Mbps H */
	} else {
		tmp &= ~(1 << 8);	/* advertise 100Mbps F */
		tmp &= ~(1 << 7);	/* advertise 100Mbps H */
	}
	if (link_speed >= 10) {
		tmp |= (1 << 6);	/* advertise 10Mbps F */
		tmp |= (1 << 5);	/* advertise 10Mbps H */
	} else {
		tmp &= ~(1 << 6);	/* advertise 10Mbps F */
		tmp &= ~(1 << 5);	/* advertise 10Mbps H */
	}
	phy_wr(EmacPssInstancePtr, 4, tmp);

	/* 1000BASE-T control register */
	tmp = phy_rd(EmacPssInstancePtr, 9);
	if (link_speed == 1000) {
		tmp |= (1 << 9);	/* advertise 1000Mbps F */
		tmp |= (1 << 8);	/* advertise 1000Mbps H */
	} else {
		tmp &= ~(1 << 9);	/* advertise 1000Mbps F */
		tmp &= ~(1 << 8);	/* advertise 1000Mbps H */
	}
	phy_wr(EmacPssInstancePtr, 9, tmp);

}
#endif
