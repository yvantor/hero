
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"
#include "fesrv.h"
#include "libpulp.h"
#include "pulp_mbox.h"
#include "pulp_common.h"

#define DFLT_CLUSTER_IDX 0

#define max(a, b)                                                                                  \
  ({                                                                                               \
    __typeof__(a) _a = (a);                                                                        \
    __typeof__(b) _b = (b);                                                                        \
    _a > _b ? _a : _b;                                                                             \
  })
#define min(a, b)                                                                                  \
  ({                                                                                               \
    __typeof__(a) _a = (a);                                                                        \
    __typeof__(b) _b = (b);                                                                        \
    _a < _b ? _a : _b;                                                                             \
  })

#define ALIGN_UP(x, p) (((x) + (p)-1) & ~((p)-1))

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

int wakeup_all(pulp_dev_t **clusters, uint32_t nr_dev) {
  int ret = 0;
  int status;
  for (uint32_t i = 0; i < nr_dev; ++i) {
    status = pulp_wakeup(clusters[i]);
    if (status != 0) {
      printf("Wakeup failed for cluster %d: %s\n", i, strerror(ret));
      ret -= 1;
    }
  }
  return ret;
}

int isolate_all(pulp_dev_t **clusters, uint32_t nr_dev, uint32_t iso) {
  int ret = 0;
  int status;
  for (uint32_t i = 0; i < nr_dev; ++i) {
    status = pulp_isolate(clusters[i], iso);
    if (status != 0) {
      printf("%sisolation failed for cluster %d: %s\n", iso == 0 ? "de-" : "", i, strerror(ret));
      ret -= 1;
    }
  }
  return ret;
}

void set_direct_tlb_map(pulp_dev_t *pulp, uint32_t idx, uint32_t low, uint32_t high) {
  struct axi_tlb_entry tlb_entry;
  tlb_entry.loc = AXI_TLB_NARROW;
  tlb_entry.flags = AXI_TLB_VALID;
  tlb_entry.idx = idx;
  tlb_entry.first = low;
  tlb_entry.last = high;
  tlb_entry.base = low;
  pulp_tlb_write(pulp, &tlb_entry);
}

void reset_tlbs(pulp_dev_t *pulp) {
  struct axi_tlb_entry tlb_entry;
  tlb_entry.flags = 0;
  tlb_entry.first = 0;
  tlb_entry.last = 0;
  tlb_entry.base = 0;
  for(unsigned idx = 0; idx < 32; ++idx) {
    tlb_entry.idx = idx;
    tlb_entry.loc = AXI_TLB_NARROW;
    pulp_tlb_write(pulp, &tlb_entry);
  }
}

int main(int argc, char *argv[]) {
  pulp_dev_t *pulp;
  pulp_dev_t **clusters;
  // pulp_perf_t perf;
  void *shared_l3_v;
  int size;
  uint32_t cluster_idx, nr_dev;
  void *addr, *a2h_rb_addr;
  int ret;
  uint32_t mask;
  struct axi_tlb_entry tlb_entry;

  #ifdef VERBOSE
  printf("This is %s\n", argv[0]);
  printf("Usage: %s [pulp_binary [cluster_idx]]\n", argv[0]);
  printf("  Default cluster index is %d\n", DFLT_CLUSTER_IDX);
  #endif
  cluster_idx = DFLT_CLUSTER_IDX;
  if (argc == 3) {
    cluster_idx = atoi(argv[2]);
    printf("  Running on cluster %d\n", cluster_idx);
  }

  // No app specified discover and exit
  pulp_set_log_level(LOG_WARN);

  // Map clusters to user-space and pick one for tests
  clusters = pulp_mmap_all(&nr_dev);
  // Restrict to local cluster and remote on qc
  nr_dev = 1 ;
  pulp = clusters[cluster_idx];

  // Add TLB entry for required ranges
  reset_tlbs(pulp);

  set_direct_tlb_map(pulp, 0, 0x00000000, 0xffffffff); // whole address space

  for(unsigned i = 0; i < 1; ++i) {
    memset(&tlb_entry, 0, sizeof(tlb_entry));
    tlb_entry.loc = AXI_TLB_NARROW;
    tlb_entry.idx = i;
    pulp_tlb_read(pulp, &tlb_entry);
    #ifdef VERBOSE
    printf("TLB readback Narrow: idx %ld first %012lx last %012lx base %012lx flags %02x\n", tlb_entry.idx,
          tlb_entry.first, tlb_entry.last, tlb_entry.base, tlb_entry.flags);
    #endif
  }
  
  // De-isolate quadrant
  isolate_all(clusters, nr_dev, 1);
  ret = isolate_all(clusters, nr_dev, 0);
  if (ret) {
    isolate_all(clusters, nr_dev, 1);
    exit(-1);
  }

  // Zero the memory to ensure 
  if (memtest(pulp->l1.v_addr, pulp->l1.size, "TCDM", 0x0))
    return -1;

  if (memtest(pulp->l2.v_addr, pulp->l2.size, "TCDM", 0x0))
    return -1;

  // and some test scratch l3 memory
  // For largest axpy problem: (2*N+1)*sizeof(double), N=3*3*6*2048
  // For largest conv2d problem: (64*112*112+64*64*7*7+64*112*112)*sizeof(double) = 14112*1024
  // 2x for good measure
  shared_l3_v = pulp_l3_malloc(pulp, 2 * 14112 * 1024, &addr);
  assert(shared_l3_v);
  pulp->l3l->heap = (uint32_t)(uintptr_t)addr;
  if (memtest(shared_l3_v, 1024, "L3", '3'))
    return -1;

  pulp_reset(pulp);

  // Loads the bin
  size = pulp_load_bin(pulp, argv[1]);

  //  wakeup_all(clusters,nr_dev);

  pulp_mbox_set_irq(pulp,C2H_DIR,0);
  pulp_mbox_write(pulp,0x90000000);
  pulp_mbox_write(pulp,0x90004000);
  pulp_mbox_write(pulp,0x90008000);


  unsigned long int start;
  unsigned long int mid;
  unsigned long int end;
  uint32_t buffer[2];
  asm volatile("": : :"memory");
  start = read_csr(cycle);
  asm volatile("": : :"memory");
  ret = pulp_exe_start(pulp,0x1C000000);
  asm volatile("": : :"memory");
  mid = read_csr(cycle);
  asm volatile("": : :"memory");
  ret = pulp_mbox_wait(pulp,10);
  asm volatile("": : :"memory");
  end = read_csr(cycle);
  asm volatile("": : :"memory");  

  pulp_mbox_read(pulp,buffer,1);
  printf("Received from CL : %d\n",buffer[0]);

  pulp_mbox_clear_irq(pulp,C2H_DIR,0);

  printf("Load bin time %lu\n", pulp_get_load_time());
  printf("EXE S %lu \n", mid - start);
  printf("MBOX %lu \n", end - mid);
  printf("Total %lu \n", end - start);
  printf("Cycle count %lu\n", pulp_get_exe_time(pulp)); 
  
  pulp_mbox_clear_irq(pulp,C2H_DIR,0);  

  pulp_mbox_flush(pulp,C2H_DIR,0);

  printf("Exiting\n");

  pulp_l3_free(pulp, shared_l3_v);

  return ret;
}
