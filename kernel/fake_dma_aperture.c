#include "fake_dma_aperture.h"

#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/string.h>

#define AFL_FAKE_DMA_MAX_BUFFERS 64u

struct afl_fake_dma_mapping {
    bool in_use;
    void *cpu_addr;
    afl_ioctl_u64_t dma_addr;
    size_t bytes;
};

static DEFINE_MUTEX(afl_fake_dma_lock);
static struct afl_fake_dma_mapping afl_fake_dma_mappings[AFL_FAKE_DMA_MAX_BUFFERS];

static bool afl_fake_dma_contains(const struct afl_fake_dma_mapping *mapping, afl_ioctl_u64_t dma_addr, size_t bytes)
{
    afl_ioctl_u64_t offset;

    if (!mapping->in_use || dma_addr < mapping->dma_addr || bytes == 0)
        return false;

    offset = dma_addr - mapping->dma_addr;
    return offset <= mapping->bytes && bytes <= mapping->bytes - offset;
}

static struct afl_fake_dma_mapping *afl_fake_dma_find_locked(afl_ioctl_u64_t dma_addr, size_t bytes)
{
    unsigned int i;

    for (i = 0; i < AFL_FAKE_DMA_MAX_BUFFERS; ++i) {
        if (afl_fake_dma_contains(&afl_fake_dma_mappings[i], dma_addr, bytes))
            return &afl_fake_dma_mappings[i];
    }

    return NULL;
}

int afl_fake_dma_register(void *cpu_addr, afl_ioctl_u64_t dma_addr, size_t bytes)
{
    unsigned int i;
    int free_index = -1;

    if (cpu_addr == NULL || dma_addr == 0 || bytes == 0)
        return -EINVAL;

    mutex_lock(&afl_fake_dma_lock);
    for (i = 0; i < AFL_FAKE_DMA_MAX_BUFFERS; ++i) {
        if (!afl_fake_dma_mappings[i].in_use) {
            if (free_index < 0)
                free_index = (int)i;
            continue;
        }
        if (afl_fake_dma_mappings[i].dma_addr == dma_addr) {
            mutex_unlock(&afl_fake_dma_lock);
            return -EEXIST;
        }
    }

    if (free_index < 0) {
        mutex_unlock(&afl_fake_dma_lock);
        return -ENOMEM;
    }

    afl_fake_dma_mappings[free_index].in_use = true;
    afl_fake_dma_mappings[free_index].cpu_addr = cpu_addr;
    afl_fake_dma_mappings[free_index].dma_addr = dma_addr;
    afl_fake_dma_mappings[free_index].bytes = bytes;
    mutex_unlock(&afl_fake_dma_lock);
    return 0;
}

void afl_fake_dma_unregister(afl_ioctl_u64_t dma_addr)
{
    unsigned int i;

    if (dma_addr == 0)
        return;

    mutex_lock(&afl_fake_dma_lock);
    for (i = 0; i < AFL_FAKE_DMA_MAX_BUFFERS; ++i) {
        if (afl_fake_dma_mappings[i].in_use && afl_fake_dma_mappings[i].dma_addr == dma_addr) {
            memset(&afl_fake_dma_mappings[i], 0, sizeof(afl_fake_dma_mappings[i]));
            break;
        }
    }
    mutex_unlock(&afl_fake_dma_lock);
}

int afl_fake_dma_read(afl_ioctl_u64_t dma_addr, void *dst, size_t bytes)
{
    struct afl_fake_dma_mapping *mapping;
    afl_ioctl_u64_t offset;

    if (dst == NULL || dma_addr == 0 || bytes == 0)
        return -EINVAL;

    mutex_lock(&afl_fake_dma_lock);
    mapping = afl_fake_dma_find_locked(dma_addr, bytes);
    if (mapping == NULL) {
        mutex_unlock(&afl_fake_dma_lock);
        return -EFAULT;
    }

    offset = dma_addr - mapping->dma_addr;
    memcpy(dst, (const u8 *)mapping->cpu_addr + offset, bytes);
    mutex_unlock(&afl_fake_dma_lock);
    return 0;
}

int afl_fake_dma_write(afl_ioctl_u64_t dma_addr, const void *src, size_t bytes)
{
    struct afl_fake_dma_mapping *mapping;
    afl_ioctl_u64_t offset;

    if (src == NULL || dma_addr == 0 || bytes == 0)
        return -EINVAL;

    mutex_lock(&afl_fake_dma_lock);
    mapping = afl_fake_dma_find_locked(dma_addr, bytes);
    if (mapping == NULL) {
        mutex_unlock(&afl_fake_dma_lock);
        return -EFAULT;
    }

    offset = dma_addr - mapping->dma_addr;
    memcpy((u8 *)mapping->cpu_addr + offset, src, bytes);
    mutex_unlock(&afl_fake_dma_lock);
    return 0;
}
