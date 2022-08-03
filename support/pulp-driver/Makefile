# Copyright 2020 ETH Zurich and University of Bologna.
# Solderpad Hardware License, Version 0.51, see LICENSE for details.
# SPDX-License-Identifier: SHL-0.51

# Buildroot-compatible kernel module makefile

obj-m := pulp.o

ARCH          := ${KERNEL_ARCH}
CROSS_COMPILE := ${KERNEL_CROSS_COMPILE}

ccflags-y += -DPLATFORM=${PLATFORM} ${CFLAGS} -O2 -I$(src)/../include
pulp-objs := pulp_module.o

EXTRA_CFLAGS = -I$(PWD)/../include

.PHONY: all deploy dis clean build

all: modules
build: modules

modules: $(patsubst %.o,%.c,$(pulp-objs)) $(patsubst %.o,%.h,$(pulp-objs))
	$(MAKE) -S ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_DIR)  M=$(shell pwd) modules

dis: modules
	$(CROSS_COMPILE)objdump -DS pulp.ko > pulp.disasm

clean:
	rm -f modules.order
	rm -f pulp.mod.*
	rm -f *.o
	rm -f pulp.ko
	rm -f Module.symvers
	rm -f .*.o.cmd
	rm -f .*.ko.cmd
	rm -f *.disasm
	rm -f *.o_shipped
