/*
 * This file is part of the PULP device driver.
 *
 * Copyright (C) 2022 ETH Zurich, University of Bologna
 *
 * Author: Luca Valente <luca.valente@unibo.it>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Mailbox
 */
#define H2C_DIR 0
#define C2H_DIR 1

#define MBOX_FIFO_DEPTH 16
#define MBOX_WRDATA_OFFSET 0x0
#define MBOX_RDDATA_OFFSET 0x4
#define MBOX_STATUS_OFFSET 0x8
#define MBOX_ERROR_OFFSET 0xC
#define MBOX_WIRQT_OFFSET 0x10 // IRQ THRESHOLD LEVEL
#define MBOX_RIRQT_OFFSET 0x14
#define MBOX_IRQS_OFFSET 0x18
#define MBOX_IRQEN_OFFSET 0x1C
#define MBOX_IRQP_OFFSET 0x20
#define MBOX_CTRL_OFFSET 0x24

#define MBOX_WFIFO_MASK_HTT 0x8   // bitmask that tells if the FIFO level is higher than the threshold
#define MBOX_RFIFO_MASK_HTT 0x4   // bitmask that tells if the FIFO level is higher than the threshold
#define MBOX_WFIFO_MASK_FULL 0x2   // bitmask that selects the Full bit
#define MBOX_RFIFO_MASK_EMPTY 0x1  // bitmask that selects the Empty bit

#define MBOX_WFIFO_MASK_ERROR 0x2   // bitmask that selects the Full bit
#define MBOX_RFIFO_MASK_ERROR  0x1  // bitmask that selects the Empty bit

#define MBOX_IRQ_MASK_ALL 0x7   // bitmask that selects all IRQs
#define MBOX_IRQ_MASK_ERROR 0x4 // bitmask that selects only Error IRQ
#define MBOX_IRQ_MASK_READ 0x2  // bitmask that selects only Rex0ad Threshold IRQ
#define MBOX_IRQ_MASK_WRITE 0x1 // bitmask that selects only Write Threshold IRQ
#define MBOX_IRQ_MASK_NONE 0x0  // bitmask that selects no IRQs

#define MBOX_CTRL_MASK_FLUSH_WRITES 0x1 // bitmask that flushes writes
#define MBOX_CTRL_MASK_FLUSH_READS 0x2  // bitmask that flushes reads
#define MBOX_H2C_BASE_ADDR 0x2000
#define MBOX_C2H_BASE_ADDR 0x3000
#define MBOX_SIZE_B 0x1000 // Interface 0 only
