/*
 * \brief  OMAP4 UART definitions
 * \author Ivan Loskutov <ivan.loskutov@ksyslabs.org>
 * \date   2012-11-08
 */

/*
 * Copyright (C) 2012 Ksys Labs LLC
 * Copyright (C) 2011-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _INCLUDE__PLATFORM__PANDABOARD__UART_DEFS_H_
#define _INCLUDE__PLATFORM__PANDABOARD__UART_DEFS_H_

#include <platform/panda/drivers/board.h>

enum {
	/** Number of UARTs */
	UARTS_NUM = 4,

	BAUD_115200 = 26,
};


static struct Omap_uart_cfg {
	Genode::addr_t mmio_base;
	Genode::size_t mmio_size;
	int            irq_number;
} omap_uart_cfg[UARTS_NUM] = {
	{ Genode::Board::TL16C750_1_MMIO_BASE, Genode::Board::TL16C750_MMIO_SIZE, Genode::Board::TL16C750_1_IRQ + 32 },
	{ Genode::Board::TL16C750_2_MMIO_BASE, Genode::Board::TL16C750_MMIO_SIZE, Genode::Board::TL16C750_2_IRQ + 32 },
	{ Genode::Board::TL16C750_3_MMIO_BASE, Genode::Board::TL16C750_MMIO_SIZE, Genode::Board::TL16C750_3_IRQ + 32 },
	{ Genode::Board::TL16C750_4_MMIO_BASE, Genode::Board::TL16C750_MMIO_SIZE, Genode::Board::TL16C750_4_IRQ + 32 },
};

#endif /* _INCLUDE__PLATFORM__PANDABOARD__UART_DEFS_H_ */
