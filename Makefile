# SPDX-License-Identifier: GPL-2.0 OR MIT
# Copyright (C) 2015-2023 Ichiro Kawazome

#
# For in kernel tree variables
# 
obj-$(CONFIG_FCLKCFG) := fclkcfg.o

#
# For out of kernel tree variables
#
CONFIG_MODULES   ?= CONFIG_FCLKCFG=m

HOST_ARCH        ?= $(shell uname -m | sed -e s/arm.*/arm/ -e s/aarch64.*/arm64/)
ARCH             ?= $(shell uname -m | sed -e s/arm.*/arm/ -e s/aarch64.*/arm64/)

ifdef KERNEL_SRC
  KERNEL_SRC_DIR := $(KERNEL_SRC)
else
  KERNEL_SRC_DIR ?= /lib/modules/$(shell uname -r)/build
endif

ifeq ($(ARCH), arm)
 ifneq ($(HOST_ARCH), arm)
   CROSS_COMPILE ?= arm-linux-gnueabihf-
 endif
endif
ifeq ($(ARCH), arm64)
 ifneq ($(HOST_ARCH), arm64)
   CROSS_COMPILE ?= aarch64-linux-gnu-
 endif
endif

all:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) $(CONFIG_MODULES) modules

modules_install:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) $(CONFIG_MODULES) modules_install

clean:
	$(MAKE) -C $(KERNEL_SRC_DIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(PWD) $(CONFIG_MODULES) clean

