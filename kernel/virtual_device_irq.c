#include "virtual_device_internal.h"

static afl_ioctl_u32_t kvdev_vector_bit(afl_ioctl_u32_t vector)
{
    return vector < AFL_IOCTL_IRQ_NUM_VECTORS ? (1u << vector) : 0;
}

static afl_ioctl_u32_t kvdev_reason_to_vector(afl_ioctl_u32_t irq_bits)
{
    if (irq_bits & AFL_KERNEL_IRQ_ERROR)
        return AFL_IOCTL_IRQ_VECTOR_ERROR;
    if (irq_bits & AFL_KERNEL_IRQ_RESET_DONE)
        return AFL_IOCTL_IRQ_VECTOR_ADMIN;
    return AFL_IOCTL_IRQ_VECTOR_COMPLETION;
}

void kvdev_irq_raise_vector_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t vector)
{
    afl_ioctl_u32_t bit = kvdev_vector_bit(vector);

    if (bit == 0)
        return;
    if ((vdev->irq_vector_enable & bit) == 0 || (vdev->irq_vector_mask & bit) != 0)
        return;

    vdev->irq_vector_pending |= bit;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_STATUS, vdev->irq_vector_pending);
}

void kvdev_irq_raise_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits)
{
    vdev->irq_pending |= irq_bits;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_STATUS, vdev->irq_pending);
    kvdev_irq_raise_vector_locked(vdev, kvdev_reason_to_vector(irq_bits));
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_IRQ_RAISE, 0, irq_bits, kvdev_mmio_read32(vdev, AFL_KERNEL_REG_ERROR_CODE));
    wake_up_interruptible(&vdev->irq_wait);
}

void kvdev_irq_clear_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits)
{
    vdev->irq_pending &= ~irq_bits;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_STATUS, vdev->irq_pending);
}

int afl_kvdev_wait_irq(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits)
{
    return wait_event_interruptible(vdev->irq_wait, vdev->irq_pending & irq_bits);
}

wait_queue_head_t *afl_kvdev_irq_wait_queue(struct afl_kernel_vdev *vdev)
{
    return &vdev->irq_wait;
}

__poll_t afl_kvdev_poll_mask(struct afl_kernel_vdev *vdev)
{
    __poll_t mask = 0;

    mutex_lock(&vdev->lock);
    if (vdev->irq_pending != 0)
        mask |= POLLIN | POLLRDNORM;
    mutex_unlock(&vdev->lock);

    return mask;
}
