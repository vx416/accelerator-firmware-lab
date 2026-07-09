#include "buffer_pool.h"

#include "fake_dma_aperture.h"

#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#define AFL_BUFFER_POOL_MAX_BUFFERS 128u
#define AFL_BUFFER_POOL_MAX_FREE_RANGES 128u

struct afl_buffer_range {
    bool in_use;
    afl_ioctl_u32_t offset;
    afl_ioctl_u32_t size;
};

struct afl_buffer_object {
    bool in_use;
    afl_ioctl_u32_t handle;
    afl_ioctl_u32_t offset;
    afl_ioctl_u32_t size;
    void *owner;
};

static DEFINE_MUTEX(afl_buffer_pool_lock);
static void *afl_buffer_pool_base;
static afl_ioctl_u64_t afl_buffer_pool_dma_base;
static afl_ioctl_u32_t afl_buffer_pool_next_handle = 1;
static struct afl_buffer_range afl_buffer_pool_free_ranges[AFL_BUFFER_POOL_MAX_FREE_RANGES];
static struct afl_buffer_object afl_buffer_pool_buffers[AFL_BUFFER_POOL_MAX_BUFFERS];

static afl_ioctl_u32_t afl_buffer_align_size(size_t size)
{
    return (afl_ioctl_u32_t)PAGE_ALIGN(size);
}

static int afl_buffer_find_free_slot(void)
{
    unsigned int i;

    for (i = 0; i < AFL_BUFFER_POOL_MAX_FREE_RANGES; ++i) {
        if (!afl_buffer_pool_free_ranges[i].in_use)
            return (int)i;
    }

    return -1;
}

static int afl_buffer_find_object_slot(void)
{
    unsigned int i;

    for (i = 0; i < AFL_BUFFER_POOL_MAX_BUFFERS; ++i) {
        if (!afl_buffer_pool_buffers[i].in_use)
            return (int)i;
    }

    return -1;
}

static struct afl_buffer_object *afl_buffer_find_object_locked(void *owner, afl_ioctl_u32_t handle)
{
    unsigned int i;

    for (i = 0; i < AFL_BUFFER_POOL_MAX_BUFFERS; ++i) {
        if (afl_buffer_pool_buffers[i].in_use &&
            afl_buffer_pool_buffers[i].owner == owner &&
            afl_buffer_pool_buffers[i].handle == handle)
            return &afl_buffer_pool_buffers[i];
    }

    return NULL;
}

static struct afl_buffer_range *afl_buffer_find_mapping_locked(void *owner, afl_ioctl_u32_t pool_offset, unsigned long size)
{
    unsigned int i;

    for (i = 0; i < AFL_BUFFER_POOL_MAX_BUFFERS; ++i) {
        struct afl_buffer_object *buffer = &afl_buffer_pool_buffers[i];
        afl_ioctl_u32_t offset_in_buffer;

        if (!buffer->in_use || buffer->owner != owner || pool_offset < buffer->offset)
            continue;

        offset_in_buffer = pool_offset - buffer->offset;
        if (offset_in_buffer <= buffer->size && size <= buffer->size - offset_in_buffer)
            return (struct afl_buffer_range *)buffer;
    }

    return NULL;
}

static void afl_buffer_sort_free_ranges_locked(void)
{
    unsigned int i;
    unsigned int j;

    for (i = 0; i < AFL_BUFFER_POOL_MAX_FREE_RANGES; ++i) {
        for (j = i + 1u; j < AFL_BUFFER_POOL_MAX_FREE_RANGES; ++j) {
            struct afl_buffer_range tmp;

            if (!afl_buffer_pool_free_ranges[j].in_use)
                continue;
            if (afl_buffer_pool_free_ranges[i].in_use &&
                afl_buffer_pool_free_ranges[i].offset <= afl_buffer_pool_free_ranges[j].offset)
                continue;

            tmp = afl_buffer_pool_free_ranges[i];
            afl_buffer_pool_free_ranges[i] = afl_buffer_pool_free_ranges[j];
            afl_buffer_pool_free_ranges[j] = tmp;
        }
    }
}

static void afl_buffer_coalesce_free_ranges_locked(void)
{
    unsigned int i;

    afl_buffer_sort_free_ranges_locked();
    for (i = 0; i + 1u < AFL_BUFFER_POOL_MAX_FREE_RANGES; ++i) {
        struct afl_buffer_range *range = &afl_buffer_pool_free_ranges[i];
        struct afl_buffer_range *next = &afl_buffer_pool_free_ranges[i + 1u];

        if (!range->in_use || !next->in_use)
            continue;
        if (range->offset + range->size != next->offset)
            continue;

        range->size += next->size;
        memset(next, 0, sizeof(*next));
        afl_buffer_sort_free_ranges_locked();
        i = 0;
    }
}

static int afl_buffer_add_free_range_locked(afl_ioctl_u32_t offset, afl_ioctl_u32_t size)
{
    int slot;

    slot = afl_buffer_find_free_slot();
    if (slot < 0)
        return -ENOMEM;

    afl_buffer_pool_free_ranges[slot].in_use = true;
    afl_buffer_pool_free_ranges[slot].offset = offset;
    afl_buffer_pool_free_ranges[slot].size = size;
    afl_buffer_coalesce_free_ranges_locked();
    return 0;
}

int afl_buffer_pool_init(void)
{
    int ret;

    afl_buffer_pool_base = vmalloc_user(AFL_IOCTL_BUFFER_POOL_BYTES);
    if (afl_buffer_pool_base == NULL)
        return -ENOMEM;

    afl_buffer_pool_dma_base = (afl_ioctl_u64_t)(unsigned long)afl_buffer_pool_base;
    ret = afl_fake_dma_register(afl_buffer_pool_base, afl_buffer_pool_dma_base, AFL_IOCTL_BUFFER_POOL_BYTES);
    if (ret != 0) {
        vfree(afl_buffer_pool_base);
        afl_buffer_pool_base = NULL;
        afl_buffer_pool_dma_base = 0;
        return ret;
    }

    mutex_lock(&afl_buffer_pool_lock);
    memset(afl_buffer_pool_free_ranges, 0, sizeof(afl_buffer_pool_free_ranges));
    memset(afl_buffer_pool_buffers, 0, sizeof(afl_buffer_pool_buffers));
    afl_buffer_pool_next_handle = 1;
    afl_buffer_pool_free_ranges[0].in_use = true;
    afl_buffer_pool_free_ranges[0].offset = 0;
    afl_buffer_pool_free_ranges[0].size = AFL_IOCTL_BUFFER_POOL_BYTES;
    mutex_unlock(&afl_buffer_pool_lock);
    return 0;
}

void afl_buffer_pool_destroy(void)
{
    afl_fake_dma_unregister(afl_buffer_pool_dma_base);
    vfree(afl_buffer_pool_base);
    afl_buffer_pool_base = NULL;
    afl_buffer_pool_dma_base = 0;
}

int afl_buffer_pool_alloc(void *owner, struct afl_buffer_pool_owner *owner_state, size_t requested_size, afl_ioctl_u32_t *handle_out,
                          afl_ioctl_u32_t *mmap_offset_out, afl_ioctl_u32_t *allocated_size_out)
{
    afl_ioctl_u32_t size;
    unsigned int i;

    if (owner == NULL || owner_state == NULL || handle_out == NULL || mmap_offset_out == NULL || allocated_size_out == NULL)
        return -EINVAL;
    if (requested_size == 0 || requested_size > AFL_IOCTL_BUFFER_MAX_BYTES)
        return -EINVAL;

    size = afl_buffer_align_size(requested_size);
    if (size == 0 || size > AFL_IOCTL_BUFFER_MAX_BYTES)
        return -EINVAL;

    mutex_lock(&afl_buffer_pool_lock);
    if (owner_state->used_bytes > AFL_IOCTL_BUFFER_PER_FD_BYTES ||
        size > AFL_IOCTL_BUFFER_PER_FD_BYTES - owner_state->used_bytes) {
        mutex_unlock(&afl_buffer_pool_lock);
        return -EDQUOT;
    }

    for (i = 0; i < AFL_BUFFER_POOL_MAX_FREE_RANGES; ++i) {
        struct afl_buffer_range *range = &afl_buffer_pool_free_ranges[i];
        struct afl_buffer_object *buffer;
        int buffer_slot;
        afl_ioctl_u32_t offset;

        if (!range->in_use || range->size < size)
            continue;

        buffer_slot = afl_buffer_find_object_slot();
        if (buffer_slot < 0) {
            mutex_unlock(&afl_buffer_pool_lock);
            return -ENOMEM;
        }

        offset = range->offset;
        range->offset += size;
        range->size -= size;
        if (range->size == 0)
            memset(range, 0, sizeof(*range));

        buffer = &afl_buffer_pool_buffers[buffer_slot];
        buffer->in_use = true;
        buffer->handle = afl_buffer_pool_next_handle++;
        if (afl_buffer_pool_next_handle == 0)
            afl_buffer_pool_next_handle = 1;
        buffer->offset = offset;
        buffer->size = size;
        buffer->owner = owner;
        owner_state->used_bytes += size;

        *handle_out = buffer->handle;
        *mmap_offset_out = AFL_IOCTL_BUFFER_MMAP_BASE + offset;
        *allocated_size_out = size;
        mutex_unlock(&afl_buffer_pool_lock);
        return 0;
    }

    mutex_unlock(&afl_buffer_pool_lock);
    return -ENOMEM;
}

int afl_buffer_pool_free(void *owner, struct afl_buffer_pool_owner *owner_state, afl_ioctl_u32_t handle)
{
    struct afl_buffer_object *buffer;
    afl_ioctl_u32_t offset;
    afl_ioctl_u32_t size;
    int ret;

    if (owner == NULL || owner_state == NULL || handle == 0)
        return -EINVAL;

    mutex_lock(&afl_buffer_pool_lock);
    buffer = afl_buffer_find_object_locked(owner, handle);
    if (buffer == NULL) {
        mutex_unlock(&afl_buffer_pool_lock);
        return -ENOENT;
    }

    offset = buffer->offset;
    size = buffer->size;
    memset(buffer, 0, sizeof(*buffer));
    owner_state->used_bytes = owner_state->used_bytes >= size ? owner_state->used_bytes - size : 0;
    ret = afl_buffer_add_free_range_locked(offset, size);
    mutex_unlock(&afl_buffer_pool_lock);
    return ret;
}

void afl_buffer_pool_free_owner(void *owner, struct afl_buffer_pool_owner *owner_state)
{
    unsigned int i;

    if (owner == NULL || owner_state == NULL)
        return;

    mutex_lock(&afl_buffer_pool_lock);
    for (i = 0; i < AFL_BUFFER_POOL_MAX_BUFFERS; ++i) {
        struct afl_buffer_object *buffer = &afl_buffer_pool_buffers[i];

        if (!buffer->in_use || buffer->owner != owner)
            continue;
        afl_buffer_add_free_range_locked(buffer->offset, buffer->size);
        memset(buffer, 0, sizeof(*buffer));
    }
    owner_state->used_bytes = 0;
    mutex_unlock(&afl_buffer_pool_lock);
}

int afl_buffer_pool_lookup_dma(void *owner, afl_ioctl_u32_t handle, afl_ioctl_u32_t offset, size_t bytes, afl_ioctl_u64_t *dma_addr_out)
{
    struct afl_buffer_object *buffer;

    if (owner == NULL || handle == 0 || bytes == 0 || dma_addr_out == NULL)
        return -EINVAL;

    mutex_lock(&afl_buffer_pool_lock);
    buffer = afl_buffer_find_object_locked(owner, handle);
    if (buffer == NULL) {
        mutex_unlock(&afl_buffer_pool_lock);
        return -ENOENT;
    }
    if (offset > buffer->size || bytes > buffer->size - offset) {
        mutex_unlock(&afl_buffer_pool_lock);
        return -EINVAL;
    }

    *dma_addr_out = afl_buffer_pool_dma_base + buffer->offset + offset;
    mutex_unlock(&afl_buffer_pool_lock);
    return 0;
}

int afl_buffer_pool_mmap(void *owner, struct vm_area_struct *vma, unsigned long byte_offset, unsigned long size)
{
    afl_ioctl_u32_t pool_offset;

    if (owner == NULL || afl_buffer_pool_base == NULL)
        return -ENODEV;
    if (byte_offset < AFL_IOCTL_BUFFER_MMAP_BASE)
        return -EINVAL;
    if (byte_offset - AFL_IOCTL_BUFFER_MMAP_BASE > AFL_IOCTL_BUFFER_POOL_BYTES)
        return -EINVAL;
    pool_offset = (afl_ioctl_u32_t)(byte_offset - AFL_IOCTL_BUFFER_MMAP_BASE);

    mutex_lock(&afl_buffer_pool_lock);
    if (afl_buffer_find_mapping_locked(owner, pool_offset, size) == NULL) {
        mutex_unlock(&afl_buffer_pool_lock);
        return -EINVAL;
    }
    mutex_unlock(&afl_buffer_pool_lock);

    return remap_vmalloc_range(vma, afl_buffer_pool_base, pool_offset >> PAGE_SHIFT);
}
