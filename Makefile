#!/usr/bin/make

MACHINE=$(shell uname -m)
ifndef KERNEL_DIR
KERNEL_DIR:=/lib/modules/`uname -r`/build
endif

file_exist=$(shell test -f $(1) && echo yes || echo no)

# test for 2.6 or 2.4 kernel
ifeq ($(call file_exist,$(KERNEL_DIR)/Rules.make), yes)
PATCHLEVEL:=4
else
PATCHLEVEL:=6
endif

KERNOBJ:=cloop.o

# Name of module
ifeq ($(PATCHLEVEL),6)
MODULE:=cloop.ko
else
MODULE:=cloop.o
endif

module: $(MODULE)

utils: 
	$(make) -C utils

all: $(MODULE) utils

# For Kernel 2.6, we now use the "recommended" way to build kernel modules
obj-m := cloop.o
# cloop-objs := cloop.o

$(MODULE): cloop.c cloop.h
	@echo "Building for Kernel Patchlevel $(PATCHLEVEL)"
	$(MAKE) modules -C $(KERNEL_DIR) M=$(CURDIR)

clean:
	rm -rf *.o *.ko Module.symvers cloop.mod.c .cloop* modules.order
