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
#define QCTL_TLB_NARROW_REG_OFFSET    0x0000UL
#define QCTL_H2C_MAILBOIX_REG_OFFSET  0x2000UL
#define QCTL_C2H_MAILBOIX_REG_OFFSET  0x3000UL

/**
 * @brief Cluster peripheral registers
 *
 */
#define CPER_CONTROLUNIT_FE  0x8
#define CPER_RI5CY_BOOTADDR0 0x40
#define CPER_RI5CY_BOOTADDR1 0x44
#define CPER_RI5CY_BOOTADDR2 0x48
#define CPER_RI5CY_BOOTADDR3 0x4C
#define CPER_RI5CY_BOOTADDR4 0x50
#define CPER_RI5CY_BOOTADDR5 0x54
#define CPER_RI5CY_BOOTADDR6 0x58
#define CPER_RI5CY_BOOTADDR7 0x5C
#define CPER_INSTRSCACHE_FE  0x14000

#endif
