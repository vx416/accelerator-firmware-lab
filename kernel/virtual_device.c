#include "virtual_device_internal.h"

#include <linux/errno.h>
#include <linux/string.h>

void afl_kvdev_init(struct afl_kernel_vdev *vdev)
{
    memset(vdev, 0, sizeof(*vdev));
    mutex_init(&vdev->lock);
    init_waitqueue_head(&vdev->irq_wait);
    vdev->telemetry.boot_count = 1;
    kvdev_init_identity_registers(vdev);
    kvdev_set_ready(vdev);
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_BOOT, 0, AFL_IOCTL_STATUS_READY, AFL_IOCTL_ERR_NONE);
}

afl_ioctl_u32_t afl_kvdev_mmio_read32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg)
{
    afl_ioctl_u32_t value;

    mutex_lock(&vdev->lock);
    value = kvdev_mmio_read32(vdev, reg);
    mutex_unlock(&vdev->lock);
    return value;
}

void afl_kvdev_mmio_write32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg, afl_ioctl_u32_t value)
{
    mutex_lock(&vdev->lock);
    if (reg == AFL_KERNEL_REG_DOORBELL)
        kvdev_ring_doorbell_locked(vdev, value);
    else
        kvdev_mmio_write32(vdev, reg, value);
    mutex_unlock(&vdev->lock);
}

int afl_kvdev_submit_command(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    afl_ioctl_u32_t next_tail;

    mutex_lock(&vdev->lock);
    next_tail = (vdev->cq_tail + 1u) % AFL_KERNEL_QUEUE_DEPTH;
    if (next_tail == vdev->cq_head) {
        mutex_unlock(&vdev->lock);
        return -EBUSY;
    }

    vdev->command_queue[vdev->cq_tail] = *command;
    vdev->cq_tail = next_tail;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_COMMAND_QUEUE_TAIL, vdev->cq_tail);
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_COMMAND_QUEUED, command->opcode, 0, AFL_IOCTL_ERR_NONE);
    mutex_unlock(&vdev->lock);
    return 0;
}

int afl_kvdev_pop_completion(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion)
{
    int ret;

    mutex_lock(&vdev->lock);
    ret = kvdev_pop_completion_locked(vdev, completion);
    mutex_unlock(&vdev->lock);
    return ret;
}
