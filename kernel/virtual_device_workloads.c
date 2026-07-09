#include "virtual_device_internal.h"

#include "fake_dma_aperture.h"

#include <linux/errno.h>
#include <linux/string.h>

static int kvdev_dma_read(afl_ioctl_u64_t dma_addr, void *dst, size_t bytes)
{
    if (dma_addr == 0 || dst == NULL)
        return -EINVAL;
    return afl_fake_dma_read(dma_addr, dst, bytes);
}

static int kvdev_dma_write(afl_ioctl_u64_t dma_addr, const void *src, size_t bytes)
{
    if (dma_addr == 0 || src == NULL)
        return -EINVAL;
    return afl_fake_dma_write(dma_addr, src, bytes);
}

static int kvdev_check_bytes(size_t bytes)
{
    if (bytes == 0 || bytes > AFL_IOCTL_MAX_TRANSFER_BYTES)
        return -EINVAL;
    return 0;
}

static void *kvdev_scratch(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t slot, size_t bytes)
{
    if (slot >= AFL_KERNEL_SCRATCH_SLOT_COUNT || bytes > AFL_KERNEL_SCRATCH_SLOT_BYTES)
        return NULL;
    return vdev->scratch[slot];
}

int kvdev_do_memcopy(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    void *tmp;
    int ret;
    const typeof(command->payload.memcopy) *req = &command->payload.memcopy;

    ret = kvdev_check_bytes(req->length);
    if (ret != 0)
        return ret;

    tmp = kvdev_scratch(vdev, 0, req->length);
    if (tmp == NULL)
        return -ENOMEM;

    ret = kvdev_dma_read(req->src_dma_addr, tmp, req->length);
    if (ret == 0)
        ret = kvdev_dma_write(req->dst_dma_addr, tmp, req->length);

    return ret;
}

int kvdev_do_vector_add(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    size_t bytes;
    s32 *a;
    s32 *b;
    s32 *out;
    int ret;
    afl_ioctl_u32_t i;
    const typeof(command->payload.vector_add) *req = &command->payload.vector_add;

    if (req->dtype != AFL_IOCTL_DTYPE_I32 || req->element_count == 0)
        return -EINVAL;
    if (req->element_count > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(s32))
        return -EINVAL;

    bytes = (size_t)req->element_count * sizeof(s32);
    a = kvdev_scratch(vdev, 0, bytes);
    b = kvdev_scratch(vdev, 1, bytes);
    out = kvdev_scratch(vdev, 2, bytes);
    if (a == NULL || b == NULL || out == NULL)
        return -ENOMEM;

    ret = kvdev_dma_read(req->a_dma_addr, a, bytes);
    if (ret != 0)
        return ret;
    ret = kvdev_dma_read(req->b_dma_addr, b, bytes);
    if (ret != 0)
        return ret;

    for (i = 0; i < req->element_count; ++i)
        out[i] = a[i] + b[i];

    return kvdev_dma_write(req->out_dma_addr, out, bytes);
}

static int kvdev_matrix_bytes(afl_ioctl_u32_t rows, afl_ioctl_u32_t cols, size_t *bytes_out)
{
    size_t elements;

    if (rows == 0 || cols == 0)
        return -EINVAL;
    if (rows > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(s32) / cols)
        return -EINVAL;

    elements = (size_t)rows * cols;
    *bytes_out = elements * sizeof(s32);
    return kvdev_check_bytes(*bytes_out);
}

int kvdev_do_matrix_mul(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    size_t a_bytes;
    size_t b_bytes;
    size_t c_bytes;
    s32 *a;
    s32 *b;
    s32 *c;
    int ret;
    afl_ioctl_u32_t row;
    afl_ioctl_u32_t col;
    afl_ioctl_u32_t inner;
    const typeof(command->payload.matrix_mul) *req = &command->payload.matrix_mul;

    if (req->dtype != AFL_IOCTL_DTYPE_I32)
        return -EINVAL;
    ret = kvdev_matrix_bytes(req->m, req->k, &a_bytes);
    if (ret != 0)
        return ret;
    ret = kvdev_matrix_bytes(req->k, req->n, &b_bytes);
    if (ret != 0)
        return ret;
    ret = kvdev_matrix_bytes(req->m, req->n, &c_bytes);
    if (ret != 0)
        return ret;

    a = kvdev_scratch(vdev, 0, a_bytes);
    b = kvdev_scratch(vdev, 1, b_bytes);
    c = kvdev_scratch(vdev, 2, c_bytes);
    if (a == NULL || b == NULL || c == NULL)
        return -ENOMEM;

    ret = kvdev_dma_read(req->a_dma_addr, a, a_bytes);
    if (ret != 0)
        return ret;
    ret = kvdev_dma_read(req->b_dma_addr, b, b_bytes);
    if (ret != 0)
        return ret;

    for (row = 0; row < req->m; ++row) {
        for (col = 0; col < req->n; ++col) {
            s32 sum = 0;
            for (inner = 0; inner < req->k; ++inner)
                sum += a[row * req->k + inner] * b[inner * req->n + col];
            c[row * req->n + col] = sum;
        }
    }

    return kvdev_dma_write(req->c_dma_addr, c, c_bytes);
}
