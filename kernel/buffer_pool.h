#ifndef AFL_KERNEL_BUFFER_POOL_H
#define AFL_KERNEL_BUFFER_POOL_H

#include <linux/mm.h>
#include <linux/types.h>

#include "afl/ioctl.h"

struct afl_buffer_pool_owner {
    size_t used_bytes;
};

int afl_buffer_pool_init(void);
void afl_buffer_pool_destroy(void);
int afl_buffer_pool_alloc(void *owner, struct afl_buffer_pool_owner *owner_state, size_t requested_size, afl_ioctl_u32_t *handle_out,
                          afl_ioctl_u32_t *mmap_offset_out, afl_ioctl_u32_t *allocated_size_out);
int afl_buffer_pool_free(void *owner, struct afl_buffer_pool_owner *owner_state, afl_ioctl_u32_t handle);
void afl_buffer_pool_free_owner(void *owner, struct afl_buffer_pool_owner *owner_state);
int afl_buffer_pool_lookup_dma(void *owner, afl_ioctl_u32_t handle, afl_ioctl_u32_t offset, size_t bytes, afl_ioctl_u64_t *dma_addr_out);
int afl_buffer_pool_mmap(void *owner, struct vm_area_struct *vma, unsigned long byte_offset, unsigned long size);

#endif
