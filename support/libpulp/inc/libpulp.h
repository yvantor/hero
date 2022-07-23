#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h> // for useconds_t

#include "pulp_shared.h"
// #include "herodev.h"

typedef struct {
  void *v_addr;
  void *p_addr;
  unsigned size;
} PulpSubDev;

enum log_level {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3,
  LOG_TRACE = 4,
  LOG_MIN = LOG_ERROR,
  LOG_MAX = LOG_TRACE,
};

// Must be same as in PULP Runtime
struct BootData {
  uint32_t boot_addr;
  uint32_t core_count;
  uint32_t hartid_base;
  uint32_t tcdm_start;
  uint32_t tcdm_size;
  uint32_t tcdm_offset;
  uint64_t global_mem_start;
  uint64_t global_mem_end;
  uint32_t cluster_count;
  uint32_t s1_quadrant_count;
};

typedef struct {
  int fd; // file descriptor
  PulpSubDev clusters;
  struct pulp_cluster_info pci;
  PulpSubDev l1;
  PulpSubDev l2;
  PulpSubDev periph;
  PulpSubDev l3;
  PulpSubDev l2;
  struct O1HeapInstance *l3_heap_mgr;
  // offset in l3 memory where shared data region starts and the o1heap manager is allocating
  uint64_t l3_data_offset;
  // L3 layout structure, private to each cluster
  struct l3_layout *l3l;
  // Physical address of l3_layout data
  void *l3l_p;
  unsigned int cluster_sel;
  volatile struct ring_buf *a2h_mbox;
  volatile struct ring_buf *h2a_mbox;
} pulp_dev_t;

/**
 * Values read from pulp cluster peripheral
 */
typedef struct {
  uint32_t tcdmStartAddress;
  uint32_t tcdmEndAddress;
  uint32_t nrCores;
  uint32_t fetchEnable;
  uint64_t scratch;
  uint64_t cycle;
  uint32_t barrier;
  uint32_t hartBaseID;
  uint32_t tcdmAccessed;
  uint32_t tcdmCongested;
  uint32_t *counters;
} pulp_perf_t;

// striping informationg structure
typedef struct {
  unsigned n_stripes;
  unsigned first_stripe_size_b;
  unsigned last_stripe_size_b;
  unsigned stripe_size_b;
} StripingDesc;

typedef enum {
  copy = 0x0, // no SVM, copy-based sharing using contiguous L3 memory
  svm_static = 0x1, // SVM, set up static mapping at offload time, might fail - use with caution
  svm_stripe = 0x2, // SVM, use striping (L1 only), might fail - use with caution
  svm_mh = 0x3, // SVM, use miss handling
  copy_tryx = 0x4, // no SVM, copy-based sharing using contiguous L3 memory, but let PULP do the tryx()
  svm_smmu = 0x5, // SVM, use SMMU instead of RAB
  svm_smmu_shpt = 0x6, // SVM, use SMMU, emulate sharing of user-space page table, no page faults
  custom = 0xF, // do not touch (custom marshalling)
} ShMemType;

typedef enum {
  val = 0x0, // pass by value

  ref_copy = 0x10, // pass by reference, no SVM, use contiguous L3 memory
  ref_svm_static = 0x11, // pass by reference, SVM, set up mapping at offload time
  ref_svm_stripe = 0x12, // pass by reference, SVM, set up striped mapping
  ref_svm_mh = 0x13, // pass by reference, SVM, do not set up mapping, use miss handling
  ref_copy_tryx = 0x14, // pass by reference, no SVM, use contiguous L3 memory, but to the tryx() - mapped to 0x10
  ref_custom = 0x1F, // pass by reference, do not touch (custom marshalling)
} ElemPassType;

// shared variable data structure
typedef struct {
  void *ptr; // address in host virtual memory
  void *ptr_l3_v; // host virtual address in contiguous L3 memory   - filled by runtime library based on sh_mem_ctrl
  void *ptr_l3_p; // PULP physical address in contiguous L3 memory  - filled by runtime library based on sh_mem_ctrl
  unsigned size; // size in Bytes
  ElemType type; // inout, in, out
  ShMemType sh_mem_ctrl;
  unsigned char cache_ctrl; // 0: flush caches, access through DRAM
    // 1: do not flush caches, access through caches
  unsigned char rab_lvl; // 0: first L1, L2 when full
    // 1: L1 only
    // 2: L2 only
  StripingDesc *stripe_desc; // only used if sh_mem_ctrl = 2
} DataDesc;


/*     _    ____ ___
 *    / \  |  _ \_ _|
 *   / _ \ | |_) | |
 *  / ___ \|  __/| |
 * /_/   \_\_|  |___|
 */

/**
 * @brief Set the log level accoring to `ll`
 *
 * @param dev pointer to PULP struct
 * @param ll log levl
 */
void pulp_set_log_level(enum log_level ll);

/**
 * @brief Discover PULP clusters by globbing /dev/pulp* and probing each device
 * @param devs if not NULL, a list of strings to the discovered devices it put in *devs
 * @param cluster if not NULL, number of clusters written to
 * @param quadrants if not NULL, number of quadrants written to
 *
 * @return int negative error or number of clusters discovered
 */
int pulp_discover(char ***devs, uint32_t *clusters, uint32_t *quadrants);

/**
 * @brief Memory-map all pulp clusters in the system to user-space
 * @param nr_dev [out] number of devices mapped
 *
 * @return pulp_dev_t** array of pointers to pulp cluster devices
 */
pulp_dev_t **pulp_mmap_all(uint32_t *nr_dev);

/**
 * @brief Map the pulp device to virtual user space using mmap syscall to the pulp driver
 * @details
 *
 * @param dev pointer to pulp struct
 * @param fname path to the char device (e.g. /dev/pulp1)
 * @return 0 on success, negative and errno on fault
 */
int pulp_mmap(pulp_dev_t *dev, char *fname);

/**
 * @brief Flush all data caches
 * @details
 *
 * @param dev pointer to pulp struct
 * @return 0 on success, negative and errno on fault
 */
int pulp_flush(pulp_dev_t *dev);

/**
 * @brief Perform hardware reset of all components. Clusters are trapped in bootrom after this and
 * wait for interrupt
 * @details
 *
 * @param dev pointer to pulp device structure
 *
 * @return 0 on success
 */
int pulp_reset(pulp_dev_t *dev);

/**
 * @brief Load pulp binary to L3
 *
 * @param dev pointer to pulp struct
 * @param name file name
 *
 * @return binary size in bytes on success, negative errno on error
 */
int pulp_load_bin(pulp_dev_t *dev, const char *name);

/**
 * @brief Set isolation of pulp cluster. Forwarded to driver. Isolate waits for isolated and then
 * puts cluster in reset. Deisolation releases from reset and clears isolateion. Return can be
 * -ETIMEOUT if isolated response not received
 * @details
 *
 * @param dev pointer to pulp struct
 * @param iso 1 to isolate, 0 to de-isolate
 * @return 0 on success, negative ERRNO on failure
 */
int pulp_isolate(pulp_dev_t *dev, int iso);

/**
 * @brief Wake up of the cluster. Forwarded to driver. Return can be
 * -ETIMEOUT if isolated response not received
 * @details
 *
 * @param dev pointer to pulp struct
 * @return 0 on success, negative ERRNO on failure
 */
int pulp_wakeup(pulp_dev_t *dev);

/**
 * @brief Wake up of the cluster. Forwarded to driver. Return can be
 * -ETIMEOUT if isolated response not received
 * @details
 *
 * @param dev pointer to pulp struct
 * @param boot_addr boot addresses for CV32E40P
 * @return 0 on success, negative ERRNO on failure
 */
int pulp_launch_cluster(pulp_dev_t *dev, uint32_t boot_addr);

/**
 * @brief Write into cluster quadrant registers
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @param val value to write to
 * @return int return value of the ioctl call, 0 on success, negative error on failure
 */
int pulp_quadrant_reg_write(pulp_dev_t *dev, uint32_t reg, uint32_t val);

/**
 * @brief Read from cluster quadrant registers
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @return val value that was read
 */
int pulp_quadrant_reg_read(pulp_dev_t *dev, uint32_t reg);

/**
 * @brief Write into cluster peripheral registers
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @param val value to write to
 * @return int return value of the ioctl call, 0 on success, negative error on failure
 */
int pulp_periph_reg_write(pulp_dev_t *dev, uint32_t reg, uint32_t val);

/**
 * @brief Read from cluster peripheral registers
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @return val value that was read
 */
int pulp_periph_reg_read(pulp_dev_t *dev, uint32_t reg);

  
/**
 * @brief Write to a SoC scratch register
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @param val value to write to
 * @return int return value of the ioctl call, 0 on success, negative error on failure
 */
int pulp_scratch_reg_write(pulp_dev_t *dev, uint32_t reg, uint32_t val);

/**
 * @brief Read from a SoC scratch register
 *
 * @param dev pointer to pulp struct
 * @param reg register offset in words
 * @param val value that was read
 * @return int return value of the ioctl call, 0 on success, negative error on failure
 */
int pulp_scratch_reg_read(pulp_dev_t *dev, uint32_t reg, uint32_t *val);


/** Allocate a chunk of memory in contiguous L3.

  \param    dev   pointer to the pulp_dev_t structure
  \param    size size in Bytes of the requested chunk
  \param    p_addr pointer to store the physical address to

  \return   virtual user-space address for host
 */
void *pulp_l3_malloc(pulp_dev_t *dev, size_t size, void **p_addr);

/** Free memory previously allocated in contiguous L3.

 \param    dev   pointer to the pulp_dev_t structure
 \param    v_addr pointer to unsigned containing the virtual address

 */
void pulp_l3_free(pulp_dev_t *dev, void *v_addr);

/** Enable mbox irq generation

  \param    dev   pointer to the pulp_dev_t structure
  \param    direction, either H2C or C2H
  \param    error, write or read?

  \return   0 on success; negative value with an errno on errors.
 */
int pulp_mbox_set_irq(pulp_dev_t *dev, uint32_t dir, uint32_t ewr);

/** Clear pending irw from mbox

  \param    dev   pointer to the pulp_dev_t structure
  \param    direction, either H2C or C2H
  \param    error, write or read?

  \return   0 on success; negative value with an errno on errors.
 */
int pulp_mbox_clear_irq(pulp_dev_t *dev, uint32_t dir, uint32_t ewr);

/** Read one or multiple words from the mailbox. Blocks if the mailbox does not contain enough
 *  data.

 \param    dev   pointer to the pulp_dev_t structure
  \param    buffer  pointer to read buffer
  \param    n_words number of words to read

  \return   0 on success; negative value with an errno on errors.
 */
int pulp_mbox_read(pulp_dev_t *dev, uint32_t *buffer, size_t n_words);

/**
 * @brief Try to read from the mailbox. On success, message is written to buffer and 1 is returned.
 * If no element is present, 0 is returned
 *
 * @param dev pointer to the pulp_dev_t structure
 * @param buffer pointer to the buffer where the message word is stores
 * @return int 0 if no message was received, 1 if message was received and written to buffer
 */
int pulp_mbox_try_read(pulp_dev_t *dev);

/** Write one word to the mailbox. Blocks if the mailbox is full.

 \param    dev   pointer to the pulp_dev_t structure
 \param     word word to write

 \return    0 on success; negative value with an errno on errors.
 */
int pulp_mbox_write(pulp_dev_t *dev, uint32_t word);

/**
 * @brief Try to read from the mailbox. On success, message is written to buffer and 1 is returned.
 * If no element is present, 0 is returned
 *
 * @param dev pointer to the pulp_dev_t structure
 * @param buffer pointer to the buffer where the message word is stores
 * @return int 0 if no message was received, 1 if message was received and written to buffer
 */
int pulp_mbox_try_write(pulp_dev_t *dev);

/**
 * @brief Flush mbox fifo
 *
 * @param dev pointer to the pulp_dev_t structure
 * @param direction, either H2C or C2H
 * @param error, write or read?
 * @return ioctl return code.
 */
int pulp_mbox_flush(pulp_dev_t *dev, uint32_t dir, uint32_t wr);

/**
 * @brief Write to a TLB entry
 *
 * @param    dev   pointer to the pulp_dev_t structure
 * @param    e     the TLB entry
 * @return int errno return code
 */
int pulp_tlb_write(pulp_dev_t *dev, struct axi_tlb_entry *e);

/**
 * @brief Read a TLB entry
 *
 * @param    dev   pointer to the pulp_dev_t structure
 * @param    e     the TLB entry
 * @return int errno return code
 */
int pulp_tlb_read(pulp_dev_t *dev, struct axi_tlb_entry *e);

/**
 * @brief Set device log level through mailboxes.
 *
 * @param dev   pointer to the pulp_dev_t structure
 * @param lvl   new log level or -1 to read it from the PULP_DEBUG environment variable
 */
void pulp_set_device_loglevel(pulp_dev_t *dev, int lvl);

/**
 * @brief Wait for message from mbox
 *
 * @param  dev   pointer to the pulp_dev_t structure
 * @param  timeout_s timeout in seconds
 * @return mcycle counter value when the event is processed by the handler
 */
int pulp_mbox_wait(pulp_dev_t *dev, int timeout_s);

/**
 * @brief Sample MCYCLE counter inside the kernel
 *
 * @param  dev   pointer to the pulp_dev_t structure
 */
int pulp_t_start(pulp_dev_t *dev);

/**
 * @brief Write boot addr and enable RI5CYs' execution
 *
 * @param  dev   pointer to the pulp_dev_t structure
 * @param  boot_addr to set for RI5CYs
 * @return 0 periph_reg_writes have been successfull
 */
int pulp_exe_start(pulp_dev_t *dev, uint32_t boot_addr);

/**
 * @brief Wait for message from mbox
 *
 * @param  dev   pointer to the pulp_dev_t structure
 * @param  timeout_s timeout in seconds
 * @return mcycle counter value when the event is processed by the handler
 */
int pulp_exe_wait(pulp_dev_t *dev, int timeout_s);

/**
 * @brief Get the clk cnt difference between pulp_t_start and when the eoc arrives
 *
 */
long unsigned int pulp_get_exe_time(pulp_dev_t *dev);


/**
 * @brief Get the clk cnt to load the binary
 *
 */
long unsigned int pulp_get_load_time();
//!@}
