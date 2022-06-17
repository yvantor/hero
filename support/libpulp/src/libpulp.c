/*
 * @Author: Noah Huetter
 * @Date:   2020-10-02 10:58:48
 * @Last Modified by:   Noah Huetter
 * @Last Modified time: 2020-11-09 17:09:55
 */
#include "libpulp.h"
#include "herodev.h"
#include "pulp_mbox.h"

#include "debug.h"
#include "fesrv.h"
#include "o1heap.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
//
//   Macros
//
// ----------------------------------------------------------------------------
#define read_csr(reg) ({ unsigned long __tmp;    \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  __tmp; })

#define ALIGN_UP(x, p) (((x) + (p)-1) & ~((p)-1))

enum log_level g_debuglevel = LOG_ERROR;
enum log_level g_pulp_debuglevel = LOG_ERROR;

// ----------------------------------------------------------------------------
//
//   Type declarations
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//
//   Static prototypes
//
// ----------------------------------------------------------------------------
static void populate_boot_data(pulp_dev_t *dev, struct BootData *bd);

// ----------------------------------------------------------------------------
//
//   Static Data
//
// ----------------------------------------------------------------------------
static uint64_t host_cnt;

// Need to share L3 resources accross clusters. They are mapped by the first mmap
// and then copied to to individual pulp devices (this is ugly but ok for now)
// Should be protected by a mutex and also, the properties are taken from the first
// cluster (as per design these are shared resources)
// TODO: Make not ugly
static PulpSubDev *g_l3 = NULL;
struct O1HeapInstance *g_l3_heap_mgr = NULL;
uint64_t g_l3_data_offset = 0;

// ----------------------------------------------------------------------------
//
//   Public functions
//
// ----------------------------------------------------------------------------

void pulp_set_log_level(enum log_level ll) { g_debuglevel = ll; }

int pulp_discover(char ***devs, uint32_t *clusters, uint32_t *quadrants) {
  int ret;
  glob_t globbuf;
  char **fname;
  int fd;
  ssize_t size;
  struct pulp_cluster_info pci;
  uint32_t cores = 0, dm_cores = 0, l_clusters, l_quadrants;
  uint8_t quadrant_present[256];
  ret = glob("/dev/pulp*", GLOB_ERR, NULL, &globbuf);
  glob("../*.c", GLOB_DOOFFS | GLOB_APPEND, NULL, &globbuf);

  l_clusters = 0;
  l_quadrants = 0;
  memset(quadrant_present, 0, sizeof(quadrant_present));

  if (ret) {
    if (ret == GLOB_NOMATCH)
      pr_info("No matches\n");
    else
      pr_trace("glob error: %s\n", strerror(errno));
    return -errno;
  }
  pr_info("Found %zu chardev matches\n", globbuf.gl_pathc);
  fname = globbuf.gl_pathv;

  if (devs)
    *devs = malloc(globbuf.gl_pathc * sizeof(char *));

  ret = 0;
  while (*fname) {
    // open device
    fd = open(*fname, O_RDWR | O_SYNC);
    if (fd < 0) {
      pr_error("Opening failed. %s \n", strerror(errno));
      goto skip;
    }

    // try reading PULP cluster struct
    size = read(fd, &pci, sizeof(pci));
    if (size != sizeof(pci)) {
      pr_error("Reading PULP cluster info failed\n");
      goto skip;
    }
    pr_info(" quadrant: %d cluster: %d computer-cores: %d dm-cores: %d l1: %ldKiB l3: %ldKiB\n",
            pci.quadrant_idx, pci.cluster_idx, pci.compute_num, pci.dm_num, pci.l1_size / 1024,
            pci.l3_size / 1024);

    if (devs)
      (*devs)[ret] = strdup(*fname);

    cores += pci.compute_num + pci.dm_num;
    dm_cores += pci.dm_num;
    // count number of quadrants
    if (pci.quadrant_idx < sizeof(quadrant_present) && !quadrant_present[pci.quadrant_idx]) {
      ++l_quadrants;
      quadrant_present[pci.quadrant_idx] = 1;
    } else if (pci.quadrant_idx > sizeof(quadrant_present)) {
      pr_error("Array overflow!\n");
    }
    ++l_clusters;

    ++ret;
  skip:
    close(fd);
    ++fname;
  }

  pr_info("Successfully probed %u clusters and found a total of %d (%d compute, %d dm) cores!\n",
          ret, cores, cores - dm_cores, dm_cores);

  if (clusters)
    *clusters = l_clusters;
  if (quadrants)
    *quadrants = l_quadrants;
  *quadrants = 0;
  return ret;
}

pulp_dev_t **pulp_mmap_all(uint32_t *nr_dev) {
  int ret, ndev;
  char **dev_names;
  pulp_dev_t **devs;
  uint32_t quadrants, clusters;

  ndev = pulp_discover(&dev_names, &clusters, &quadrants);
  if (ndev < 0) {
    *nr_dev = ndev;
    return NULL;
  }

  devs = malloc(ndev * sizeof(pulp_dev_t *));

  for (unsigned i = 0; i < ndev; ++i) {
    pr_info("Mapping dev: %s\n", dev_names[i]);
    devs[i] = malloc(sizeof(pulp_dev_t));
    ret = pulp_mmap(devs[i], dev_names[i]);

    if (ret < 0)
      goto fail;
  }
  *nr_dev = ndev;
  return devs;
fail:
  *nr_dev = ret;
  free(devs);
  return NULL;
}

int pulp_mmap(pulp_dev_t *dev, char *fname) {
  // int offset;
  ssize_t size;
  //  void *addr, *addr2;

  // This is probably the first call into this library, set the debug level from ENV here
  const char *s = getenv("LIBPULP_DEBUG");
  if (s != NULL) {
    g_debuglevel = atoi(s);
    pr_trace("Set debug level to %d\n", g_debuglevel);
  }

  pr_trace("mmap with %s\n", fname);

  // open device
  dev->fd = open(fname, O_RDWR | O_SYNC);
  if (dev->fd < 0) {
    pr_error("Opening failed. %s \n", strerror(errno));
    return -ENOENT;
  }

  // try reading pulp cluster struct
  size = read(dev->fd, &dev->pci, sizeof(dev->pci));
  if (size != sizeof(dev->pci)) {
    pr_error("Reading PULP cluster info failed\n");
    goto close;
  }
  pr_info("computer-cores: %d dm-cores: %d l1: %ldKiB l3: %ldKiB \n",
          dev->pci.compute_num, dev->pci.dm_num, dev->pci.l1_size / 1024, dev->pci.l3_size / 1024);

  // mmap tcdm
  dev->l1.size = dev->pci.l1_size;
  dev->l1.p_addr = dev->pci.l1_paddr;
  dev->l1.v_addr =
      mmap(NULL, dev->l1.size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, PULP_MMAP_TCDM);
  if (dev->l1.v_addr == MAP_FAILED) {
    pr_error("mmap() failed for TCDM. %s\n", strerror(errno));
    // return -EIO;
  }
  pr_debug("TCDM mapped to virtual user space at %p.\n", dev->l1.v_addr);

  // mmap reserved DDR space
  if (!g_l3) {
    g_l3 = malloc(sizeof(*g_l3));
    g_l3->size = dev->pci.l3_size;
    g_l3->p_addr = dev->pci.l3_paddr;
    g_l3->v_addr =
        mmap(NULL, g_l3->size, PROT_READ | PROT_WRITE, MAP_SHARED, dev->fd, PULP_MMAP_L3);
    if (g_l3->v_addr == MAP_FAILED) {
      pr_error("mmap() failed for Shared L3 memory. %s\n", strerror(errno));
      // return -EIO;
    }
    pr_debug("Shared L3 memory mapped to virtual user space at %p.\n", g_l3->v_addr);
  }
  memcpy(&dev->l3, g_l3, sizeof(*g_l3));

  // Initialize L3 heap manager in the middle of the reserved memory range
  if (!g_l3_heap_mgr) {
    g_l3_data_offset = ALIGN_UP(dev->l3.size / 2, O1HEAP_ALIGNMENT);
    pr_debug("Initializing o1heap at %p size %x\n", (void *)(dev->l3.v_addr + g_l3_data_offset),
             dev->l3.size / 2);
    g_l3_heap_mgr =
        o1heapInit((void *)(dev->l3.v_addr + g_l3_data_offset), dev->l3.size / 2, NULL, NULL);
    if (g_l3_heap_mgr == NULL) {
      pr_error("Failed to initialize L3 heap manager.\n");
      return -ENOMEM;
    } else {
      pr_debug("Allocated L3 heap manager at %p.\n", g_l3_heap_mgr);
    }
  }
  dev->l3_heap_mgr = g_l3_heap_mgr;
  dev->l3_data_offset = g_l3_data_offset;

  // Put a l3 layout struct into shared L3 to pass initial pointers to PULP
  // Pointer to this struct passed to PULP through scratch 2 register
  dev->l3l = pulp_l3_malloc(dev, sizeof(struct l3_layout), &dev->l3l_p);
  assert(dev->l3l);

  return 0;

close:
  close(dev->fd);
  return -ENOENT;
}

int pulp_flush(pulp_dev_t *dev) {
  // forward to driver
  return ioctl(dev->fd, PULPIOS_FLUSH);
}

int pulp_reset(pulp_dev_t *dev) {
  int ret;

  // Put clusters in isolation
  ret = pulp_isolate(dev, 1);
  if (ret)
    return ret;

  // de-isolate
  ret = pulp_isolate(dev, 0);
  if (ret)
    return ret;

  return 0;
}

int pulp_t_start(pulp_dev_t *dev) {

  int ret;
  ret = ioctl(dev->fd, PULPIOT_START_T);
  asm volatile("": : :"memory");

  return ret;
}

int pulp_exe_start(pulp_dev_t *dev, uint32_t boot_addr) {
  uint32_t ret;

  pulp_t_start(dev);
  
  pr_trace("Write boot address\n");
  for(int i = 0; i<8; i++) {
    ret = pulp_periph_reg_write(dev, CPER_RI5CY_BOOTADDR0 + (i*0x4), (uint32_t)boot_addr);
    if(ret)
      return ret;
  }
  
  pr_trace("Fetch enable cores\n");
  ret = pulp_periph_reg_write(dev,CPER_CONTROLUNIT_FE,(uint32_t)0xff);
  if(ret)
    return ret;
  
  return 0;
}
  
int pulp_load_bin(pulp_dev_t *dev, const char *name) {
  FILE *fd;
  size_t size;
  int ret;
  uint8_t *dat;
  unsigned errs = 0;
  uint32_t boot_data_off;
  struct BootData *bd;

  ret = access(name, R_OK);
  if (ret) {
    pr_error("Can't access file %s: %s\n", name, strerror(ret));
    return ret;
  }

  fd = fopen(name, "rb");
  if (!fd) {
    pr_error("File %s open failed: %s\n", name, strerror(errno));
    return errno;
  }

  // get file size and check if it fits L3
  fseek(fd, 0L, SEEK_END);
  size = ftell(fd);
  fseek(fd, 0L, SEEK_SET);
  if (size > dev->l3.size) {
    pr_error("binary exceeds L3 size: %ld > %d\n", size, dev->l3.size);
    ret = -EFBIG;
    goto abort;
  }

  pr_trace("copy binary %s to L3\n", name);
  fread(dev->l3.v_addr, size, 1, fd);
  //pulp_flush(dev);

  // testing: read file to memory and compare with L3
  dat = (uint8_t *)malloc(size);
  fseek(fd, 0L, SEEK_SET);
  fread(dat, size, 1, fd);
  for (unsigned i = 0; errs < 8 && i < size; ++i)
    errs = ((uint8_t *)dev->l3.v_addr)[i] != dat[i] ? errs + 1 : errs;
  pr_trace("Verify accelerator binary in L3: %s %d errors (of %ld bytes)\n",
           errs != 0 ? SHELL_RED "FAIL" SHELL_RST : SHELL_GRN "PASS" SHELL_RST, errs, size);
  free(dat);
  if (errs) {
    pr_error("Verify failed\n");
    ret = -EIO;
    goto abort;
  } else {
    ret = size;
  }


  // Program boot-rom and set pointer to it in scratch1
  boot_data_off = ALIGN_UP(size, 0x100);

  bd = malloc(sizeof(*bd));
  if (!bd) {
    pr_error("error allocating boot data: %s\n", strerror(errno));
    ret = -ENOMEM;
    goto abort;
  }
  populate_boot_data(dev, bd);

  memcpy((uint8_t *)dev->l3.v_addr + boot_data_off, bd, sizeof(*bd));
  free(bd);

  // ri5cy's fetch enable
  ret = pulp_exe_start(dev,(uint32_t)dev->l3.p_addr);
  
  return ret;

abort:
  fclose(fd);
  return ret;
}

int pulp_isolate(pulp_dev_t *dev, int iso) {
  // Forward to the driver
  uint32_t cmd;
  int ret;
  if (iso)
    cmd = PULPIOS_ISOLATE;
  else
    cmd = PULPIOS_DEISOLATE;
  if ((ret = ioctl(dev->fd, PULPIOC_SET_OPTIONS, &cmd))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  } else {
    pr_debug("%sisolate success on quadrant %d\n", iso ? "" : "de", dev->pci.quadrant_idx);
  }
  return ret;
}

int pulp_wakeup(pulp_dev_t *dev) {
  // Forward to the driver
  uint32_t cmd;
  int ret;
  cmd = PULPIOS_WAKEUP;
  if ((ret = ioctl(dev->fd, PULPIOC_SET_OPTIONS, &cmd))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  } else {
    pr_debug("Wakeup success on quadrant %d\n", dev->pci.quadrant_idx);
  }

  return ret;
}

int pulp_periph_reg_write (pulp_dev_t *dev, uint32_t reg, uint32_t val) {
  int ret;
  struct pulpios_reg sreg;
  sreg.off = reg;
  sreg.val = val;
  if ((ret = ioctl(dev->fd, PULPIOC_PERIPH_W, &sreg))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  return ret;
}
       
int pulp_periph_reg_read(pulp_dev_t *dev, uint32_t reg) {
  int ret;
  struct pulpios_reg sreg;
  sreg.off = reg;
  if ((ret = ioctl(dev->fd, PULPIOC_PERIPH_R, &sreg))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
    return ret;
  }
  return sreg.val;
}

int pulp_scratch_reg_write(pulp_dev_t *dev, uint32_t reg, uint32_t val) {
  int ret;
  struct pulpios_reg sreg;
  sreg.off = reg;
  sreg.val = val;
  if ((ret = ioctl(dev->fd, PULPIOS_SCRATCH_W, &sreg))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  return ret;
}

int pulp_scratch_reg_read(pulp_dev_t *dev, uint32_t reg, uint32_t *val) {
  int ret;
  struct pulpios_reg sreg;
  sreg.off = reg;
  if ((ret = ioctl(dev->fd, PULPIOS_SCRATCH_R, &sreg))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  return ret;
}

int pulp_tlb_write(pulp_dev_t *dev, struct axi_tlb_entry *e) {
  int ret;

  pr_trace("TLB write slice: loc %s idx %ld first %012lx last %012lx base %012lx flags %02x\n",
           e->loc == AXI_TLB_NARROW ? "narrow" : "wide", e->idx, e->first, e->last, e->base,
           e->flags);

  if ((ret = ioctl(dev->fd, PULPIOS_WRITE_TLB_ENTRY, e))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  return ret;
}

int pulp_tlb_read(pulp_dev_t *dev, struct axi_tlb_entry *e) {
  int ret;
  if ((ret = ioctl(dev->fd, PULPIOS_READ_TLB_ENTRY, e))) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  return ret;
}

void pulp_fesrv_run(pulp_dev_t *dev) {
  pr_error("unimplemented\n");
  // // init fesrv
  // dev->fesrv.hsmem = dev->bram.v_addr;
  // dev->fesrv.dmem = dev->l1.v_addr;
  // fesrvInit(&dev->fesrv, PULP_TOT_NR_CORES);
  // dev->fesrv.dataOffset = 0;
  // dev->fesrv.dmemSize = H_l1_SIZE_B;
  // // do not autoabort
  // dev->fesrv.runFor = 0;

  // // call polling thread
  // fesrvRun(&dev->fesrv);
}

int pulp_read_performance(pulp_dev_t *dev, pulp_perf_t *perf) {
  pr_error("unimplemented\n");
  return -1;
  // perf->counters = malloc(PULP_PERIPH_PERF_CNT_PER_CORE * PULP_TOT_NR_CORES *
  // sizeof(uint32_t));

  // if (!perf->counters) {
  //   printf("Memory allocation failed\n");
  //   return -1;
  // }

  // uint64_t *p = (uint64_t *)dev->periph.v_addr;

  // perf->l1StartAddress = p[PULP_PERIPH_TCDM_START / 8];
  // perf->l1EndAddress = p[PULP_PERIPH_TCDM_END / 8];
  // perf->nrCores = p[PULP_PERIPH_NR_CORES / 8];
  // perf->fetchEnable = p[PULP_PERIPH_FETCH_ENA / 8];
  // perf->scratch = p[PULP_PERIPH_SCRATCH_REG / 8];
  // perf->cycle = p[PULP_PERIPH_CYCLE_COUNT / 8];
  // perf->barrier = p[PULP_PERIPH_BARRIER / 8];
  // perf->hartBaseID = p[PULP_PERIPH_CLUSTER_ID / 8];
  // perf->l1Accessed = p[PULP_PERIPH_TCDM_ACCESSED / 8];
  // perf->l1Congested = p[PULP_PERIPH_TCDM_CONGESTED / 8];

  // for (int i = 0; i < PULP_PERIPH_PERF_CNT_PER_CORE * PULP_TOT_NR_CORES; i++)
  //   perf->counters[i] = p[PULP_PERIPH_PERF_CNT / 8 + i];

  // return PULP_PERIPH_PERF_CNT_PER_CORE * PULP_TOT_NR_CORES;
}

void *pulp_l3_malloc(pulp_dev_t *dev, size_t size, void **p_addr) {
  // Align
  if (size & 0x7) {
    size = (size & ~0x7) + 0x8;
  }

  void *v_addr = o1heapAllocate((O1HeapInstance *)dev->l3_heap_mgr, size);
  if (v_addr == 0) {
    return 0;
  }

  // Calculate physical address.
  *p_addr = dev->l3.p_addr + (v_addr - dev->l3.v_addr);
  pr_trace("pulp_l3_malloc: size: %lx virt: %p phys: %p\n", size, v_addr, *p_addr);

  return v_addr;
}

void pulp_l3_free(pulp_dev_t *dev, void *v_addr, void *p_addr) {
  o1heapFree((O1HeapInstance *)dev->l3_heap_mgr, v_addr);
}

int pulp_exec_done(pulp_dev_t *dev, uint64_t *mask) {
  pr_error("unimplemented\n");
  return -1;
  // int ret = 0;
  // uint64_t imask = 0;
  // for (int i = 0; i < dev->fesrv.nrCores; i++) {
  //   ret += (dev->fesrv.coresExited[i] == 1) ? 1 : 0;
  //   imask |= ((dev->fesrv.coresExited[i] == 1) ? 1 : 0) << i;
  // }
  // if (mask)
  //   *mask = imask;
  // return ret;
}

int pulp_exe_wait(pulp_dev_t *dev, int timeout_s) {
  int ret;
  struct pulpiot_val values;
  values.timeout = timeout_s * 1000;
  ret = ioctl(dev->fd, PULPIOT_WAIT_EOC, &values);

  if (ret<0) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  else if (ret==0) {
    pr_warn("pulp_exe_wait timed out\n");
  }

  host_cnt = values.counter;
  
  return ret;
}

void pulp_dbg_stack(pulp_dev_t *dev, uint32_t stack_size, char fill) {
  pr_error("unimplemented\n");
}

int pulp_mbox_set_irq(pulp_dev_t *dev, uint32_t dir, uint32_t ewr) {
  int ret = 0;
  switch(dir) {
    case H2C_DIR: {
      ret = pulp_periph_reg_write(dev,MBOX_H2C_BASE_ADDR+MBOX_IRQEN_OFFSET,1<<ewr);
      return ret;
    }
    case C2H_DIR: {
      ret = pulp_periph_reg_write(dev,MBOX_C2H_BASE_ADDR+MBOX_IRQEN_OFFSET,1<<ewr);
      return ret;
    }
    default: {
      ret = -1;
      return ret;
    }
  }
 return ret;
}

int pulp_mbox_clear_irq(pulp_dev_t *dev, uint32_t dir, uint32_t ewr) {
  int ret = 0 ;
  switch(dir) {
    case H2C_DIR: {
      ret = pulp_periph_reg_write(dev,MBOX_H2C_BASE_ADDR+MBOX_IRQS_OFFSET,1<<ewr);
      return ret;
    }
    case C2H_DIR: {
      ret = pulp_periph_reg_write(dev,MBOX_C2H_BASE_ADDR+MBOX_IRQS_OFFSET,1<<ewr);
      return ret;
    }
    default: {
      ret = -1;
      return ret;
    }
  }
  return ret;
}
  
int pulp_mbox_read(pulp_dev_t *dev, uint32_t *buffer, size_t n_words) {
  int retry = 0;
  for (int i=0;i<n_words;i++) {
    
    while(pulp_mbox_try_read(dev)==0) {
      retry++;
      if (++retry == 100) {
        pr_warn("high retry on mbox read()\n");
        retry = 0;
      }
    }
    buffer[i]=pulp_periph_reg_read(dev,MBOX_C2H_BASE_ADDR+MBOX_RDDATA_OFFSET);
    i++;
  }
  
  return 0;
}

int pulp_mbox_try_read(pulp_dev_t *dev) {
  return ( pulp_periph_reg_read(dev,MBOX_C2H_BASE_ADDR+MBOX_STATUS_OFFSET) && MBOX_RFIFO_MASK_EMPTY ) ;
}

int pulp_mbox_try_write(pulp_dev_t *dev) {
  return ( pulp_periph_reg_read(dev,MBOX_H2C_BASE_ADDR+MBOX_STATUS_OFFSET) && MBOX_WFIFO_MASK_FULL ) ;
}

int pulp_mbox_write(pulp_dev_t *dev, uint32_t word) {
  pr_trace("mbox write %08x\n", word);
  int ret = 0;
  int retry = 0;
  while(pulp_mbox_try_read(dev)!=0) {
    retry++;
    if (++retry == 100) {
      pr_warn("high retry on mbox write()\n");
      retry = 0;
    }
    usleep(10000);
  }
  pulp_periph_reg_write(dev,MBOX_H2C_BASE_ADDR+MBOX_WRDATA_OFFSET,word);
  return ret;
}

void pulp_set_device_loglevel(pulp_dev_t *dev, int lvl) {
  // the default
  g_pulp_debuglevel = 0;
  if (lvl == -1) {
    const char *s = getenv("PULP_DEBUG");
    if (s != NULL) {
      g_pulp_debuglevel = atoi(s);
    }
  } else {
    g_pulp_debuglevel = lvl;
  }
  pr_trace("Set pulp debug level to %d\n", g_pulp_debuglevel);
  pulp_mbox_write(dev, MBOX_DEVICE_LOGLVL);
  pulp_mbox_write(dev, g_pulp_debuglevel);
}

int pulp_mbox_wait(pulp_dev_t *dev, int timeout_s) {

  int ret;
  struct pulpiot_val values;
  values.timeout = timeout_s * 1000;
  ret = ioctl(dev->fd, PULPIOT_WAIT_MBOX, &values);

  if (ret<0) {
    pr_error("ioctl() failed. %s \n", strerror(errno));
  }
  else if (ret==0) {
    pr_warn("pulp_mbox_wait timed out\n");
  }

  host_cnt = values.counter;
  
  return ret;
}

uint64_t pulp_get_exe_time() {
  return host_cnt;
}

// ----------------------------------------------------------------------------
//
//   Static
//
// ----------------------------------------------------------------------------

static void populate_boot_data(pulp_dev_t *dev, struct BootData *bd) {
  bd->core_count = dev->pci.compute_num + dev->pci.dm_num;
  // hartid of first pulp core
  bd->hartid_base = 1;
  // Global cluster base address. Each cluster's TCDM is calculated using this offset,
  // tcdm_offset, hartid_base and mhartid CSR
  bd->tcdm_start = 0x10000000;
  // size of TCDM address space
  bd->tcdm_size = 0x20000;
  // offset between cluster address spaces
  bd->tcdm_offset = 0x40000;
  bd->global_mem_start = (uint32_t)(uintptr_t)(dev->pci.l3_paddr + dev->l3_data_offset);
  bd->global_mem_end = (uint32_t)(uintptr_t)(dev->pci.l3_paddr + dev->l3.size);
  // TODO: Handle multi-cluster
  bd->cluster_count = 1;
  // Let's pretend all clusters were in the same quadrant
  bd->s1_quadrant_count = 1;
  // unused
  bd->boot_addr = (uint32_t)(uintptr_t)dev->pci.l3_paddr;
}
