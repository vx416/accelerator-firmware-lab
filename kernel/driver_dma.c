#include "driver_dma.h"

#include "fake_dma_aperture.h"

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

int afl_driver_dma_alloc(struct afl_driver_dma_buffer *buffer, size_t bytes)
{
    int ret;

    if (buffer == NULL || bytes == 0 || bytes > AFL_IOCTL_MAX_TRANSFER_BYTES)
        return -EINVAL;

    memset(buffer, 0, sizeof(*buffer));
    buffer->cpu_addr = kmalloc(bytes, GFP_KERNEL);
    if (buffer->cpu_addr == NULL)
        return -ENOMEM;

    buffer->dma_addr = (afl_ioctl_u64_t)(unsigned long)buffer->cpu_addr;
    buffer->bytes = bytes;
    ret = afl_fake_dma_register(buffer->cpu_addr, buffer->dma_addr, buffer->bytes);
    if (ret != 0) {
        kfree(buffer->cpu_addr);
        memset(buffer, 0, sizeof(*buffer));
        return ret;
    }
    return 0;
}

void afl_driver_dma_free(struct afl_driver_dma_buffer *buffer)
{
    if (buffer == NULL)
        return;

    afl_fake_dma_unregister(buffer->dma_addr);
    kfree(buffer->cpu_addr);
    memset(buffer, 0, sizeof(*buffer));
}

int afl_driver_dma_stage_from_user(struct afl_driver_dma_buffer *buffer, afl_ioctl_u64_t user_ptr, size_t bytes)
{
    int ret;

    ret = afl_driver_dma_alloc(buffer, bytes);
    if (ret != 0)
        return ret;
    if (user_ptr == 0) {
        afl_driver_dma_free(buffer);
        return -EINVAL;
    }
    if (copy_from_user(buffer->cpu_addr, (const void __user *)(unsigned long)user_ptr, bytes) != 0) {
        afl_driver_dma_free(buffer);
        return -EFAULT;
    }

    return 0;
}

int afl_driver_dma_copy_to_user(const struct afl_driver_dma_buffer *buffer, afl_ioctl_u64_t user_ptr, size_t bytes)
{
    if (buffer == NULL || buffer->cpu_addr == NULL || user_ptr == 0 || bytes > buffer->bytes)
        return -EINVAL;
    if (copy_to_user((void __user *)(unsigned long)user_ptr, buffer->cpu_addr, bytes) != 0)
        return -EFAULT;
    return 0;
}
