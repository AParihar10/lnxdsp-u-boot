// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * (C) Copyright 2022 - Analog Devices, Inc.
 *
 * Written and/or maintained by Timesys Corporation
 *
 * Converted to driver model by Nathan Barrett-Morrison
 *
 * Contact: Nathan Barrett-Morrison <nathan.morrison@timesys.com>
 * Contact: Greg Malysa <greg.malysa@timesys.com>
 *
 */

#include <common.h>
#include <log.h>
#include <malloc.h>
#include <spi.h>
#include <asm/io.h>
#include <asm/gpio.h>
#include <dm.h>
#include <errno.h>
#include <fdtdec.h>
#include <clk.h>

#include "adi_spi3.h"

DECLARE_GLOBAL_DATA_PTR;

#define DEBUG

#define MAX_SPI_NUM	2
#define SPI_IDLE_VAL	0xff

int adi_spi_cs_valid(unsigned int bus, unsigned int cs)
{
	if (bus > MAX_SPI_NUM)
		return 0;
	return cs >= 1 && cs <= (MAX_CTRL_CS - 5);
}

/* @todo gpio cs is not currently supported */
int cs_is_valid(unsigned int bus, unsigned int cs)
{
	if (is_gpio_cs(cs)){
		return 0;
	}
	else{
		return adi_spi_cs_valid(bus, cs);
	}
}

static int adi_spi_cs_info(struct udevice *bus, uint cs,
			   struct spi_cs_info *info)
{
	struct adi_spi_priv *priv = dev_get_priv(bus);

	if (cs > (MAX_CTRL_CS - 5)) {
		printf("invalid chipselect %u\n", cs);
		return -EINVAL;
	}

	return 0;
}


static int adi_spi_of_to_plat(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	const void *blob = gd->fdt_blob;
	int node = dev_of_offset(bus);
	int subnode;
	fdt_addr_t addr;

	plat->max_hz = fdtdec_get_int(blob, node, "spi-max-frequency", 500000);
	plat->bus_num = fdtdec_get_int(blob, node, "bus-num", 0);
	addr = dev_read_addr(bus);

	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	plat->regs = (struct adi_spi_regs *)addr;

	/* All other parameters are embedded in the child node */
	subnode = fdt_first_subnode(blob, node);
	if (subnode < 0) {
		printf("Error: subnode with SPI flash config missing!\n");
		return -ENODEV;
	}

	/* Read other parameters from DT */
	plat->cs_num = fdtdec_get_int(blob, subnode, "reg", 0);

	return 0;
}

static int adi_spi_probe(struct udevice *bus)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	struct adi_spi_priv *priv = dev_get_priv(bus);
	int cs;

	priv->bus_num = plat->bus_num;
	priv->regs = plat->regs;
	priv->cs_num = plat->cs_num;

	if (!cs_is_valid(priv->bus_num, priv->cs_num)) {
		printf("Invalid chip select\r\n");
		return -ENODEV;
	}
	if (is_gpio_cs(priv->cs_num)) {
		cs = gpio_cs(priv->cs_num);
		gpio_request(cs, "adi-spi3");
		gpio_direction_output(cs, !priv->cs_pol);
	}

	if (IS_ENABLED(CONFIG_SPI_FLASH_MMAP))
		priv->memory_map = (void *)CONFIG_SPI_MM_BASE;

	writel(0x0, &plat->regs->control);
	writel(0x0, &plat->regs->rx_control);
	writel(0x0, &plat->regs->tx_control);

	return 0;
}

static int adi_spi_remove(struct udevice *dev)
{
	return -ENODEV;
}

static int adi_spi_claim_bus(struct udevice *dev)
{
	struct adi_spi_priv *priv;
	struct udevice *bus = dev->parent;

	priv = dev_get_priv(bus);

	debug("%s: control:%i clock:%i\n",
	      __func__, priv->control, priv->clock);

	writel(priv->control, &priv->regs->control);
	writel(priv->clock, &priv->regs->clock);
	writel(0x0, &priv->regs->delay);

	return 0;
}

static int adi_spi_release_bus(struct udevice *dev)
{
	struct adi_spi_priv *priv;
	struct udevice *bus = dev->parent;

	priv = dev_get_priv(bus);

	debug("%s: control:%i clock:%i\n",
	      __func__, priv->control, priv->clock);

	writel(0x0, &priv->regs->rx_control);
	writel(0x0, &priv->regs->tx_control);
	writel(0x0, &priv->regs->control);

	return 0;
}

void adi_spi_cs_activate(struct adi_spi_priv *priv)
{
	if (is_gpio_cs(priv->cs_num)) {
		unsigned int cs = gpio_cs(priv->cs_num);

		gpio_set_value(cs, priv->cs_pol);
	} else {
		u32 ssel;

		ssel = readl(&priv->regs->ssel);
		ssel |= BIT_SSEL_EN(priv->cs_num);
		if (priv->cs_pol)
			ssel |= BIT_SSEL_VAL(priv->cs_num);
		else
			ssel &= ~BIT_SSEL_VAL(priv->cs_num);
		writel(ssel, &priv->regs->ssel);
	}
}

void adi_spi_cs_deactivate(struct adi_spi_priv *priv)
{
	if (is_gpio_cs(priv->cs_num)) {
		unsigned int cs = gpio_cs(priv->cs_num);

		gpio_set_value(cs, !priv->cs_pol);
		gpio_set_value(cs, 1);
	} else {
		u32 ssel;

		ssel = readl(&priv->regs->ssel);
		if (priv->cs_pol)
			ssel &= ~BIT_SSEL_VAL(priv->cs_num);
		else
			ssel |= BIT_SSEL_VAL(priv->cs_num);
		/* deassert cs */
		writel(ssel, &priv->regs->ssel);
		/* disable cs */
		ssel &= ~BIT_SSEL_EN(priv->cs_num);
		writel(ssel, &priv->regs->ssel);
	}
}

static int adi_spi_pio_xfer(struct adi_spi_priv *priv, const u8 *tx, u8 *rx,
			    uint bytes, unsigned long flags)
{
	u8 value;

	/* Discard invalid rx data and empty rfifo */
	while (!(readl(&priv->regs->status) & SPI_STAT_RFE))
		readl(&priv->regs->rfifo);

	if (flags & SPI_XFER_QUAD) {
		/* switch current SPI transfer to quad SPI mode */
		writel((priv->control & ~SPI_CTL_SOSI) | SPI_CTL_MIO_QUAD,
		       &priv->regs->control);
		/*
		 * Data can only be transferred in one direction in quad SPI
		 * mode, trgger the transfer in respective direction.
		 */
		if (rx) {
			writel(0x0, &priv->regs->tx_control);
			writel(SPI_RXCTL_REN | SPI_RXCTL_RTI,
			       &priv->regs->rx_control);

			while (bytes--) {
				while (readl(&priv->regs->status) &
					SPI_STAT_RFE)
					if (ctrlc())
						return -1;
				value = readl(&priv->regs->rfifo);
				*rx++ = value;
			}
		} else if (tx) {
			writel(0x0, &priv->regs->rx_control);
			writel(SPI_TXCTL_TEN | SPI_TXCTL_TTI,
			       &priv->regs->tx_control);

			while (bytes--) {
				value = *tx++;
				writel(value, &priv->regs->tfifo);
				while (readl(&priv->regs->status) &
					SPI_STAT_TFF)
					if (ctrlc())
						return -1;
			}

			/* Wait till the tfifo is empty */
			while ((readl(&priv->regs->status) & SPI_STAT_TFS) !=
				0x40000)
				if (ctrlc())
					return -1;
		} else {
			return -1;
		}
	} else {
		int spi_idle;

		#if defined(CONFIG_SPI_IDLE_VAL)
			spi_idle = CONFIG_SPI_IDLE_VAL;
		#else
			spi_idle = SPI_IDLE_VAL;
		#endif
		/*
		 * Set current SPI transfer in normal mode and trigger
		 * the bi-direction transfer by tx write operation.
		 */
		writel(priv->control, &priv->regs->control);
		writel(SPI_RXCTL_REN, &priv->regs->rx_control);
		writel(SPI_TXCTL_TEN | SPI_TXCTL_TTI, &priv->regs->tx_control);

		while (bytes--) {
			value = (tx ? *tx++ : spi_idle);
			debug("%s: tx:%x ", __func__, value);
			writel(value, &priv->regs->tfifo);
			while (readl(&priv->regs->status) & SPI_STAT_RFE)
				if (ctrlc())
					return -1;
			value = readl(&priv->regs->rfifo);
			if (rx)
				*rx++ = value;
			debug("rx:%x\n", value);
		}
	}

	return 0;
}

static int adi_spi_xfer(struct udevice *dev, unsigned int bitlen,
			const void *dout, void *din, unsigned long flags)
{
	struct udevice *bus = dev->parent;
	struct adi_spi_priv *priv = dev_get_priv(bus);

	const u8 *tx = dout;
	u8 *rx = din;
	uint bytes = bitlen / 8;
	int ret = 0;

	debug("%s: bus_num:%i cs:%i bitlen:%i bytes:%i flags:%lx\n", __func__,
	      priv->bus_num, priv->cs_num, bitlen, bytes, flags);

#ifdef CONFIG_SPI_FLASH_MMAP
	if (flags & SPI_XFER_MMAP) {
		u32 control = priv->control;
		u32 delay = SPI_DLY_LEADX | SPI_DLY_LAGX | 3;
		u32 ssel = readl(&plat->regs->ssel);

#if defined(CONFIG_SPI_FLASH_STMICRO)
		/* Use Fast read quad IO command in memory mapped mode */
		u32 mmrdh =  3 << SPI_MMRDH_ADRSIZE_OFF;

		mmrdh |= 0xEB | 5 << SPI_MMRDH_DMYSIZE_OFF;
#elif defined(CONFIG_SPI_FLASH_ISSI)
		u32 mmrdh =  4 << SPI_MMRDH_ADRSIZE_OFF;

		mmrdh |= 0xEC | 3 << SPI_MMRDH_DMYSIZE_OFF;
		/* TODO : Temporary fix to enable bit 11 to  set bus width to 4.
		Need to make this part specific */
		mmrdh |= (1 << 11);
#else
		u32 mmrdh =  3 << SPI_MMRDH_ADRSIZE_OFF;
		/* Use Octal word read quad IO command in memory mapped mode */
		mmrdh |= 0xE3 | 1 << SPI_MMRDH_DMYSIZE_OFF;
#endif
		mmrdh |= 1 << SPI_MMRDH_TRIDMY_OFF;
		/* Ask SPI controller to send out address via 4-pin mode */
		mmrdh |= SPI_MMRDH_ADRPINS | SPI_MMRDH_MERGE;

		control |= SPI_CTL_MMSE | SPI_CTL_MIO_QUAD;
		/* Enable hardware auto chip select signal */
		control |= SPI_CTL_ASSEL | SPI_CTL_SELST;
		control |= SPI_CTL_EMISO | SPI_CTL_SIZE32;
		control |= SPI_CTL_FMODE;
		control &= ~SPI_CTL_SOSI;

		writel(mmrdh, &plat->regs->mmrdh);
		/* Set memory map top boundary */
		writel((u32)priv->memory_map + CONFIG_SPI_MM_SIZE,
		       &plat->regs->mmtop);
		writel(delay, &plat->regs->delay);
		writel(SPI_RXCTL_REN, &plat->regs->rx_control);
		writel(SPI_TXCTL_TEN | SPI_TXCTL_TTI, &plat->regs->tx_control);
		writel(control, &plat->regs->control);
		/* Enable hardware ssel pin */
		writel(ssel | BIT_SSEL_EN(priv->cs_num), &plat->regs->ssel);
		return 0;
	} else if (flags & SPI_XFER_MMAP_END) {
		u32 ssel = readl(&plat->regs->ssel);

		/* Restore control reg for non memory mapped transfer */
		writel(priv->control, &plat->regs->control);
		writel(0, &plat->regs->delay);
		/* Disable hardware ssel pin */
		writel(ssel & ~BIT_SSEL_EN(priv->cs_num), &plat->regs->ssel);
		return 0;
	}
#endif

	if (bitlen == 0)
		goto done;

	/* we can only do 8 bit transfers */
	if (bitlen % 8) {
		flags |= SPI_XFER_END;
		goto done;
	}

	if (flags & SPI_XFER_BEGIN)
		adi_spi_cs_activate(priv);

	ret = adi_spi_pio_xfer(priv, tx, rx, bytes, flags);

 done:
	if (flags & SPI_XFER_END)
		adi_spi_cs_deactivate(priv);

	return ret;
}

static int adi_spi_set_speed(struct udevice *bus, uint speed)
{
	struct adi_spi_platdata *plat = dev_get_plat(bus);
	struct adi_spi_priv *priv = dev_get_priv(bus);
	int ret;
	u32 clock, spi_base_clk;
	struct clk spi_clk;

	ret = clk_get_by_name(bus, "spi", &spi_clk);
	if (ret < 0) {
		printf("Can't get SPI clk: %d\n", ret);
		return ret;
	}
	spi_base_clk = clk_get_rate(&spi_clk);

	if (speed > plat->max_hz)
		speed = plat->max_hz;

	if (speed > spi_base_clk)
		return -ENODEV;

	clock = spi_base_clk / speed;
	if (clock)
		clock--;

	priv->clock = clock;

	debug("%s: priv->clock: %x, speed: %x, get_spi_clk(): %x\n",
	      __func__, clock, speed, spi_base_clk);

	return 0;
}

static int adi_spi_set_mode(struct udevice *bus, uint mode)
{
	struct adi_spi_priv *priv = dev_get_priv(bus);
	u32 reg;

	reg = SPI_CTL_EN | SPI_CTL_MSTR;
	if (mode & SPI_CPHA)
		reg |= SPI_CTL_CPHA;
	if (mode & SPI_CPOL)
		reg |= SPI_CTL_CPOL;
	if (mode & SPI_LSB_FIRST)
		reg |= SPI_CTL_LSBF;
	reg &= ~SPI_CTL_ASSEL;

	priv->control = reg;
	priv->cs_pol = mode & SPI_CS_HIGH ? 1 : 0;

	debug("%s: control=%d, cs_pol=%d\n", __func__, reg, priv->cs_pol);

	return 0;
}

static const struct dm_spi_ops adi_spi_ops = {
	.claim_bus = adi_spi_claim_bus,
	.release_bus = adi_spi_release_bus,
	.xfer = adi_spi_xfer,
	.set_speed = adi_spi_set_speed,
	.set_mode = adi_spi_set_mode,
	.cs_info = adi_spi_cs_info,
};

static const struct udevice_id adi_spi_ids[] = {
	{ .compatible = "adi,spi3" },
	{ }
};

U_BOOT_DRIVER(adi_spi3) = {
	.name = "adi_spi3",
	.id = UCLASS_SPI,
	.of_match = adi_spi_ids,
	.ops = &adi_spi_ops,
	.of_to_plat = adi_spi_of_to_plat,
	.probe = adi_spi_probe,
	.remove = adi_spi_remove,
	.plat_auto = sizeof(struct adi_spi_platdata),
	.priv_auto = sizeof(struct adi_spi_priv),
	.per_child_auto = sizeof(struct spi_slave),
};
