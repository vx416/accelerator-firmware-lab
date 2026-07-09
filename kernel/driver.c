#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "afl/ioctl.h"
#include "buffer_pool.h"
#include "driver_dma.h"
#include "driver_transport.h"
#include "virtual_device.h"

static struct afl_kernel_vdev afl_dev;
static DEFINE_MUTEX(afl_async_lock);
static DEFINE_MUTEX(afl_trace_snapshot_lock);
static struct afl_ioctl_trace afl_trace_snapshot;

struct afl_file_ctx {
    struct afl_buffer_pool_owner buffer_owner;
};

struct afl_async_vector_add {
    bool pending;
    struct afl_driver_dma_buffer a_dma;
    struct afl_driver_dma_buffer b_dma;
    struct afl_driver_dma_buffer out_dma;
    afl_ioctl_u64_t out_user_ptr;
    size_t bytes;
};

static struct afl_async_vector_add afl_async_vector;

static void afl_async_vector_clear_locked(void)
{
    afl_driver_dma_free(&afl_async_vector.out_dma);
    afl_driver_dma_free(&afl_async_vector.b_dma);
    afl_driver_dma_free(&afl_async_vector.a_dma);
    memset(&afl_async_vector, 0, sizeof(afl_async_vector));
}

static int afl_async_is_pending(void)
{
    int pending;

    mutex_lock(&afl_async_lock);
    pending = afl_async_vector.pending;
    mutex_unlock(&afl_async_lock);
    return pending;
}

static int afl_driver_vector_bytes(const struct afl_ioctl_vector_add *req, size_t *bytes_out)
{
    if (req->dtype != AFL_IOCTL_DTYPE_I32 || req->element_count == 0)
        return -EINVAL;
    if (req->element_count > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(s32))
        return -EINVAL;

    *bytes_out = (size_t)req->element_count * sizeof(s32);
    return 0;
}

static int afl_driver_matrix_bytes(afl_ioctl_u32_t rows, afl_ioctl_u32_t cols, size_t *bytes_out)
{
    if (rows == 0 || cols == 0)
        return -EINVAL;
    if (rows > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(s32) / cols)
        return -EINVAL;

    *bytes_out = (size_t)rows * cols * sizeof(s32);
    return 0;
}

static long afl_ioctl_memcopy(unsigned long arg)
{
    struct afl_ioctl_memcopy req;
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    struct afl_driver_dma_buffer src_dma;
    struct afl_driver_dma_buffer dst_dma;
    long ret;

    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));
    memset(&src_dma, 0, sizeof(src_dma));
    memset(&dst_dma, 0, sizeof(dst_dma));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;
    if (req.length == 0 || req.length > AFL_IOCTL_MAX_TRANSFER_BYTES)
        return -EINVAL;

    ret = afl_driver_dma_stage_from_user(&src_dma, req.src_user_ptr, req.length);
    if (ret != 0)
        return ret;
    ret = afl_driver_dma_alloc(&dst_dma, req.length);
    if (ret != 0)
        goto out_free_src;

    command.opcode = AFL_KVDEV_OP_MEMCOPY;
    command.payload.memcopy.src_dma_addr = src_dma.dma_addr;
    command.payload.memcopy.dst_dma_addr = dst_dma.dma_addr;
    command.payload.memcopy.length = req.length;
    command.payload.memcopy.flags = req.flags;

    ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
    if (ret == 0)
        ret = completion.result;
    if (ret == 0)
        ret = afl_driver_dma_copy_to_user(&dst_dma, req.dst_user_ptr, req.length);

    afl_driver_dma_free(&dst_dma);
out_free_src:
    afl_driver_dma_free(&src_dma);
    return ret;
}

static long afl_ioctl_vector_add(unsigned long arg)
{
    struct afl_ioctl_vector_add req;
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    struct afl_driver_dma_buffer a_dma;
    struct afl_driver_dma_buffer b_dma;
    struct afl_driver_dma_buffer out_dma;
    size_t bytes;
    long ret;

    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));
    memset(&a_dma, 0, sizeof(a_dma));
    memset(&b_dma, 0, sizeof(b_dma));
    memset(&out_dma, 0, sizeof(out_dma));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;
    ret = afl_driver_vector_bytes(&req, &bytes);
    if (ret != 0)
        return ret;

    ret = afl_driver_dma_stage_from_user(&a_dma, req.a_user_ptr, bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_dma_stage_from_user(&b_dma, req.b_user_ptr, bytes);
    if (ret != 0)
        goto out_free_a;
    ret = afl_driver_dma_alloc(&out_dma, bytes);
    if (ret != 0)
        goto out_free_b;

    command.opcode = AFL_KVDEV_OP_VECTOR_ADD;
    command.payload.vector_add.a_dma_addr = a_dma.dma_addr;
    command.payload.vector_add.b_dma_addr = b_dma.dma_addr;
    command.payload.vector_add.out_dma_addr = out_dma.dma_addr;
    command.payload.vector_add.element_count = req.element_count;
    command.payload.vector_add.dtype = req.dtype;

    ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
    if (ret == 0)
        ret = completion.result;
    if (ret == 0)
        ret = afl_driver_dma_copy_to_user(&out_dma, req.out_user_ptr, bytes);

    afl_driver_dma_free(&out_dma);
out_free_b:
    afl_driver_dma_free(&b_dma);
out_free_a:
    afl_driver_dma_free(&a_dma);
    return ret;
}

static long afl_ioctl_vector_add_async(unsigned long arg)
{
    struct afl_ioctl_vector_add req;
    struct afl_kvdev_command command;
    struct afl_driver_dma_buffer a_dma;
    struct afl_driver_dma_buffer b_dma;
    struct afl_driver_dma_buffer out_dma;
    size_t bytes;
    long ret;

    memset(&command, 0, sizeof(command));
    memset(&a_dma, 0, sizeof(a_dma));
    memset(&b_dma, 0, sizeof(b_dma));
    memset(&out_dma, 0, sizeof(out_dma));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;
    ret = afl_driver_vector_bytes(&req, &bytes);
    if (ret != 0)
        return ret;

    mutex_lock(&afl_async_lock);
    if (afl_async_vector.pending) {
        mutex_unlock(&afl_async_lock);
        return -EBUSY;
    }
    mutex_unlock(&afl_async_lock);

    ret = afl_driver_dma_stage_from_user(&a_dma, req.a_user_ptr, bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_dma_stage_from_user(&b_dma, req.b_user_ptr, bytes);
    if (ret != 0)
        goto out_free_a;
    ret = afl_driver_dma_alloc(&out_dma, bytes);
    if (ret != 0)
        goto out_free_b;

    command.opcode = AFL_KVDEV_OP_VECTOR_ADD;
    command.payload.vector_add.a_dma_addr = a_dma.dma_addr;
    command.payload.vector_add.b_dma_addr = b_dma.dma_addr;
    command.payload.vector_add.out_dma_addr = out_dma.dma_addr;
    command.payload.vector_add.element_count = req.element_count;
    command.payload.vector_add.dtype = req.dtype;

    mutex_lock(&afl_async_lock);
    if (afl_async_vector.pending) {
        mutex_unlock(&afl_async_lock);
        ret = -EBUSY;
        goto out_free_out;
    }

    afl_async_vector.pending = true;
    afl_async_vector.a_dma = a_dma;
    afl_async_vector.b_dma = b_dma;
    afl_async_vector.out_dma = out_dma;
    afl_async_vector.out_user_ptr = req.out_user_ptr;
    afl_async_vector.bytes = bytes;
    memset(&a_dma, 0, sizeof(a_dma));
    memset(&b_dma, 0, sizeof(b_dma));
    memset(&out_dma, 0, sizeof(out_dma));
    mutex_unlock(&afl_async_lock);

    ret = afl_driver_transport_submit(&afl_dev, &command);
    if (ret != 0) {
        mutex_lock(&afl_async_lock);
        afl_async_vector_clear_locked();
        mutex_unlock(&afl_async_lock);
    }

out_free_out:
    afl_driver_dma_free(&out_dma);
out_free_b:
    afl_driver_dma_free(&b_dma);
out_free_a:
    afl_driver_dma_free(&a_dma);
    return ret;
}

static long afl_ioctl_get_completion(unsigned long arg)
{
    struct afl_kvdev_completion kv_completion;
    struct afl_ioctl_completion user_completion;
    long ret;

    memset(&kv_completion, 0, sizeof(kv_completion));
    memset(&user_completion, 0, sizeof(user_completion));

    mutex_lock(&afl_async_lock);
    if (!afl_async_vector.pending) {
        mutex_unlock(&afl_async_lock);
        return -ENOENT;
    }

    ret = afl_driver_transport_pop_completion(&afl_dev, &kv_completion);
    if (ret != 0) {
        mutex_unlock(&afl_async_lock);
        return ret;
    }

    if (kv_completion.result == 0)
        ret = afl_driver_dma_copy_to_user(&afl_async_vector.out_dma, afl_async_vector.out_user_ptr, afl_async_vector.bytes);

    user_completion.status = kv_completion.status;
    user_completion.error_code = kv_completion.error_code;
    user_completion.result = kv_completion.result == 0 ? (afl_ioctl_u32_t)ret : (afl_ioctl_u32_t)kv_completion.result;

    afl_async_vector_clear_locked();
    mutex_unlock(&afl_async_lock);

    if (copy_to_user((void __user *)arg, &user_completion, sizeof(user_completion)) != 0)
        return -EFAULT;
    return ret;
}

static long afl_ioctl_get_trace(unsigned long arg)
{
    long ret = 0;

    mutex_lock(&afl_trace_snapshot_lock);
    afl_kvdev_get_trace(&afl_dev, &afl_trace_snapshot);
    if (copy_to_user((void __user *)arg, &afl_trace_snapshot, sizeof(afl_trace_snapshot)) != 0)
        ret = -EFAULT;
    mutex_unlock(&afl_trace_snapshot_lock);
    return ret;
}

static long afl_ioctl_matrix_mul(unsigned long arg)
{
    struct afl_ioctl_matrix_mul req;
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    struct afl_driver_dma_buffer a_dma;
    struct afl_driver_dma_buffer b_dma;
    struct afl_driver_dma_buffer c_dma;
    size_t a_bytes;
    size_t b_bytes;
    size_t c_bytes;
    long ret;

    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));
    memset(&a_dma, 0, sizeof(a_dma));
    memset(&b_dma, 0, sizeof(b_dma));
    memset(&c_dma, 0, sizeof(c_dma));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;
    if (req.dtype != AFL_IOCTL_DTYPE_I32)
        return -EINVAL;
    ret = afl_driver_matrix_bytes(req.m, req.k, &a_bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_matrix_bytes(req.k, req.n, &b_bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_matrix_bytes(req.m, req.n, &c_bytes);
    if (ret != 0)
        return ret;

    ret = afl_driver_dma_stage_from_user(&a_dma, req.a_user_ptr, a_bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_dma_stage_from_user(&b_dma, req.b_user_ptr, b_bytes);
    if (ret != 0)
        goto out_free_a;
    ret = afl_driver_dma_alloc(&c_dma, c_bytes);
    if (ret != 0)
        goto out_free_b;

    command.opcode = AFL_KVDEV_OP_MATRIX_MUL;
    command.payload.matrix_mul.a_dma_addr = a_dma.dma_addr;
    command.payload.matrix_mul.b_dma_addr = b_dma.dma_addr;
    command.payload.matrix_mul.c_dma_addr = c_dma.dma_addr;
    command.payload.matrix_mul.m = req.m;
    command.payload.matrix_mul.n = req.n;
    command.payload.matrix_mul.k = req.k;
    command.payload.matrix_mul.dtype = req.dtype;
    command.payload.matrix_mul.flags = req.flags;

    ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
    if (ret == 0)
        ret = completion.result;
    if (ret == 0)
        ret = afl_driver_dma_copy_to_user(&c_dma, req.c_user_ptr, c_bytes);

    afl_driver_dma_free(&c_dma);
out_free_b:
    afl_driver_dma_free(&b_dma);
out_free_a:
    afl_driver_dma_free(&a_dma);
    return ret;
}

static long afl_ioctl_alloc_buffer(struct file *file, unsigned long arg)
{
    struct afl_file_ctx *ctx = file->private_data;
    struct afl_ioctl_alloc_buffer req;
    int ret;

    if (ctx == NULL)
        return -ENODEV;
    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;

    ret = afl_buffer_pool_alloc(ctx, &ctx->buffer_owner, req.size, &req.handle, &req.mmap_offset, &req.allocated_size);
    if (ret != 0)
        return ret;
    req.reserved = 0;

    if (copy_to_user((void __user *)arg, &req, sizeof(req)) != 0) {
        struct afl_ioctl_free_buffer cleanup;

        cleanup.handle = req.handle;
        cleanup.reserved = 0;
        afl_buffer_pool_free(ctx, &ctx->buffer_owner, cleanup.handle);
        return -EFAULT;
    }
    return 0;
}

static long afl_ioctl_free_buffer(struct file *file, unsigned long arg)
{
    struct afl_file_ctx *ctx = file->private_data;
    struct afl_ioctl_free_buffer req;

    if (ctx == NULL)
        return -ENODEV;
    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;

    return afl_buffer_pool_free(ctx, &ctx->buffer_owner, req.handle);
}

static long afl_ioctl_vector_add_buffer(struct file *file, unsigned long arg)
{
    struct afl_file_ctx *ctx = file->private_data;
    struct afl_ioctl_vector_add_buffer req;
    struct afl_ioctl_vector_add shape;
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    size_t bytes;
    long ret;

    if (ctx == NULL)
        return -ENODEV;

    memset(&req, 0, sizeof(req));
    memset(&shape, 0, sizeof(shape));
    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;

    shape.element_count = req.element_count;
    shape.dtype = req.dtype;
    ret = afl_driver_vector_bytes(&shape, &bytes);
    if (ret != 0)
        return ret;

    command.opcode = AFL_KVDEV_OP_VECTOR_ADD;
    ret = afl_buffer_pool_lookup_dma(ctx, req.a_handle, req.a_offset, bytes, &command.payload.vector_add.a_dma_addr);
    if (ret != 0)
        return ret;
    ret = afl_buffer_pool_lookup_dma(ctx, req.b_handle, req.b_offset, bytes, &command.payload.vector_add.b_dma_addr);
    if (ret != 0)
        return ret;
    ret = afl_buffer_pool_lookup_dma(ctx, req.out_handle, req.out_offset, bytes, &command.payload.vector_add.out_dma_addr);
    if (ret != 0)
        return ret;
    command.payload.vector_add.element_count = req.element_count;
    command.payload.vector_add.dtype = req.dtype;

    ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
    if (ret == 0)
        ret = completion.result;
    return ret;
}

static long afl_ioctl_matrix_mul_buffer(struct file *file, unsigned long arg)
{
    struct afl_file_ctx *ctx = file->private_data;
    struct afl_ioctl_matrix_mul_buffer req;
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    size_t a_bytes;
    size_t b_bytes;
    size_t c_bytes;
    long ret;

    if (ctx == NULL)
        return -ENODEV;

    memset(&req, 0, sizeof(req));
    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));

    if (copy_from_user(&req, (const void __user *)arg, sizeof(req)) != 0)
        return -EFAULT;
    if (req.dtype != AFL_IOCTL_DTYPE_I32)
        return -EINVAL;

    ret = afl_driver_matrix_bytes(req.m, req.k, &a_bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_matrix_bytes(req.k, req.n, &b_bytes);
    if (ret != 0)
        return ret;
    ret = afl_driver_matrix_bytes(req.m, req.n, &c_bytes);
    if (ret != 0)
        return ret;

    command.opcode = AFL_KVDEV_OP_MATRIX_MUL;
    ret = afl_buffer_pool_lookup_dma(ctx, req.a_handle, req.a_offset, a_bytes, &command.payload.matrix_mul.a_dma_addr);
    if (ret != 0)
        return ret;
    ret = afl_buffer_pool_lookup_dma(ctx, req.b_handle, req.b_offset, b_bytes, &command.payload.matrix_mul.b_dma_addr);
    if (ret != 0)
        return ret;
    ret = afl_buffer_pool_lookup_dma(ctx, req.c_handle, req.c_offset, c_bytes, &command.payload.matrix_mul.c_dma_addr);
    if (ret != 0)
        return ret;
    command.payload.matrix_mul.m = req.m;
    command.payload.matrix_mul.n = req.n;
    command.payload.matrix_mul.k = req.k;
    command.payload.matrix_mul.dtype = req.dtype;
    command.payload.matrix_mul.flags = req.flags;

    ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
    if (ret == 0)
        ret = completion.result;
    return ret;
}

static long afl_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct afl_kvdev_command command;
    struct afl_kvdev_completion completion;
    long ret = 0;

    memset(&command, 0, sizeof(command));
    memset(&completion, 0, sizeof(completion));

    if (cmd != AFL_IOCTL_GET_COMPLETION && cmd != AFL_IOCTL_GET_TRACE && afl_async_is_pending())
        return -EBUSY;

    switch (cmd) {
    case AFL_IOCTL_GET_VERSION:
        command.opcode = AFL_KVDEV_OP_GET_VERSION;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        if (ret == 0 && copy_to_user((void __user *)arg, &completion.payload.version, sizeof(completion.payload.version)) != 0)
            ret = -EFAULT;
        break;
    case AFL_IOCTL_GET_TELEMETRY:
        command.opcode = AFL_KVDEV_OP_GET_TELEMETRY;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        if (ret == 0 && copy_to_user((void __user *)arg, &completion.payload.telemetry, sizeof(completion.payload.telemetry)) != 0)
            ret = -EFAULT;
        break;
    case AFL_IOCTL_RESET:
        command.opcode = AFL_KVDEV_OP_RESET;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        break;
    case AFL_IOCTL_GET_STATUS:
        command.opcode = AFL_KVDEV_OP_GET_STATUS;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        if (ret == 0 && copy_to_user((void __user *)arg, &completion.payload.device_status, sizeof(completion.payload.device_status)) != 0)
            ret = -EFAULT;
        break;
    case AFL_IOCTL_RUN_SELFTEST:
        command.opcode = AFL_KVDEV_OP_RUN_SELFTEST;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        break;
    case AFL_IOCTL_MEMCOPY:
        ret = afl_ioctl_memcopy(arg);
        break;
    case AFL_IOCTL_VECTOR_ADD:
        ret = afl_ioctl_vector_add(arg);
        break;
    case AFL_IOCTL_VECTOR_ADD_ASYNC:
        ret = afl_ioctl_vector_add_async(arg);
        break;
    case AFL_IOCTL_GET_COMPLETION:
        ret = afl_ioctl_get_completion(arg);
        break;
    case AFL_IOCTL_GET_TRACE:
        ret = afl_ioctl_get_trace(arg);
        break;
    case AFL_IOCTL_MATRIX_MUL:
        ret = afl_ioctl_matrix_mul(arg);
        break;
    case AFL_IOCTL_ALLOC_BUFFER:
        ret = afl_ioctl_alloc_buffer(file, arg);
        break;
    case AFL_IOCTL_FREE_BUFFER:
        ret = afl_ioctl_free_buffer(file, arg);
        break;
    case AFL_IOCTL_VECTOR_ADD_BUFFER:
        ret = afl_ioctl_vector_add_buffer(file, arg);
        break;
    case AFL_IOCTL_MATRIX_MUL_BUFFER:
        ret = afl_ioctl_matrix_mul_buffer(file, arg);
        break;
    case AFL_IOCTL_TRIGGER_FAULT:
        command.opcode = AFL_KVDEV_OP_TRIGGER_FAULT;
        if (copy_from_user(&command.payload.fault, (const void __user *)arg, sizeof(command.payload.fault)) != 0) {
            ret = -EFAULT;
            break;
        }
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        if (ret == 0 && copy_to_user((void __user *)arg, &completion.payload.fault, sizeof(completion.payload.fault)) != 0)
            ret = -EFAULT;
        break;
    case AFL_IOCTL_TRIGGER_TIMEOUT: {
        struct afl_ioctl_completion user_completion;

        command.opcode = AFL_KVDEV_OP_TRIGGER_TIMEOUT;
        ret = afl_driver_transport_submit_and_wait(&afl_dev, &command, &completion);
        if (ret != 0)
            break;
        memset(&user_completion, 0, sizeof(user_completion));
        user_completion.status = completion.status;
        user_completion.error_code = completion.error_code;
        user_completion.result = (afl_ioctl_u32_t)completion.result;
        if (copy_to_user((void __user *)arg, &user_completion, sizeof(user_completion)) != 0)
            ret = -EFAULT;
        break;
    }
    default:
        ret = -ENOTTY;
        break;
    }

    return ret;
}

static int afl_open(struct inode *inode, struct file *file)
{
    struct afl_file_ctx *ctx;

    (void)inode;
    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (ctx == NULL)
        return -ENOMEM;

    file->private_data = ctx;
    return 0;
}

static int afl_release(struct inode *inode, struct file *file)
{
    struct afl_file_ctx *ctx = file->private_data;

    (void)inode;
    if (ctx != NULL) {
        afl_buffer_pool_free_owner(ctx, &ctx->buffer_owner);
        kfree(ctx);
        file->private_data = NULL;
    }
    return 0;
}

static __poll_t afl_poll(struct file *file, poll_table *wait)
{
    (void)file;
    poll_wait(file, afl_driver_transport_irq_wait_queue(&afl_dev), wait);
    return afl_driver_transport_poll_mask(&afl_dev);
}

static int afl_mmap(struct file *file, struct vm_area_struct *vma)
{
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long byte_offset = vma->vm_pgoff << PAGE_SHIFT;

    return afl_buffer_pool_mmap(file->private_data, vma, byte_offset, size);
}

static const struct file_operations afl_fops = {
    .owner = THIS_MODULE,
    .open = afl_open,
    .release = afl_release,
    .mmap = afl_mmap,
    .poll = afl_poll,
    .unlocked_ioctl = afl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = afl_ioctl,
#endif
};

static struct miscdevice afl_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "afl0",
    .fops = &afl_fops,
    .mode = 0666,
};

static int __init afl_kernel_init(void)
{
    int ret;

    afl_kvdev_init(&afl_dev);
    ret = afl_buffer_pool_init();
    if (ret != 0)
        return ret;

    ret = misc_register(&afl_miscdev);
    if (ret != 0) {
        afl_buffer_pool_destroy();
        return ret;
    }

    pr_info("afl_kernel: registered /dev/%s\n", afl_miscdev.name);
    return 0;
}

static void __exit afl_kernel_exit(void)
{
    misc_deregister(&afl_miscdev);
    afl_buffer_pool_destroy();
    pr_info("afl_kernel: unregistered /dev/%s\n", afl_miscdev.name);
}

module_init(afl_kernel_init);
module_exit(afl_kernel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("accelerator-firmware-lab");
MODULE_DESCRIPTION("Minimal accelerator firmware lab kernel backend");
