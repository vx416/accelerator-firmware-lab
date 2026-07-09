#ifndef AFL_KERNEL_DRIVER_DMA_H
#define AFL_KERNEL_DRIVER_DMA_H

#include <linux/types.h>

#include "afl/ioctl.h"

struct afl_driver_dma_buffer {
    void *cpu_addr;
    afl_ioctl_u64_t dma_addr;
    size_t bytes;
};

int afl_driver_dma_alloc(struct afl_driver_dma_buffer *buffer, size_t bytes);
void afl_driver_dma_free(struct afl_driver_dma_buffer *buffer);
int afl_driver_dma_stage_from_user(struct afl_driver_dma_buffer *buffer, afl_ioctl_u64_t user_ptr, size_t bytes);
int afl_driver_dma_copy_to_user(const struct afl_driver_dma_buffer *buffer, afl_ioctl_u64_t user_ptr, size_t bytes);

#endif
