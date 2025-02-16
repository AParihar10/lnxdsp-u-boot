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
#include <dm.h>
#include <errno.h>
#include <post.h>
#include <watchdog.h>
#include <serial.h>
#include <asm/io.h>
#include <linux/compiler.h>
#include <linux/bitops.h>
#include <clk.h>

#if CONFIG_SC59X_64
#ifdef ADI_DYNAMIC_OSPI_QSPI_UART_MANAGEMENT
#include <asm/arch-adi/sc5xx-64/sc598-som-ezkit-dynamic-qspi-ospi-uart-mux.h>
#endif
#endif

/*
 * UART4 Masks
 */

/* UART_CONTROL */
#define UEN			BIT(0)
#define LOOP_ENA		BIT(1)
#define UMOD			(3 << 4)
#define UMOD_UART		(0 << 4)
#define UMOD_MDB		BIT(4)
#define UMOD_IRDA		BIT(4)
#define WLS			(3 << 8)
#define WLS_5			(0 << 8)
#define WLS_6			BIT(8)
#define WLS_7			(2 << 8)
#define WLS_8			(3 << 8)
#define STB			BIT(12)
#define STBH			BIT(13)
#define PEN			BIT(14)
#define EPS			BIT(15)
#define STP			BIT(16)
#define FPE			BIT(17)
#define FFE			BIT(18)
#define SB			BIT(19)
#define FCPOL			BIT(22)
#define RPOLC			BIT(23)
#define TPOLC			BIT(24)
#define MRTS			BIT(25)
#define XOFF			BIT(26)
#define ARTS			BIT(27)
#define ACTS			BIT(28)
#define RFIT			BIT(29)
#define RFRT			BIT(30)

/* UART_STATUS */
#define DR			BIT(0)
#define OE			BIT(1)
#define PE			BIT(2)
#define FE			BIT(3)
#define BI			BIT(4)
#define THRE			BIT(5)
#define TEMT			BIT(7)
#define TFI			BIT(8)
#define ASTKY			BIT(9)
#define ADDR			BIT(10)
#define RO			BIT(11)
#define SCTS			BIT(12)
#define CTS			BIT(16)
#define RFCS			BIT(17)

/* UART_EMASK */
#define ERBFI			BIT(0)
#define ETBEI			BIT(1)
#define ELSI			BIT(2)
#define EDSSI			BIT(3)
#define EDTPTI			BIT(4)
#define ETFI			BIT(5)
#define ERFCI			BIT(6)
#define EAWI			BIT(7)
#define ERXS			BIT(8)
#define ETXS			BIT(9)

DECLARE_GLOBAL_DATA_PTR;

struct uart4_reg {
	u32 revid;
	u32 control;
	u32 status;
	u32 scr;
	u32 clock;
	u32 emask;
	u32 emaskst;
	u32 emaskcl;
	u32 rbr;
	u32 thr;
	u32 taip;
	u32 tsr;
	u32 rsr;
	u32 txdiv_cnt;
	u32 rxdiv_cnt;
};

struct adi_uart4_platdata {
	// Hardware registers
	struct uart4_reg *regs;

	// Enable divide-by-one baud rate setting
	bool edbo;
};

#ifdef ADI_DYNAMIC_OSPI_QSPI_UART_MANAGEMENT

//If we're using the dynamic muxing of OSPI/QSPI/UART due to pinmux conflicts,
//Use a buffer to store characters which could not be transmitted while UART was disabled
//Once UART is reenabled and attempts to send another character, transmit through anything in
//the buffer first

#define BUFFER_SIZE 4096
bool uartEnabled = 1;
bool uartReadyToEnable;
char uartBuffer[BUFFER_SIZE];
int uartBufferPos;
#endif

static int adi_uart4_set_brg(struct udevice *dev, int baudrate)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;
	u32 divisor, uart_base_clk_rate;
	struct clk uart_base_clk;

	if (clk_get_by_index(dev, 0, &uart_base_clk)) {
		printf("%s: Could not get UART base clock\n", dev->name);
		return -1;
	}

	uart_base_clk_rate = clk_get_rate(&uart_base_clk);

	if (plat->edbo) {
		u16 divisor16 = (uart_base_clk_rate + (baudrate / 2)) / baudrate;

		divisor = divisor16 | BIT(31);
	} else {
		// Divisor is only 16 bits
		divisor = 0x0000ffff & ((uart_base_clk_rate + (baudrate * 8)) / (baudrate * 16));
	}

	writel(divisor, &regs->clock);
	return 0;
}

static int adi_uart4_pending(struct udevice *dev, bool input)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;

	if (input)
		return (readl(&regs->status) & DR) ? 1 : 0;
	else
		return (readl(&regs->status) & THRE) ? 0 : 1;
}

static int adi_uart4_getc(struct udevice *dev)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;
	int uart_rbr_val;

	if (!adi_uart4_pending(dev, true))
		return -EAGAIN;

	uart_rbr_val = readl(&regs->rbr);
	writel(-1, &regs->status);

	return uart_rbr_val;
}

static int adi_uart4_putc(struct udevice *dev, const char ch)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;

#ifdef ADI_DYNAMIC_OSPI_QSPI_UART_MANAGEMENT
	if (uartReadyToEnable) {
		adi_disable_ospi(1);
		writel('\n', &regs->thr);
	}

	if (!uartEnabled) {
		if (uartBufferPos < BUFFER_SIZE)
			uartBuffer[uartBufferPos++] = ch;
		return 0;
	} else if (uartBufferPos) {
		int i;

		for (i = 0; i < uartBufferPos; i++) {
			while (adi_uart4_pending(dev, false))
				WATCHDOG_RESET();
			writel(uartBuffer[i], &regs->thr);
		}
		uartBufferPos = 0;
	}
#endif

	if (adi_uart4_pending(dev, false))
		return -EAGAIN;

	writel(ch, &regs->thr);
	return 0;
}

static void adi_uart4_suspend(struct udevice *dev)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;
	u32 val;

	val = readl(&regs->control);
	writel(val & ~UEN, &regs->control);
}

static void adi_uart4_resume(struct udevice *dev)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;
	u32 val;

	val = readl(&regs->control);
	writel(val | UEN, &regs->control);
}

static const struct dm_serial_ops adi_uart4_serial_ops = {
	.setbrg = adi_uart4_set_brg,
	.getc = adi_uart4_getc,
	.putc = adi_uart4_putc,
	.pending = adi_uart4_pending,
	.suspend = adi_uart4_suspend,
	.resume = adi_uart4_resume,
};

static int adi_uart4_ofdata_to_platdata(struct udevice *dev)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	fdt_addr_t addr;

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;

	plat->regs = (struct uart4_reg *)addr;
	plat->edbo = dev_read_bool(dev, "adi,enable-edbo");

	return 0;
}

static int adi_uart4_probe(struct udevice *dev)
{
	struct adi_uart4_platdata *plat = dev->platdata;
	struct uart4_reg *regs = plat->regs;

	/* always enable UART to 8-bit mode */
	writel(UEN | UMOD_UART | WLS_8, &regs->control);

	writel(-1, &regs->status);

	return 0;
}

static const struct udevice_id adi_uart4_serial_ids[] = {
	{ .compatible = "adi,uart4" },
	{ }
};

U_BOOT_DRIVER(serial_adi_uart4) = {
	.name = "serial_adi_uart4",
	.id = UCLASS_SERIAL,
	.of_match = adi_uart4_serial_ids,
	.ofdata_to_platdata = adi_uart4_ofdata_to_platdata,
	.platdata_auto_alloc_size = sizeof(struct adi_uart4_platdata),
	.probe = adi_uart4_probe,
	.ops = &adi_uart4_serial_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
