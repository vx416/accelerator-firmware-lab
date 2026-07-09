#include "virtual_device_internal.h"

#include <linux/errno.h>

void kvdev_push_completion_locked(struct afl_kernel_vdev *vdev, const struct afl_kvdev_completion *completion)
{
    afl_ioctl_u32_t next_tail = (vdev->comp_tail + 1u) % AFL_KERNEL_QUEUE_DEPTH;

    if (next_tail == vdev->comp_head) {
        kvdev_set_error(vdev, AFL_IOCTL_ERR_QUEUE_FULL);
        return;
    }

    vdev->completion_queue[vdev->comp_tail] = *completion;
    vdev->comp_tail = next_tail;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_COMPLETION_QUEUE_TAIL, vdev->comp_tail);
    kvdev_irq_raise_locked(vdev, AFL_KERNEL_IRQ_COMPLETION);
}

int kvdev_pop_completion_locked(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion)
{
    if (vdev->comp_head == vdev->comp_tail)
        return -ENOENT;

    *completion = vdev->completion_queue[vdev->comp_head];
    vdev->comp_head = (vdev->comp_head + 1u) % AFL_KERNEL_QUEUE_DEPTH;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_COMPLETION_QUEUE_HEAD, vdev->comp_head);
    if (vdev->comp_head == vdev->comp_tail)
        kvdev_irq_clear_locked(vdev, AFL_KERNEL_IRQ_COMPLETION);
    return 0;
}

void kvdev_process_command_queue_locked(struct afl_kernel_vdev *vdev)
{
    while (vdev->cq_head != vdev->cq_tail) {
        struct afl_kvdev_command command = vdev->command_queue[vdev->cq_head];
        vdev->cq_head = (vdev->cq_head + 1u) % AFL_KERNEL_QUEUE_DEPTH;
        kvdev_mmio_write32(vdev, AFL_IOCTL_REG_COMMAND_QUEUE_HEAD, vdev->cq_head);
        kvdev_execute_command_locked(vdev, &command);
    }
}

void kvdev_ring_doorbell_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t doorbell_bits)
{
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DOORBELL, doorbell_bits);
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_DOORBELL, 0, doorbell_bits, AFL_IOCTL_ERR_NONE);
    if (doorbell_bits & AFL_KERNEL_DOORBELL_COMMAND_QUEUE)
        kvdev_process_command_queue_locked(vdev);
}
