/*
 * This file is part of the PULP device driver.
 *
 * Copyright (C) 2022 ETH Zurich, University of Bologna
 *
 * Author: Noah Huetter <huettern@iis.ee.ethz.ch>
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

#ifndef _PULP_SHARED_H
#define _PULP_SHARED_H

/**
 * @brief When reading from the pulp driver /dev/pulp of size sizeof(struct pulp_cluster_info)
 * this struct is populated and returned to user space
 * @compute_num: Number of compute cores in the cluster
 * @dm_num: Number of data mover cores in the cluster
 * @cluster_idx: Cluster index in a multi-cluster system
 * @quadrant_idx: Quadrant index in a multi-quadrant system
 * @l3_size: Size of the reserved L3 memory
 * @l1_size: Size of the TCDM address space
 * @l3_paddr: L3 physical base address
 * @l1_paddr: L1 physical base address
 */
struct pulp_cluster_info {
  uint32_t compute_num;
  uint32_t dm_num;
  uint32_t cluster_idx;
  uint32_t quadrant_idx;
  size_t l3_size;
  size_t l2_size;
  size_t l1_size;
  size_t periph_size;
  void *l3_paddr;
  void *l2_paddr;
  void *l1_paddr;
};

/**
 * Macros to calculate the base address for mmap'ing regions to user space. Must be a multple of
 * PAGE_SHIFT
 */
#define PULP_MMAP_L3_MAIN (0 * sysconf(_SC_PAGE_SIZE))
#define PULP_MMAP_TCDM    (1 * sysconf(_SC_PAGE_SIZE))
#define PULP_MMAP_L2_SCPM (2 * sysconf(_SC_PAGE_SIZE))

typedef enum {
  inout = 0,
  in = 1,
  out = 2,
} ElemType;

/**
 * IOCTL
 */

#define PULPIOC_MAGIC 's'

struct pulpios_reg {
  uint32_t off;
  uint32_t val;
};

struct pulpiot_val {
  int      timeout;
  uint64_t counter;
};

enum AxiTlbFlags { AXI_TLB_VALID = 0x07, AXI_TLB_RO = 0x02 };
enum AxiTlbLoc { AXI_TLB_NARROW = 1, AXI_TLB_WIDE = 2 };
struct axi_tlb_entry {
  enum AxiTlbLoc loc;
  enum AxiTlbFlags flags;
  uint64_t idx;
  uint64_t first;
  uint64_t last;
  uint64_t base;
};

/**
 * @brief set options of the pulp cluster. See below for options encoding
 *
 */
#define PULPIOC_SET_OPTIONS _IOW(PULPIOC_MAGIC, 0, char *)
/**
 * @brief Write to pcratch registers. Pass `struct pulpios_reg` containing the register offset and
 * value
 *
 */
#define PULPIOS_SCRATCH_W _IOW(PULPIOC_MAGIC, 1, struct pulpios_reg)
/**
 * @brief Read from pcratch registers. Pass `struct pulpios_reg` containing the register offset and
 * value is returned in the sturct
 *
 */
#define PULPIOS_SCRATCH_R _IOWR(PULPIOC_MAGIC, 2, struct pulpios_reg)
/**
 * @brief Read isolation bits for quadrant passed by argument. Return by value, 4 bit field of
 * isolated register
 *
 */
#define PULPIOS_READ_ISOLATION _IOR(PULPIOC_MAGIC, 3, char *)

/**
 * @brief Set bits in the SW-interrupt register. Pass struct pulpios_reg with register offset in
 * `reg` and value in `val`.
 *
 */
#define PULPIOS_SET_IPI _IOR(PULPIOC_MAGIC, 4, struct pulpios_reg)

/**
 * @brief Clear bits in the SW-interrupt register. Pass struct pulpios_reg with register offset
 * in `reg` and value in `val` (set bits in `val` will be cleared).
 *
 */
#define PULPIOS_CLEAR_IPI _IOR(PULPIOC_MAGIC, 5, struct pulpios_reg)

/**
 * @brief Get bits in the SW-interrupt register. Pass struct pulpios_reg with register offset in
 * `reg` and value is written to `val`
 *
 */
#define PULPIOS_GET_IPI _IOWR(PULPIOC_MAGIC, 6, struct pulpios_reg)

/**
 * @brief Does a simple `fence` to flush the data-cache (cva6 specific, might not work on all
 * architectures)
 *
 */
#define PULPIOS_FLUSH _IO(PULPIOC_MAGIC, 7)
/**
 * @brief Write a TLB entry with the contents of the argument struct
 *
 */
#define PULPIOS_WRITE_TLB_ENTRY _IOR(PULPIOC_MAGIC, 8, struct axi_tlb_entry)
/**
 * @brief Read a TLB entry to the argument struct
 *
 */
#define PULPIOS_READ_TLB_ENTRY _IOW(PULPIOC_MAGIC, 9, struct axi_tlb_entry)

/**
 * @brief write in cluster periph region
 *
 */
#define PULPIOC_PERIPH_W _IOW(PULPIOC_MAGIC, 10, struct pulpios_reg)

/**
 * @brief read from cluster periph region
 *
 */
#define PULPIOC_PERIPH_R _IOW(PULPIOC_MAGIC, 11, struct pulpios_reg)

/**
 * @brief read from cluster periph region
 *
 */
#define PULPIOT_WAIT_MBOX _IOWR(PULPIOC_MAGIC, 12, struct pulpiot_val)

/**
 * @brief read from cluster periph region
 *
 */
#define PULPIOT_WAIT_EOC _IOWR(PULPIOC_MAGIC, 13, struct pulpiot_val)

/**
 * @brief start cluster @ boot_addr
 *
 */
#define PULPIOC_PERIPH_START _IOW(PULPIOC_MAGIC, 14, struct pulpios_reg)

/**
 * @brief start counter in kernel space
 *
 */
#define PULPIOT_GET_T _IOW(PULPIOC_MAGIC, 15, struct pulpiot_val)

/**
 * @brief write in cluster periph region
 *
 */
#define PULPIOC_QUADRANT_W _IOW(PULPIOC_MAGIC, 16, struct pulpios_reg)

/**
 * @brief read from cluster periph region
 *
 */
#define PULPIOC_QUADRANT_R _IOW(PULPIOC_MAGIC, 17, struct pulpios_reg)


// Values for PULPIOC_SET_OPTIONS
#define PULPIOS_DEISOLATE 0x0001   /* De-Isolate the cluster */
#define PULPIOS_ISOLATE   0x0002   /* Isolate the cluster    */
#define PULPIOS_WAKEUP    0x0003   /* Wake up the cluster    */


/**
 * @brief Cluster peripheral registers
 *
 */
#define CPER_CONTROLUNIT_FE  0x8
#define CPER_RI5CY_BOOTADDR0 0x40
#define CPER_INSTRSCACHE_FE  0x1400

#define TIMER_COUNTER 0x0
#define TIMER_CTRL 0x4
#define TIMER_CMP 0x8
#define N_CLUSTERS 1
// #define N_CORES 2
#define L2_MEM_SIZE_KB 512
#define L1_MEM_SIZE_KB 256
#define PULP_DEFAULT_FREQ_MHZ 50
#define CLKING_INPUT_FREQ_MHZ 100

#endif
