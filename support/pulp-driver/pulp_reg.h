#ifndef _PULP_REG_H
#define _PULP_REG_H

/**
 * @brief SoC-control register map
 *
 */
#define SCTL_REG_OFFSET 0x0

/**
 * @brief Quadrant-control register map
 *
 */
#define QCTL_TLB_NARROW_REG_OFFSET 0x0UL
#define QCTL_TLB_WIDE_REG_OFFSET   0x1000UL

/**
 * @brief Cluster peripheral registers
 *
 */
#define CPER_CONTROLUNIT_FE 0x8
#define CPER_RI5CY_BOOTADDR 0x40
#define CPER_INSTRSCACHE_FE 0x14000

#endif
