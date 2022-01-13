/*
 * U-boot - main board file
 *
 * Copyright (c) 2013-2014 Analog Devices Inc.
 *
 * Licensed under the GPL-2 or later.
 */

#include <common.h>
#include <netdev.h>
#include <phy.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <asm/mach-types.h>
#include <asm/arch/portmux.h>
#include <asm/arch/sc59x_64.h>
#include <asm/arch-sc59x-64/dwmmc.h>
#include <linux/delay.h>
#include <watchdog.h>
#include "soft_switch.h"
#include <asm/armv8/mmu.h>
#include "sc598-som-ezkit-shared.h"

static struct mm_region sc598_mem_map[] = {
	{
		/* Peripherals */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x80000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* DDR */
		.virt = 0x80000000UL,
		.phys = 0x80000000UL,
		.size = 0x40000000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_INNER_SHARE
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = sc598_mem_map;

extern char __bss_start, __bss_end;
static void bss_clear(void)
{
	u32 *to = (void *)&__bss_start;
	int i, sz;

	sz = &__bss_end - &__bss_start;
	for (i = 0; i < sz; i += 4)
		*to++ = 0;
}

DECLARE_GLOBAL_DATA_PTR;
int board_early_init_f(void)
{
	bss_clear();

//#ifdef CONFIG_HW_WATCHDOG
//	hw_watchdog_init();
//#endif

#ifdef CONFIG_USB_DWC2
	static const unsigned short pins_usbc0[] = P_USBC0;
	if (peripheral_request_list(pins_usbc0, "usbc0")){
		printf("Unable to pinmux OSPI0\r\n");
	}
	gpio_request(GPIO_PG11, "usb_reset");
	gpio_direction_output(GPIO_PG11, 1);
#endif

	return 0;
}

void set_spu_securep_msec(int n, bool msec)
{
	void __iomem *p = (void __iomem *)(REG_SPU0_CTL + 0xA00 + 4 * n);
	u32 securep = readl(p);

	if (msec)
		writel(securep | 0x2, p);
	else
		writel(securep & ~0x2, p);
}

/* miscellaneous platform dependent initialisations */
int misc_init_r(void)
{
//	set_spu_securep_msec(55, 1);
//	set_spu_securep_msec(56, 1);
//	set_spu_securep_msec(58, 1);
//	set_spu_securep_msec(153, 1);

return 0;

}

unsigned long flash_init(void)
{

#if 0
	/*  Enable bank 1 of smc because AMS1 is connected to enable of SRAM */
	*pREG_SMC0_GCTL = 0x10;
	*pREG_SMC0_B1CTL = 0x00000001;
	*pREG_SMC0_B1TIM = 0x05220522;
#endif
	return 0;
}

int dram_init(void)
{
	gd->ram_size = CONFIG_SYS_SDRAM_SIZE;
	return 0;
}

void s_init(void)
{
}

#ifdef CONFIG_DESIGNWARE_ETH
int board_eth_init(struct bd_info *bis)
{
	int ret = 0;

#if ADI_HAVE_CARRIER == 1
	if (CONFIG_DW_PORTS >= 1) {
		gpio_request(GPIO_PG12, "emac0_phy_pwdn");
		gpio_direction_output(GPIO_PG12, 1);

		adi_enable_ethernet_softconfig();
		mdelay(20);
		adi_disable_ethernet_softconfig();
		mdelay(90);
		adi_enable_ethernet_softconfig();
		mdelay(20);

		// select RGMII, little endian for both ports
		writel((readl(REG_PADS0_PCFG0) | 0xc), REG_PADS0_PCFG0);
		writel(readl(REG_PADS0_PCFG0) & ~(1 << 19), REG_PADS0_PCFG0);
		writel(readl(REG_PADS0_PCFG0) & ~(1 << 20), REG_PADS0_PCFG0);

		static const unsigned short pins[] = P_RGMII0;
		if (!peripheral_request_list(pins, "emac0"))
			ret += dwmac4_initialize(REG_EMAC0_MACCFG,
					PHY_INTERFACE_MODE_RGMII);
	}

	if (CONFIG_DW_PORTS >= 2) {
		static const unsigned short pins[] = P_RMII1;
		if (!peripheral_request_list(pins, "emac1"))
			ret += dwmac4_initialize(REG_EMAC1_MACCFG, 0);
	}
#endif

	return ret;
}

int board_phy_config(struct phy_device *phydev)
{
	int  phy_data = 0;

	if (CONFIG_DW_PORTS & 1) {
#ifdef CONFIG_PHY_TI
		phy_data = phy_read(phydev, MDIO_DEVAD_NONE, 0x32);
		phy_write(phydev, MDIO_DEVAD_NONE, 0x32, (1 << 7) | phy_data);
		int cfg3 = 0;
		#define MII_DP83867_CFG3    (0x1e)
		/*
		 * Pin INT/PWDN on DP83867 should be configured as an Interrupt Output
		 * instead of a Power-Down Input on ADI SC5XX boards in order to
		 * prevent the signal interference from other peripherals during they
		 * are running at the same time.
		 */
		cfg3 = phy_read(phydev, MDIO_DEVAD_NONE, MII_DP83867_CFG3);
		cfg3 |= (1 << 7);
		phy_write(phydev, MDIO_DEVAD_NONE, MII_DP83867_CFG3, cfg3);
#endif
	}

	if (CONFIG_DW_PORTS & 2)
		phy_write(phydev, MDIO_DEVAD_NONE, 0x11, 3);

	if (phydev->drv->config)
		phydev->drv->config(phydev);

	return 0;
}
#endif

int board_init(void)
{
	void __iomem *timer = 0x310ae000;
	void __iomem *spu_securec;

	gd->bd->bi_arch_number = MACH_TYPE_SC594_SOM_EZKIT;
	/* boot param addr */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + (0x100);

	// enable coresight timestamp generator with default settings
	writel(1, timer);

	// disable secure-only access to sharc cores so we can load from EL1
	spu_securec = REG_SPU0_SECUREC0;
	writel(0, spu_securec);
	spu_securec = REG_SPU0_SECUREC1;
	writel(0, spu_securec);
	spu_securec = REG_SPU0_SECUREC2;
	writel(0, spu_securec);

#ifdef CONFIG_MMC_SDHCI_ADI
	adi_mmc_init();
#endif

#if ADI_HAVE_CARRIER == 1
#ifdef CONFIG_SOFT_SWITCH
	adi_initialize_soft_switches();
#else
	return 0;
#endif
#endif

	return 0;
}

