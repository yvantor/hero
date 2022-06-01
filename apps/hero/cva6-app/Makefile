# Copyright 2020 ETH Zurich and University of Bologna.
# Solderpad Hardware License, Version 0.51, see LICENSE for details.
# SPDX-License-Identifier: SHL-0.51

# Pull common makefile
MKFILE_PATH := $(abspath $(lastword $(MAKEFILE_LIST)))
MKFILE_DIR  := $(dir $(MKFILE_PATH))
ROOT        := ${MKFILE_DIR}../../..
ROOTFS      := $(ROOT)/rootfs
RV64_CC     := $(HERO_INSTALL)/bin/riscv64-hero-linux-gnu-gcc

OBJ_C := common.o
APPS  := bringup

# C flags
CFLAGS    += -Wall -O0 -g -rdynamic -fPIC
CFLAGS    += -I../include/
DEPDIR 		:= .deps
DEPFLAGS 	= -MMD -MP
CFLAGS 		+= $(INC_FLAGS) $(DEPFLAGS)
LDFLAGS   += -lpulp

RFS_TARGET_DIR = $(ROOTFS)/root

all: ${APPS}

debug:
	@echo $(CFLAGS)

# compile
%.o : %.c
	@echo CC $@
	@echo $(CFLAGS)
	$(RV64_CC) $(CFLAGS) -c $< -o $@

# link
bringup: bringup.o ${OBJ_C} ${LIBPULP}
	@echo LD $@
	$(RV64_CC) $(LDFLAGS) -o $@ $< ${OBJ_C}
	cp ${APPS} run.sh ../../../rootfs

clean:
	rm -f ${OBJ_C} ${APPS} *.d *.o

install-rootfs:
	mkdir -p ${RFS_TARGET_DIR}
	cp ${APPS} ${RFS_TARGET_DIR}
	cp run.sh ${RFS_TARGET_DIR}
	cp ${LIBPULP} ${ROOTFS}/usr/lib

# include dependencies
-include $(DEPS)
