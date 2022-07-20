#include <asm/io.h>       /* ioremap, iounmap, iowrite32 */
#include <asm/uaccess.h>  /* for put_user */
#include <linux/cdev.h>   /* cdev struct */
#include <linux/delay.h>  // sleep
#include <linux/device.h> // class_create, device_create
#include <linux/interrupt.h> /* interrupt handling */
#include <linux/kernel.h> /* Needed for KERN_INFO */
#include <linux/mm.h>     /* vm_area_struct struct, page struct, PAGE_SHIFT, page_to_phys */
#include <linux/module.h> /* Needed by all modules */
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h> /* Necessary because we use the proc fs */
#include <linux/spinlock.h>
#include "pulp_module.h"
#include "pulp_reg.h"

// ----------------------------------------------------------------------------
//
//   Module properties
//
// ----------------------------------------------------------------------------

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luca Valente");
MODULE_DESCRIPTION("PULP driver");

/* Match table for of_platform binding */
static const struct of_device_id pulp_of_match[] = {
    {
        .compatible = "eth,alsaqr",
    },
    {/* sentinel */},
};
MODULE_DEVICE_TABLE(of, pulp_of_match);

#define dbg(...)  printk(KERN_DEBUG "PULP Debug: " __VA_ARGS__)
#define info(...) printk(KERN_INFO "PULP Info: " __VA_ARGS__)
#define warn(...) printk(KERN_WARNING "PULP Warning: " __VA_ARGS__)

#define DEVICE_NAME "pulp"
#define CLASS_NAME "pulp"

#define read_csr(reg) ({ unsigned long __tmp; \
  asm volatile ("": : :"memory"); \
  asm volatile ("csrr %0, " #reg : "=r"(__tmp)); \
  asm volatile ("": : :"memory"); \
  __tmp; })

// VM_RESERVERD for mmap
#ifndef VM_RESERVED
#define VM_RESERVED (VM_DONTEXPAND | VM_DONTDUMP)
#endif

// AXI TLBs on PULP quadrant port translate 64-bit addresses, thus contain 3 uint64_t
// (first, last, base) and 1 uint32_t (flags) register
#define TLB_ENTRY_BYTES (3 * 8 + 1 * 4)

/**
 * Representation of a generic shared memory-region
 * @pbase: Physical address base
 * @vbase: Kernel virtual address base
 */
struct shared_mem {
  phys_addr_t pbase;
  void __iomem *vbase;
  resource_size_t size;
};

/**
 * struct pulp_cluster - Internal representation of a pulp cluster
 * @dev: Pointer to device structure
 * @pbase: peripherals base
 * @soc_regs: kernel-mapped soc-control registers
 * @l1: TCDM memory
 * @l2: L2 SCP memory
 * @l3: Shared L3 memory
 * @pci: PULP cluster info, shared with userspace on read
 * @list: Links it to the global pc_list
 * @minor: Minor device number in /dev
 * @fops: Copy of a pointer to the file operations
 * @nodename: nodename of the chardev
 * @mode: mode of the chardev
 * @this_device: the chardev
 * @quadrant_ctrl: handle to the associated quadrant controller
 */
struct pulp_cluster {
  struct device *dev;
  void __iomem *pbase;
  struct shared_mem l1;
  struct shared_mem l2;
  struct shared_mem l3;
  struct pulp_cluster_info pci;
  struct list_head list;
  int minor;
  int irq_mbox;
  int irq_eoc;
  const struct file_operations *fops;
  const char *nodename;
  umode_t mode;
  struct device *this_device;
  struct quadrant_ctrl *quadrant_ctrl;
};

struct pulp_dev {
  struct class *class;
  struct cdev cdev;
  struct device *pDev;
  int major;
};

struct quadrant_ctrl {
  u32  quadrant_idx;
  void __iomem *regs;
  struct list_head list;
};

// ----------------------------------------------------------------------------
//
//   Static function declaration
//
// ----------------------------------------------------------------------------

static void     set_isolation                    (struct pulp_cluster *pc, int iso);
static int      isolate                          (struct pulp_cluster *pc);
static int      deisolate                        (struct pulp_cluster *pc);
static int      wakeup                           (struct pulp_cluster *pc);
static void     cluster_periph_write             (struct pulp_cluster *pc, uint32_t reg_off, uint32_t val);
static uint32_t cluster_periph_read              (struct pulp_cluster *pc, uint32_t reg_off);
static uint32_t get_isolation                    (uint32_t quadrant);
static void     soc_reg_write                    (uint32_t reg_off, uint32_t val);
static uint32_t soc_reg_read                     (uint32_t reg_off);
static void     timer_reg_write                  (uint32_t reg_off, uint32_t val);
static uint32_t timer_reg_read                   (uint32_t reg_off);
static void     quadrant_ctrl_reg_write          (struct quadrant_ctrl *qc, uint32_t reg_off, uint32_t val);
static uint32_t quadrant_ctrl_reg_read           (struct quadrant_ctrl *qc, uint32_t reg_off);
static struct   quadrant_ctrl *get_quadrant_ctrl (u32 quadrant_idx);
static int      write_tlb                        (struct pulp_cluster *pc, struct axi_tlb_entry *tlbe);
static int      read_tlb                         (struct pulp_cluster *pc, struct axi_tlb_entry *tlbe);
static irqreturn_t pulp_isr                      (int irq, void *ptr);

// ----------------------------------------------------------------------------
//
//   Static data
//
// ----------------------------------------------------------------------------

static struct pulp_dev pulp_dev;

/**
 * @brief A list containing all pointers to registers pulp cluster `struct pulp_cluster` structs
 */
static LIST_HEAD(pc_list);
/**
 * @brief A list containing all pointers to quadrant controllers `struct quadrant_ctrl` structs
 */
static LIST_HEAD(quadrant_ctrl_list);
/**
 * @brief Protect the pc_list
 *
 */
static DEFINE_MUTEX(pulp_mtx);
/**
 * @brief To check the job's completion
 *
 */
static DECLARE_COMPLETION(ctrl_finished);
/**
 * @brief To check the mbox status
 *
 */
static DECLARE_COMPLETION(mbox_finished);
/**
 * @brief Cycle # when the cluster ends
 *
 */
static uint32_t host_cycles_end;

// ----------------------------------------------------------------------------
//
//   "Shared" Data
//
// ----------------------------------------------------------------------------

/**
 * @soc_lock: Protects the soc-reg resources
 */
spinlock_t soc_lock;
/**
 * soc-regs are ioremapped by the first module probe
 *
 * @soc_regs: kernel-mapped soc-control registers
 */
void __iomem *soc_regs;

/**
 * @soc_lock: Protects the timer-reg resources
 */
spinlock_t timer_lock;
/**
 * timer-regs are ioremapped by the first module probe
 *
 * @timer_regs: kernel-mapped apb timer registers
 */
void __iomem *timer_regs;

// ----------------------------------------------------------------------------
//
//   SYSFS
//
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
//
//   File OPs
//
// ----------------------------------------------------------------------------

/*
 * Called when a process tries to open the device file, like
 * "cat /dev/mycharfile"
 */
static int pulp_open(struct inode *inode, struct file *file) {
  int minor = iminor(inode);
  struct pulp_cluster *pc;
  int err = -ENODEV;

  // mutex_lock(&pulp_mtx);

  list_for_each_entry(pc, &pc_list, list) {
    if (pc->minor == minor) {
      /*
       * Place the mipcdevice in the file's private_data so it can be used by the
       * file operations
       */
      file->private_data = pc;
      break;
    }
  }

  if (!file->private_data) {
    goto fail;
  }

  err = 0;
  dbg("cluster %d opened\n", pc->minor);

fail:
  // mutex_unlock(&pulp_mtx);
  return err;
}

/*
 * Called when a process closes the device file.
 */
static int pulp_release(struct inode *inode, struct file *file) {
  struct pulp_cluster *pc;
  pc = file->private_data;
  dbg("cluster %d released\n", pc->minor);
  return 0;
}

/*
 * Called when a process, which already opened the dev file, attempts to
 * read from it. If size is correct, returns a copy of the pulp_cluster_info struct
 */
static ssize_t pulp_read(struct file *file, char __user *buffer, size_t length, loff_t *offset) {
  struct pulp_cluster *pc;
  pc = file->private_data;

  // only support reads of size sizeof(struct pulp_cluster_info)
  if (length != sizeof(struct pulp_cluster_info)) {
    info(KERN_ALERT "Sorry, this operation isn't supported.\n");
    return -EINVAL;
  }

  if (copy_to_user(buffer, &pc->pci, sizeof(struct pulp_cluster_info)) != 0)
    return -EFAULT;

  return sizeof(struct pulp_cluster_info);
}

/*
 * Called when a process writes to dev file: echo "hi" > /dev/hello
 */
static ssize_t pulp_write(struct file *file, const char *buff, size_t len, loff_t *off) {
  info(KERN_ALERT "Sorry, this operation isn't supported.\n");
  return -EINVAL;
}

irqreturn_t pulp_isr(int irq, void *dev)
{

  struct pulp_cluster *pc;

  pc = dev;

  asm volatile ("": : :"memory");
  host_cycles_end = timer_reg_read(TIMER_COUNTER);
  asm volatile ("": : :"memory");

  dbg("PULP: Handling IRQ %0d.\n", irq);

  if(irq == pc->irq_mbox) {
    complete(&mbox_finished);
  }
  else if (irq == pc->irq_eoc) {
    complete(&ctrl_finished);
  }
  else {
    printk(KERN_WARNING "PULP: Cannot handle interrupt %d, cannot be mapped to to identifier\n", irq);
    return IRQ_NONE;
  }

  return IRQ_HANDLED;
}

static long pulp_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  void __user *argp = (void __user *)arg;
  int __user *p = argp;
  struct pulpios_reg sreg;
  struct pulpiot_val values;
  struct axi_tlb_entry tlbe;
  struct pulp_cluster *pc;
  int i = 0;
  int timed_out;
  pc = file->private_data;

  // check correct magic
  if (_IOC_TYPE(cmd) != PULPIOC_MAGIC)
    return -ENOTTY;

  dbg("ioctl with cmd %d arg %ld\n", cmd, arg);

  // Switch according to the ioctl called
  switch (cmd) {
  case PULPIOC_SET_OPTIONS: {
    int options, retval = -EINVAL;
    if (get_user(options, p))
      return -EFAULT;

    switch (options) {
      case(PULPIOS_ISOLATE):    {
        retval = isolate(pc);
        return 0;
      }
      case(PULPIOS_DEISOLATE):  {
        retval = deisolate(pc);
        return 0;
      }
      case(PULPIOS_WAKEUP):     {
        reinit_completion(&ctrl_finished);
        reinit_completion(&mbox_finished);
        retval = wakeup(pc);
        return 0;
      }
      default:
        return -EFAULT;

    }

    return retval;
  }
 case PULPIOS_SCRATCH_W: {
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    // Sanitize to 1 pcratch registers
    if (sreg.off > 1)
      return -EINVAL;
    dbg("pcratch write reg %d val %#x\n", sreg.off, sreg.val);
    soc_reg_write(sreg.off, sreg.val);
    return 0;
  }
  case PULPIOS_SCRATCH_R: {
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    // Sanitize to 4 pcratch registers
    if (sreg.off > 1)
      return -EINVAL;
    sreg.val = soc_reg_read(sreg.off);
    dbg("pcratch read reg %d val %#x\n", sreg.off, sreg.val);
    if (copy_to_user(p, &sreg, sizeof(sreg)))
      return -EFAULT;
    return 0;
  }
  case PULPIOS_READ_ISOLATION: {
    uint32_t quadrant;
    if (get_user(quadrant, p))
      return -EFAULT;
    return get_isolation(quadrant);
  }
  case PULPIOS_SET_IPI: {
    return -EFAULT;
  }
  case PULPIOS_GET_IPI: {
    return -EFAULT;
  }
  case PULPIOS_CLEAR_IPI: {
    return -EFAULT;
  }
  case PULPIOS_FLUSH: {
    asm volatile("fence");
    return 0;
  }
  case PULPIOS_WRITE_TLB_ENTRY: {
    if (copy_from_user(&tlbe, p, sizeof(tlbe)))
      return -EFAULT;
    return write_tlb(pc, &tlbe);
  }
  case PULPIOS_READ_TLB_ENTRY: {
    int ret;
    if (copy_from_user(&tlbe, p, sizeof(tlbe)))
      return -EFAULT;
    ret = read_tlb(pc, &tlbe);
    if (copy_to_user(p, &tlbe, sizeof(tlbe)))
      return -EFAULT;
    return ret;
  }
  case PULPIOC_PERIPH_W: { 
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    dbg("c periph write write reg %d val %#x\n", sreg.off, sreg.val);
    cluster_periph_write(pc,sreg.off, sreg.val);
    return 0;
  }
  case PULPIOC_PERIPH_R: { 
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    dbg("c periph read @ reg %d \n", sreg.off);
    sreg.val = cluster_periph_read(pc,sreg.off);
    if (copy_to_user(p, &sreg, sizeof(sreg)))
      return -EFAULT;
    return 0;
  }
  case PULPIOC_PERIPH_START: { 
    host_cycles_end = 0;
    //reset the counter and enable
    timer_reg_write(TIMER_COUNTER,0);
    timer_reg_write(TIMER_CTRL,1);
    asm volatile ("": : :"memory"); 
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    for(i = 0; i<8; i++) {
      cluster_periph_write(pc,CPER_RI5CY_BOOTADDR0+(i*4), sreg.val);
    }
    wakeup(pc);
    cluster_periph_write(pc,CPER_CONTROLUNIT_FE, (uint32_t) 0xff);
    return 0;
  }
  case PULPIOC_QUADRANT_W: { 
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    dbg("c quadrant write write reg %d val %#x\n", sreg.off, sreg.val);
    quadrant_ctrl_reg_write(pc->quadrant_ctrl,sreg.off,sreg.val);
    return 0;
  }
  case PULPIOC_QUADRANT_R: { 
    if (copy_from_user(&sreg, p, sizeof(sreg)))
      return -EFAULT;
    sreg.val = quadrant_ctrl_reg_read(pc->quadrant_ctrl,sreg.off);
    dbg("c quadrant read @ reg %x : %x\n", sreg.off, sreg.val);
    if (copy_to_user(p, &sreg, sizeof(sreg)))
      return -EFAULT;
    return 0;
  }
  case(PULPIOT_WAIT_MBOX): {
    if (copy_from_user(&values, p, sizeof(values)))
      return -EFAULT;
    timed_out = wait_for_completion_timeout(&mbox_finished,values.timeout * HZ / 1000);
    info("time count: %u\n", host_cycles_end);
    return host_cycles_end;
  }
  case(PULPIOT_WAIT_EOC): {
    if (copy_from_user(&values, p, sizeof(values)))
      return -EFAULT;
    timed_out = wait_for_completion_timeout(&mbox_finished,values.timeout * HZ / 1000);
    info("time count: %u\n", host_cycles_end);
    return host_cycles_end;
  }
  case(PULPIOT_GET_T): {
    values.counter = host_cycles_end ;
    printk(KERN_DEBUG "PULP: counter @ cycle %u = %u\n", host_cycles_end , (uint32_t) values.counter);    
    if (copy_to_user(p, &values, sizeof(values)))
      return -EFAULT;
    return 0;
  }
  default:
    return -ENOTTY;
  }

  return 0;
}

/**
 * @brief memory map to user-space. The module's address map is contiguous
 * refer to the PULP_MMAP_x_BASE() macros for address calculation
 *
 */
int pulp_mmap(struct file *file, struct vm_area_struct *vma) {
  unsigned long mapoffset, vsize, psize;
  char type[20];
  int ret;
  struct pulp_cluster *pc;
  pc = file->private_data;

  dbg("mmap with offset %#lx\n", vma->vm_pgoff);

  switch (vma->vm_pgoff) {
  case 0:
    strncpy(type, "l3", sizeof(type));
    mapoffset = pc->l3.pbase;
    psize = pc->l3.size;
    break;
  case 1:
    strncpy(type, "l1", sizeof(type));
    mapoffset = pc->l1.pbase;
    psize = pc->l1.size;
    break;
  case 2:
    strncpy(type, "l2", sizeof(type));
    mapoffset = pc->l2.pbase;
    psize = pc->l2.size;
    break;
  default:
    return -EINVAL;
  }

  vsize = vma->vm_end - vma->vm_start;
  if (vsize > psize) {
    dbg("error: vsize %ld > psize %ld\n", vsize, psize);
    dbg("  vma->vm_end %lx vma->vm_start %lx\n", vma->vm_end, vma->vm_start);
    return -EINVAL;
  }

  // set protection flags to avoid caching and paging
  vma->vm_flags |= VM_IO | VM_RESERVED;
  vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

  dbg("%s mmap: phys: %#lx, virt: %#lx vsize: %#lx psize: %#lx\n", type, mapoffset, vma->vm_start,
      vsize, psize);

  ret = remap_pfn_range(vma, vma->vm_start, mapoffset >> PAGE_SHIFT, vsize, vma->vm_page_prot);

  if (ret)
    dbg("mmap error: %d\n", ret);

  return ret;
}

int pulp_flush(struct file *file, fl_owner_t id) {
  // unsigned long mapoffset, vsize, psize;
  // char type[20];
  // int ret;
  // struct pulp_cluster *pc;
  // pc = file->private_data;

  dbg("flush\n");
  return 0;
}

struct file_operations pulp_fops = {
    .owner = THIS_MODULE,
    .open = pulp_open,
    .read = pulp_read,
    .write = pulp_write,
    .release = pulp_release,
    .unlocked_ioctl = pulp_ioctl,
    .mmap = pulp_mmap,
    .flush = pulp_flush,
};

// ----------------------------------------------------------------------------
//
//   Statics
//
// ----------------------------------------------------------------------------
/**
 * @brief Set the reset of the quadrant
 *
 * @param reset 1 to assert reset, 0 de-assert reset
 */
static void set_reset(struct pulp_cluster *pc, int reset) {

  uint32_t scw;

  dbg("set_reset cluster %d %s\n", pc->quadrant_ctrl->quadrant_idx,
      reset ? "ASSERT" : "DE-ASSERT");

  scw=1;
  if(reset==1)
    scw=0;
  
  // Active-low reset
  soc_reg_write(0,scw);
  dbg("soc_reg: %x\n", soc_reg_read(0));
}

/**
 * @brief Set the isolation of the cluster in the soc-control register
 *
 * @param iso 1 to isolate, 0 to de-isolate
 */
static void set_isolation(struct pulp_cluster *pc, int iso) {

  set_reset(pc, iso);

}

/**
 * @brief Isolate a quadrant by first setting the isolation and after succesful isolation
 * (isolated = 0xf), put quadrant in reset. If isolation does not succeed, does not reset the
 * quadrant and returns ETIMEOUT
 */
static int isolate(struct pulp_cluster *pc) {
  unsigned quadrant_id = pc->pci.quadrant_idx;
  u32 timeout = 1000; // [x10 us]
  uint32_t iso;

  set_isolation(pc, 1);
  do {
    iso = get_isolation(quadrant_id);
    if (iso != 0x0)
      udelay(10);
  } while (iso != 0x0 && --timeout);

  if (iso != 0x0) {
    set_reset(pc, 1);
    return -ETIMEDOUT;
  }
  return 0;
}

/**
 * @brief Release reset and deisolate quadrant. On success (isolated = 0) return 0, on fail (isolate
 * != 0) put quadrant into reset and isolate, return -ETIMEDOUT
 */
static int deisolate(struct pulp_cluster *pc) {
  unsigned quadrant_id = pc->pci.quadrant_idx;
  u32 timeout = 1000; // [x10 us]
  uint32_t iso;

  set_reset(pc, 0);
  set_isolation(pc, 0);
  do {
    iso = get_isolation(quadrant_id);
    if (iso != 0x1)
      udelay(10);
  } while (iso != 0x1 && --timeout);

  if (iso != 0x1) {
    set_isolation(pc, 1);
    set_reset(pc, 1);
    return -ETIMEDOUT;
  }

  return 0;
}


/**
 * @brief Enable clock and fetch enable of the cluster
 *
 * @param quadrant quadrant index to read isolation bits from
 */
static int wakeup(struct pulp_cluster *pc) {
  unsigned quadrant_id = pc->pci.quadrant_idx;
  u32 timeout = 1000; // [x10 us]
  uint32_t iso;
  
  soc_reg_write(0,0x3);
  do {
    iso = get_isolation(quadrant_id);
    if (iso != 0x3)
      udelay(10);
  } while (iso != 0x3 && --timeout);

  soc_reg_write(0,0x7);
  do {
    iso = get_isolation(quadrant_id);
    if (iso != 0x7)
      udelay(10);
  } while (iso != 0x7 && --timeout);

  if (iso != 0x7) {
    info("Something went wrong with the wakeup\n");
    set_reset(pc, 1);
    return -ETIMEDOUT;
  }

  return 0;
}

/**
 * @brief Read isolation bits of quadrant `quadrant`
 *
 * @param quadrant quadrant index to read isolation bits from
 */
static uint32_t get_isolation(uint32_t quadrant) {

  uint32_t srv;
  srv = soc_reg_read(0);
  info("soc_reg: %x\n", srv);

  return srv;

}

static void soc_reg_write(uint32_t reg_off, uint32_t val) {
  u32 rb;
  spin_lock(&soc_lock);
  iowrite32(val, (void *)soc_regs + reg_off);
  rb = ioread32((void *)soc_regs + reg_off);
  spin_unlock(&soc_lock);
  dbg("soc_reg_write reg %d value %08x rb: %08x\n", reg_off, val, rb);
}
static uint32_t soc_reg_read(uint32_t reg_off) {
  u32 val;
  spin_lock(&soc_lock);
  val = ioread32((void *)soc_regs + reg_off);
  spin_unlock(&soc_lock);
  return val;
}
static void timer_reg_write(uint32_t reg_off, uint32_t val) {
  u32 rb;
  spin_lock(&timer_lock);
  iowrite32(val, (void *)timer_regs + reg_off);
  rb = ioread32((void *)timer_regs + reg_off);
  spin_unlock(&timer_lock);
  dbg("timer_reg_write reg %d value %08x rb: %08x\n", reg_off, val, rb);
}
static uint32_t timer_reg_read(uint32_t reg_off) {
  u32 val;
  spin_lock(&timer_lock);
  val = ioread32((void *)timer_regs + reg_off);
  spin_unlock(&timer_lock);
  return val;
}
static void cluster_periph_write(struct pulp_cluster *pc, uint32_t reg_off, uint32_t val) {
  iowrite32(val, (void *)(pc->pbase + reg_off));
}
static uint32_t cluster_periph_read (struct pulp_cluster *pc, uint32_t reg_off){
  return ioread32((void *)pc->pbase + reg_off);
}
static void quadrant_ctrl_reg_write(struct quadrant_ctrl *qc, uint32_t reg_off, uint32_t val) {
  iowrite32(val, (void *)qc->regs + reg_off);
}
static uint32_t quadrant_ctrl_reg_read(struct quadrant_ctrl *qc, uint32_t reg_off) {
  return ioread32((void *)qc->regs + reg_off);
}
static struct quadrant_ctrl *get_quadrant_ctrl(u32 quadrant_idx) {
  struct quadrant_ctrl *qc, *ret;
  ret = NULL;
  list_for_each_entry(qc, &quadrant_ctrl_list, list) if (qc->quadrant_idx == quadrant_idx) {
    ret = qc;
    break;
  }
  return ret;
}

static int write_tlb(struct pulp_cluster *pc, struct axi_tlb_entry *tlbe) {
  uint32_t reg_off;

  // TODO: Sanitize index in range correctly
  if (tlbe->idx > 64)
    return -EINVAL;

  if (tlbe->loc == AXI_TLB_NARROW) {
    reg_off = QCTL_TLB_NARROW_REG_OFFSET + tlbe->idx * TLB_ENTRY_BYTES;
  } else
    return -EINVAL;

  dbg("tlb write offset %#x first %#llx last %#llx base %#llx flags %#x\n", reg_off,
       tlbe->first >> 12, tlbe->last >> 12, tlbe->base >> 12, tlbe->flags);

  iowrite32( ( (tlbe->first >> 12) <<32 ) >> 32, pc->quadrant_ctrl->regs + reg_off + 0 );
  iowrite32(   (tlbe->first >> 12)        >> 32, pc->quadrant_ctrl->regs + reg_off + 4 );
  iowrite32( ( (tlbe->last  >> 12) <<32 ) >> 32, pc->quadrant_ctrl->regs + reg_off + 8 );
  iowrite32(   (tlbe->last  >> 12)        >> 32, pc->quadrant_ctrl->regs + reg_off + 12);
  iowrite32( ( (tlbe->base  >> 12) <<32 ) >> 32, pc->quadrant_ctrl->regs + reg_off + 16);
  iowrite32(   (tlbe->base  >> 12)        >> 32, pc->quadrant_ctrl->regs + reg_off + 20);
  iowrite32(    tlbe->flags                    , pc->quadrant_ctrl->regs + reg_off + 24);
  return 0;
}
static int read_tlb(struct pulp_cluster *pc, struct axi_tlb_entry *tlbe) {
  uint32_t reg_off;

  // TODO: Sanitize index in range correctly
  if (tlbe->idx > 64)
    return -EINVAL;

  if (tlbe->loc == AXI_TLB_NARROW) {
    reg_off = QCTL_TLB_NARROW_REG_OFFSET + tlbe->idx * TLB_ENTRY_BYTES;
  } else
    return -EINVAL;

  tlbe->first = (  (uint64_t) ( (uint64_t) ioread32(pc->quadrant_ctrl->regs + reg_off + 4 ) << 32 ) | ioread32(pc->quadrant_ctrl->regs + reg_off + 0 )  ) << 12;
  tlbe->last  = (  (uint64_t) ( (uint64_t) ioread32(pc->quadrant_ctrl->regs + reg_off + 12) << 32 ) | ioread32(pc->quadrant_ctrl->regs + reg_off + 8 )  ) << 12;
  tlbe->base  = (  (uint64_t) ( (uint64_t) ioread32(pc->quadrant_ctrl->regs + reg_off + 20) << 32 ) | ioread32(pc->quadrant_ctrl->regs + reg_off + 16)  ) << 12;
  tlbe->flags = ioread32(pc->quadrant_ctrl->regs + reg_off + 24);
  dbg("  TLB read first %#llx last %#llx\n", tlbe->first, tlbe->last);

  return 0;
}

// ----------------------------------------------------------------------------
//
//   Platform Driver
//
// ----------------------------------------------------------------------------

/**
 * @brief read property `propname` from node `np` and return value if exists, else `default
 *
 * @param np device nope pointer
 * @param propname property name
 * @param default value if not found
 * @return u32 property value or default
 */
static u32 of_get_prop_u32_default(const struct device_node *np, const char *propname, u32 dflt) {
  int ret;
  u32 value;
  ret = of_property_read_u32(np, propname, &value);
  return !ret ? value : dflt;
}

/**
 * pulp_probe - pulp cluster probe function.
 * @pdev:	Pointer to platform device structure.
 *
 * Return: 0, on success
 *	    Non-zero error value on failure.
 *
 */
static int pulp_probe(struct platform_device *pdev) {
  struct resource *res, memres;
  struct pulp_cluster *pc;
  struct quadrant_ctrl *qc;
  struct device_node *np;
  struct resource socres;
  struct resource timerres;
  struct resource quadctrlres;
  int ret;
  int err = 0;

  dev_info(&pdev->dev, "probe\n");

  // Allocate memory for the pulp cluster structure
  pc = devm_kmalloc(&pdev->dev, sizeof(*pc), GFP_KERNEL);
  pc->nodename = NULL;
  if (!pc)
    return -ENOMEM;

  // Populate cluster info struct
  pc->pci.compute_num = of_get_prop_u32_default(pdev->dev.of_node, "eth,compute-cores", 8);
  pc->pci.dm_num = of_get_prop_u32_default(pdev->dev.of_node, "eth,dm-cores", 1);
  pc->pci.cluster_idx = of_get_prop_u32_default(pdev->dev.of_node, "eth,cluster-idx", 0);
  pc->pci.quadrant_idx = of_get_prop_u32_default(pdev->dev.of_node, "eth,quadrant-idx", 0);

  dev_info(&pdev->dev, "computer-cores: %d dm-cores: %d cluster: %d quadrant: %d\n",
           pc->pci.compute_num, pc->pci.dm_num, pc->pci.cluster_idx, pc->pci.quadrant_idx);

  INIT_LIST_HEAD(&pc->list);
  // mutex_lock(&pulp_mtx);

  // Get resource and remap to kernel space
  // the pulp node should have two reg properties, one for TCDM the other for peripherals

  ret = platform_get_irq(pdev, 0);
  if (ret <= 0) {
    printk(KERN_DEBUG "PULP: Could not allocate IRQ resource mbox\n");
    pc->irq_mbox = -1;
    return -ENODEV;
  }
  pc->irq_mbox = ret;
  printk(KERN_DEBUG "PULP irq_mbox: %d\n", ret);

  ret = platform_get_irq(pdev, 1);
  if (ret <= 0) {
    printk(KERN_DEBUG "PULP: Could not allocate IRQ resource mbox\n");
    pc->irq_eoc = -1;
    return -ENODEV;
  }
  pc->irq_eoc = ret;
  printk(KERN_DEBUG "PULP irq_eoc: %d\n", ret);

  // TCDM is mapped as memory
  res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  pc->l1.pbase = res->start;
  pc->l1.size = resource_size(res);
  pc->l1.vbase = memremap(res->start, resource_size(res), MEMREMAP_WT);
  if (!pc->l1.vbase) {
    dev_err(&pdev->dev, "memremap of TCDM failed\n");
    err = -ENOMEM;
    goto out;
  }
  dev_info(&pdev->dev, "Remapped TCDM phys %px virt %px size %x\n", (void *)pc->l1.pbase,
           (void *)pc->l1.vbase, (unsigned int)pc->l1.size);

  // Cluster peripheral is mapped as resource
  res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
  pc->pbase = devm_ioremap_resource(&pdev->dev, res);
  if (IS_ERR(pc->pbase)) {
    err = PTR_ERR(pc->pbase);
    goto out;
  }
  pc->pci.periph_size = resource_size(res);
  dev_info(&pdev->dev, "peripherals virt %px\n", pc->pbase);

  // SPM is mapped as memory
  res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
  pc->l2.pbase = res->start;
  pc->l2.size = resource_size(res);
  pc->l2.vbase = memremap(res->start, resource_size(res), MEMREMAP_WT);
  if (!pc->l2.vbase) {
    dev_err(&pdev->dev, "memremap of L2SPM failed\n");
    err = -ENOMEM;
    goto out;
  }
  dev_info(&pdev->dev, "Remapped L2SPM phys %px virt %px size %x\n", (void *)pc->l2.pbase,
           (void *)pc->l2.vbase, (unsigned int)pc->l2.size);

  // SoC control
  if (!soc_regs) {
    spin_lock_init(&soc_lock);
    np = of_parse_phandle(pdev->dev.of_node, "eth,soc-ctl", 0);
    if (!np) {
      dev_err(&pdev->dev, "No %s specified\n", "eth,soc-ctl");
      err = -EINVAL;
      goto out;
    }
    ret = of_address_to_resource(np, 0, &socres);
    soc_regs = devm_ioremap_resource(&pdev->dev, &socres);
    if (IS_ERR(soc_regs)) {
      dev_err(&pdev->dev, "could not map soc-ctl regs\n");
      err = PTR_ERR(soc_regs);
      goto out;
    }
  }
  dev_info(&pdev->dev, "soc_regs virt %px\n", (void *)soc_regs);

  // APB timer
  if (!timer_regs) {
    spin_lock_init(&timer_lock);
    np = of_parse_phandle(pdev->dev.of_node, "eth,timer-ctl", 0);
    if (!np) {
      dev_err(&pdev->dev, "No %s specified\n", "eth,timer-ctl");
      err = -EINVAL;
      goto out;
    }
    ret = of_address_to_resource(np, 0, &timerres);
    timer_regs = devm_ioremap_resource(&pdev->dev, &timerres);
    if (IS_ERR(timer_regs)) {
      dev_err(&pdev->dev, "could not map timer-ctl regs\n");
      err = PTR_ERR(timer_regs);
      goto out;
    }
  }
  dev_info(&pdev->dev, "timer_regs virt %px\n", (void *)timer_regs);

  // Quadrant control
  qc = get_quadrant_ctrl(pc->pci.quadrant_idx);
  if (!qc) {
    dev_info(&pdev->dev, "quadrantr ctrl not found, mapping new\n");
    np = of_parse_phandle(pdev->dev.of_node, "eth,quadrant-ctrl", 0);
    if (!np) {
      dev_err(&pdev->dev, "No %s specified\n", "eth,quadrant-ctrl");
      err = -EINVAL;
      goto out;
    }
    qc = devm_kmalloc(&pdev->dev, sizeof(*qc), GFP_KERNEL);
    if (!qc) {
      err = -ENOMEM;
      goto out;
    }
    qc->quadrant_idx = pc->pci.quadrant_idx;
    list_add(&qc->list, &quadrant_ctrl_list);
    ret = of_address_to_resource(np, 0, &quadctrlres);
    qc->regs = devm_ioremap_resource(&pdev->dev, &quadctrlres);
    if (IS_ERR(qc->regs)) {
      dev_err(&pdev->dev, "could not map quadrant-ctrl regs\n");
      err = PTR_ERR(qc->regs);
      goto out;
    }
  }
  pc->quadrant_ctrl = qc;
  dev_info(&pdev->dev, "quadrant ctrl virt %px quadrant %d\n", (void *)qc->regs, qc->quadrant_idx);

  // Get reserved memory region from Device-tree
  np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
  if (!np) {
    dev_err(&pdev->dev, "No %s specified\n", "memory-region");
    err = -EINVAL;
    goto out;
  }

  // map it to kernel space
  ret = of_address_to_resource(np, 0, &memres);
  if (ret) {
    dev_err(&pdev->dev, "No memory address assigned to the region\n");
    err = -EINVAL;
    goto out;
  }

  pc->l3.pbase = memres.start;
  pc->l3.size = resource_size(&memres);
  pc->l3.vbase = memremap(memres.start, resource_size(&memres), MEMREMAP_WB);
  // pc->l3.vbase = devm_ioremap_resource(&pdev->dev, &memres);
  if (!pc->l3.vbase) {
    dev_err(&pdev->dev, "memremap failed\n");
    err = -ENOMEM;
    goto out;
  }
  dev_info(&pdev->dev, "Remapped shared L3 phys %px virt %px\n", (void *)pc->l3.pbase,
           (void *)pc->l3.vbase);

  
  if (pc->irq_mbox != -1) {
    ret=request_irq(pc->irq_mbox, pulp_isr, 0, "PULP", pc);
    if (ret) {
      printk(KERN_WARNING "PULP: Error requesting IRQ %d.\n", pc->irq_mbox);
      return ret;
    }
  }
  else {
    printk(KERN_WARNING "PULP: Error requesting IRQ %d.\n", pc->irq_mbox);
    return -1;
  }

  if (pc->irq_eoc != -1) {
    ret=request_irq(pc->irq_eoc, pulp_isr, 0, "PULP", pc);
    if (ret) {
      printk(KERN_WARNING "PULP: Error requesting IRQ %d.\n", pc->irq_eoc);
      return ret;
    }
  }
  else {
    printk(KERN_WARNING "PULP: Error requesting IRQ %d.\n", pc->irq_eoc);
    return -1;
  }

  pc->pci.periph_size = pc->l3.size;
  pc->pci.l1_size = pc->l1.size;
  pc->pci.l3_size = pc->l3.size;
  pc->pci.l2_size = pc->l2.size;
  pc->pci.l3_paddr = (void *)pc->l3.pbase;
  pc->pci.l1_paddr = (void *)pc->l1.pbase;
  pc->pci.l2_paddr = (void *)pc->l2.pbase;

  list_add(&pc->list, &pc_list);

  printk(KERN_DEBUG "PULP: probed!\n");
  
out:
  // mutex_unlock(&pulp_mtx);
  return err;
}

/**
 * @brief Cleanup
 * @pdev:	Pointer to platform device structure.
 *
 * Return: 0, on success
 *	    Non-zero error value on failure.
 */
static int pulp_remove(struct platform_device *pdev) { return 0; }

static struct platform_driver pulp_driver = {
    .probe = pulp_probe,
    .remove = pulp_remove,
    .driver =
        {
            .name = "eth_pulp_cluster",
            .of_match_table = pulp_of_match,
        },
};

static char *pulp_devnode(struct device *dev, umode_t *mode) {
  struct pulp_cluster *pc = dev_get_drvdata(dev);

  if (mode && pc->mode)
    *mode = pc->mode;
  if (pc->nodename)
    return kstrdup(pc->nodename, GFP_KERNEL);
  return NULL;
}

// ----------------------------------------------------------------------------
//
//   Init Module
//
// ----------------------------------------------------------------------------

int pulp_init(void) {
  int ret;
  char devname[12];
  struct pulp_cluster *pc;

  info("Loading pulp module\n");
  
  // Create /sys/class/pulp in preparation of creating /dev/pulp
  pulp_dev.class = class_create(THIS_MODULE, CLASS_NAME);
  if (IS_ERR(pulp_dev.class)) {
    info(KERN_WARNING "can't create class\n");
    return -1;
  }

  // register character device and optain major number so that accesses on the char device are
  // mapped to this module. Request major 0 to get a dynamically assigned major number
  pulp_dev.major = register_chrdev(0, DEVICE_NAME, &pulp_fops);
  if (pulp_dev.major < 0) {
    info(KERN_ALERT "Registering char device failed with %d\n", pulp_dev.major);
    return pulp_dev.major;
  }
  pulp_dev.class->devnode = pulp_devnode;

  // Dipcover clusters from devicetree and register
  ret = platform_driver_register(&pulp_driver);
  if (ret) {
    info(KERN_ALERT "Registering platform driver failed: %d\n", ret);
    return ret;
  }

  printk("list for each entry\n");
  list_for_each_entry(pc, &pc_list, list) {
    printk("enter\n");
    // Use same file operations for all clusters
    pc->fops = &pulp_fops;

    // Use cluster index as device minor number
    pc->minor = pc->pci.cluster_idx;

    // Create file in /dev/
    snprintf(devname, sizeof(devname), DEVICE_NAME "%d", pc->minor);
    pc->this_device = device_create_with_groups(pulp_dev.class, NULL, MKDEV(pulp_dev.major, pc->minor),
                                                pc, NULL, devname);
    if (IS_ERR(pc->this_device)) {
      info(KERN_WARNING "can't create device in /dev/\n");
    } else {
      info("Created char device /dev/%s\n", devname);
    }
  }

  printk("Done\n");
  return 0;
}

// ----------------------------------------------------------------------------
//
//   Cleanup module
//
// ----------------------------------------------------------------------------

void pulp_exit(void) {
  struct pulp_cluster *pc;

  // mutex_lock(&pulp_mtx);

  list_for_each_entry(pc, &pc_list, list) {
    device_destroy(pulp_dev.class, MKDEV(pulp_dev.major, pc->minor));
  }

  class_destroy(pulp_dev.class);
  unregister_chrdev(pulp_dev.major, DEVICE_NAME);

  platform_driver_unregister(&pulp_driver);

  // mutex_unlock(&pulp_mtx);

  free_irq(pc->irq_mbox,NULL);
  free_irq(pc->irq_eoc,NULL);
  
  info("unload complete\n");
}

module_init(pulp_init);
module_exit(pulp_exit);
