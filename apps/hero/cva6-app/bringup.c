
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

  printf("This is %s\n", argv[0]);
  printf("Usage: %s [pulp_binary [cluster_idx]]\n", argv[0]);
  printf("  Default cluster index is %d\n", DFLT_CLUSTER_IDX);
  cluster_idx = DFLT_CLUSTER_IDX;
  if (argc == 3) {
    cluster_idx = atoi(argv[2]);
    printf("  Running on cluster %d\n", cluster_idx);
  }

  // No app specified discover and exit
  pulp_set_log_level(LOG_INFO);
  // if (argc < 2) {
  //   pulp_discover(NULL, NULL, NULL);
  //   exit(0);
  // }

  // Map clusters to user-space and pick one for tests
  pulp_set_log_level(LOG_MAX);
  clusters = pulp_mmap_all(&nr_dev);
  // Restrict to local cluster and remote on qc
  nr_dev = 1 ;
  pulp = clusters[cluster_idx];

  // Use L3 layout struct from the cluster provided as argument and set it's pointer in scratch[2]
  // pulp_scratch_reg_write(pulp, 2, (uint32_t)(uintptr_t)pulp->l3l_p);

  // clear all interrupts
//  pulp_ipi_clear(pulp, 0, ~0U);
//  pulp_ipi_get(pulp, 0, &mask);
//  printf("clint after clear: %08x\n", mask);

  // Add TLB entry for required ranges
  reset_tlbs(pulp);

  //set_direct_tlb_map(pulp, 0, 0x10000000, 0x10400000); 
  //set_direct_tlb_map(pulp, 1, 0x02000000, 0x02000fff); // SoC Control
  set_direct_tlb_map(pulp, 0, 0x00000000, 0xffffffff); // whole address space
  //set_direct_tlb_map(pulp, 1, 0x80000000, 0xffffffff); // HBM0/1

  for(unsigned i = 0; i < 1; ++i) {
    memset(&tlb_entry, 0, sizeof(tlb_entry));
    tlb_entry.loc = AXI_TLB_NARROW;
    tlb_entry.idx = i;
    pulp_tlb_read(pulp, &tlb_entry);
    printf("TLB readback Narrow: idx %ld first %012lx last %012lx base %012lx flags %02x\n", tlb_entry.idx,
          tlb_entry.first, tlb_entry.last, tlb_entry.base, tlb_entry.flags);
  }
  
  // De-isolate quadrant
  isolate_all(clusters, nr_dev, 1);
  ret = isolate_all(clusters, nr_dev, 0);
  if (ret) {
    isolate_all(clusters, nr_dev, 1);
    exit(-1);
  }

  // setup front-end server. Do this here so that the host communication data is before any other
  // data in L3 to prevent overwriting thereof
  //fesrv_t *fs = malloc(sizeof(fesrv_t));
  //fesrv_init(fs, pulp, &a2h_rb_addr);
  //pulp->l3l->a2h_rb = (uint32_t)(uintptr_t)a2h_rb_addr;

  // fill memory with known pattern
  if (memtest(pulp->l1.v_addr, pulp->l1.size, "TCDM", 'T'))
    return -1;

  printf("memtest l1 passed\n");

  wakeup_all(clusters,nr_dev);
  printf("Choped the Suey!\n");
    
  
  // and some test scratch l3 memory
  // For largest axpy problem: (2*N+1)*sizeof(double), N=3*3*6*2048
  // For largest conv2d problem: (64*112*112+64*64*7*7+64*112*112)*sizeof(double) = 14112*1024
  // 2x for good measure
  shared_l3_v = pulp_l3_malloc(pulp, 2 * 14112 * 1024, &addr);
  assert(shared_l3_v);
  pulp->l3l->heap = (uint32_t)(uintptr_t)addr;
  printf("alloc l3l_v->heap: %08x\r\n", pulp->l3l->heap);
  if (memtest(shared_l3_v, 1024, "L3", '3'))
    return -1;

  size = pulp_load_bin(pulp, argv[1]);
//  snprintf(shared_l3_v, 1024, "this is linux");
//
//  if (argc >= 2) {
//
//    printf("Data in allocated L3:\n");
//    hexdump((void *)shared_l3_v, 32);
//    printf("Data in L1:\n");
//    hexdump(pulp->l1.v_addr, 32);
//
//    // Fill some stuff in the mailbox
//    pulp_mbox_write(pulp, 100);
//    pulp_mbox_write(pulp, 101);
//    pulp_mbox_write(pulp, 102);
//
//    printf("Set interrupt on core %d\n", 1 + cluster_idx * 9);
//    pulp_ipi_set(pulp, 0, 1 << (1 + cluster_idx * 9));
//
//    printf("Waiting for program to terminate..\n");
//    fflush(stdout);
//    fs->abortAfter = 60;
//    fesrv_run(fs);
//    // sleep(3);
//
//    printf("Data in allocated L3:\n");
//    hexdump((void *)shared_l3_v, 32);
//    printf("Data in L1:\n");
//    hexdump(pulp->l1.v_addr, 6 * 0x10);
//
//    // read mailbox
//    uint32_t msg;
//    while (pulp_mbox_try_read(pulp, &msg)) {
//      printf("Mailbox: %d\n", msg);
//    }
//
//    pulp_ipi_get(pulp, 0, &mask);
//    printf("clint after completion: %08x\n", mask);
//  }
//


  printf("Exiting\n");
  return ret;
}
