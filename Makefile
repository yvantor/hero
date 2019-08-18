ROOT := $(patsubst %/,%, $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
export PATH := $(RISCV)/bin:$(RISCV)/usr/bin:$(PATH)

.PHONY: all br-ariane br-hero br-qemu toolchain-ariane-linux tools tools-isa-sim tools-openocd

all: br-ariane br-hero

# buildroot
br-ariane:
	mkdir -p $(CURDIR)/output/br-ariane
	$(MAKE) O=$(CURDIR)/output/br-ariane BR2_EXTERNAL=$(ROOT) -C $(ROOT)/buildroot ariane_defconfig
	if [ -a $(CURDIR)/local.cfg ]; then cat $(CURDIR)/local.cfg >> $(CURDIR)/output/br-ariane/.config; fi
	$(MAKE) -C $(CURDIR)/output/br-ariane
	cp $(CURDIR)/output/br-ariane/images/bbl.bin $(CURDIR)

br-hero:
	mkdir -p $(CURDIR)/output/br-hero
	$(MAKE) O=$(CURDIR)/output/br-hero BR2_EXTERNAL=$(ROOT) -C $(ROOT)/buildroot hero_defconfig
	if [ -a $(CURDIR)/local.cfg ]; then cat $(CURDIR)/local.cfg >> $(CURDIR)/output/br-hero/.config; fi
	$(MAKE) -C $(CURDIR)/output/br-hero

br-qemu:
	mkdir -p $(CURDIR)/output/br-qemu
	$(MAKE) O=$(CURDIR)/output/br-qemu BR2_EXTERNAL=$(ROOT) -C $(ROOT)/buildroot qemu_defconfig
	if [ -a $(CURDIR)/local.cfg ]; then cat $(CURDIR)/local.cfg >> $(CURDIR)/output/br-qemu/.config; fi
	$(MAKE) -C $(CURDIR)/output/br-qemu

# support
pulp-sdk:
	(export PULP_RISCV_GCC_TOOLCHAIN=$(RISCV); \
	 	cd support/pulp-sdk; \
		scripts/hero/setup.sh; \
	)

# toolchain
tc-ariane-bare:
	mkdir -p $(CURDIR)/output/tc-ariane-bare/
	cd $(CURDIR)/output/tc-ariane-bare/ && $(ROOT)/toolchain/build.sh $(ROOT)/toolchain/ariane-bare.config

tc-ariane-linux:
	mkdir -p $(CURDIR)/output/tc-ariane-linux/
	cd $(CURDIR)/output/tc-ariane-linux/ && $(ROOT)/toolchain/build.sh $(ROOT)/toolchain/ariane-linux.config

tc-pulp:
	mkdir -p $(CURDIR)/output/tc-pulp/
	cd $(CURDIR)/output/tc-pulp/ && $(ROOT)/toolchain/build.sh $(ROOT)/toolchain/pulp.config

# hardware
hw-ariane:
	$(MAKE) -C $(ROOT)/hardware/ariane fpga
	mv $(ROOT)/hardware/ariane/fpga/work-fpga/ariane_xilinx.mcs $(ROOT)

# tools
tools: tools-isa-sim tools-openocd

tools-isa-sim:
	mkdir -p $(CURDIR)/output/tools-isa-sim/
	(cd $(CURDIR)/output/tools-isa-sim/; \
		export PATH=$(RISCV)/bin:${PATH}; \
		echo ${PATH}; \
		$(ROOT)/tools/riscv-isa-sim/configure --prefix=$(RISCV); \
		$(MAKE); \
	  chmod -R u+w $(RISCV); \
		$(MAKE) install; \
	  chmod -R u-w $(RISCV); \
	)

tools-openocd:
	mkdir -p $(CURDIR)/output/tools-openocd/
	(export CCACHE=none; \
		export PATH=$(RISCV)/bin:${PATH}; \
		cd $(ROOT)/tools/riscv-openocd/; \
		./bootstrap; \
		cd $(CURDIR)/output/tools-openocd/; \
		$(ROOT)/tools/riscv-openocd/configure --prefix=$(RISCV); \
		$(MAKE); \
	  chmod -R u+w $(RISCV); \
		$(MAKE) install; \
	  chmod -R u-w $(RISCV); \
	)
