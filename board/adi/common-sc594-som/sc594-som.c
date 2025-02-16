/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 */

#include <common.h>
#include <netdev.h>
#include <phy.h>
#include <asm/io.h>
#include <asm/mach-types.h>
#include <asm/arch-adi/sc5xx/sc5xx.h>
#include <linux/delay.h>
#include <watchdog.h>

#include <asm/arch-adi/sc5xx/sc59x-shared.h>
#include <asm/arch-adi/sc5xx/soc.h>

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

#ifdef CONFIG_HW_WATCHDOG
	hw_watchdog_init();
#endif

#ifdef CONFIG_CADENCE_QSPI
	//adi_multiplex_ospi();
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

void adi_eth_init(void)
{
#if defined(CONFIG_ADI_CARRIER_SOMCRR_EZKIT) || defined(CONFIG_ADI_CARRIER_SOMCRR_EZLITE)
	adi_enable_ethernet_softconfig();
	mdelay(20);
	adi_disable_ethernet_softconfig();
	mdelay(90);
	adi_enable_ethernet_softconfig();
	mdelay(20);
#endif

	// select RGMII
	writel((readl(REG_PADS0_PCFG0) | 0xc), REG_PADS0_PCFG0);
}

int board_phy_config(struct phy_device *phydev)
{
#ifdef CONFIG_ADI_CARRIER_SOMCRR_EZKIT
	fixup_dp83867_phy(phydev);
#endif

	return 0;
}

int board_init(void)
{
#ifdef CONFIG_ADI_CARRIER_SOMCRR_EZLITE
	gd->bd->bi_arch_number = MACH_TYPE_SC594_SOM_EZLITE;
#else
	gd->bd->bi_arch_number = MACH_TYPE_SC594_SOM_EZKIT;
#endif
	/* boot param addr */
	gd->bd->bi_boot_params = CONFIG_SYS_SDRAM_BASE + (0x100);

	adi_eth_init();

//	set_spu_securep_msec(55, 1);
//	set_spu_securep_msec(56, 1);
//	set_spu_securep_msec(58, 1);
//	set_spu_securep_msec(153, 1);

	return 0;
}

