#ifndef AFL_KERNEL_FAKE_DMA_APERTURE_H
#define AFL_KERNEL_FAKE_DMA_APERTURE_H

#include <linux/types.h>

#include "afl/ioctl.h"

int afl_fake_dma_register(void *cpu_addr, afl_ioctl_u64_t dma_addr, size_t bytes);
void afl_fake_dma_unregister(afl_ioctl_u64_t dma_addr);
int afl_fake_dma_read(afl_ioctl_u64_t dma_addr, void *dst, size_t bytes);
int afl_fake_dma_write(afl_ioctl_u64_t dma_addr, const void *src, size_t bytes);

#endif
