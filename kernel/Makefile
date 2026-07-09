obj-m += afl_kernel.o
afl_kernel-y := driver.o driver_transport.o driver_dma.o fake_dma_aperture.o \
	virtual_device.o virtual_device_regs.o virtual_device_trace.o \
	virtual_device_irq.o virtual_device_workloads.o virtual_device_fw.o \
	virtual_device_queues.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
PROJECT_ROOT := $(abspath $(PWD)/..)

ccflags-y += -I$(PROJECT_ROOT)/include

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
