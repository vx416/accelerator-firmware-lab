#include "virtual_device_internal.h"

afl_ioctl_u32_t kvdev_abi_version(void)
{
    return AFL_IOCTL_PACK_VERSION(AFL_IOCTL_ABI_VERSION_MAJOR, AFL_IOCTL_ABI_VERSION_MINOR, 0);
}

afl_ioctl_u32_t kvdev_fw_version(void)
{
    return AFL_IOCTL_PACK_VERSION(AFL_IOCTL_FW_VERSION_MAJOR, AFL_IOCTL_FW_VERSION_MINOR, AFL_IOCTL_FW_VERSION_PATCH);
}

afl_ioctl_u32_t kvdev_mmio_read32(const struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg)
{
    return reg < AFL_KERNEL_REG_COUNT ? vdev->registers[reg] : 0;
}

void kvdev_mmio_write32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg, afl_ioctl_u32_t value)
{
    if (reg < AFL_KERNEL_REG_COUNT)
        vdev->registers[reg] = value;
}

void kvdev_refresh_counter_registers(struct afl_kernel_vdev *vdev)
{
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_COMMAND_COUNT_LO, (afl_ioctl_u32_t)vdev->telemetry.command_count);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_RESET_COUNT_LO, (afl_ioctl_u32_t)vdev->telemetry.reset_count);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_TRACE_HEAD, vdev->trace_head);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_TRACE_COUNT, vdev->trace_count);
}

void kvdev_init_identity_registers(struct afl_kernel_vdev *vdev)
{
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_DEVICE_ID, AFL_IOCTL_DEVICE_ID);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_VENDOR_ID, AFL_IOCTL_VENDOR_ID);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_ABI_VERSION, kvdev_abi_version());
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_FW_VERSION, kvdev_fw_version());
    vdev->irq_vector_enable = (1u << AFL_IOCTL_IRQ_NUM_VECTORS) - 1u;
    vdev->irq_vector_mask = 0;
    vdev->irq_vector_pending = 0;
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_STATUS, vdev->irq_vector_pending);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_MASK, vdev->irq_vector_mask);
    kvdev_mmio_write32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_ENABLE, vdev->irq_vector_enable);
}

void kvdev_fill_version(struct afl_ioctl_version *version)
{
    version->device_id = AFL_IOCTL_DEVICE_ID;
    version->vendor_id = AFL_IOCTL_VENDOR_ID;
    version->abi_version = kvdev_abi_version();
    version->fw_version = kvdev_fw_version();
}

void kvdev_fill_status_locked(struct afl_kernel_vdev *vdev, struct afl_ioctl_status *status)
{
    status->device_id = AFL_IOCTL_DEVICE_ID;
    status->vendor_id = AFL_IOCTL_VENDOR_ID;
    status->abi_version = kvdev_abi_version();
    status->fw_version = kvdev_fw_version();
    status->device_status = kvdev_mmio_read32(vdev, AFL_KERNEL_REG_DEVICE_STATUS);
    status->error_code = kvdev_mmio_read32(vdev, AFL_KERNEL_REG_ERROR_CODE);
    status->command_count = (afl_ioctl_u32_t)vdev->telemetry.command_count;
    status->reset_count = (afl_ioctl_u32_t)vdev->telemetry.reset_count;
    status->doorbell = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_DOORBELL);
    status->irq_status = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_IRQ_STATUS);
    status->command_queue_head = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_COMMAND_QUEUE_HEAD);
    status->command_queue_tail = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_COMMAND_QUEUE_TAIL);
    status->completion_queue_head = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_COMPLETION_QUEUE_HEAD);
    status->completion_queue_tail = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_COMPLETION_QUEUE_TAIL);
    status->trace_head = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_TRACE_HEAD);
    status->trace_count = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_TRACE_COUNT);
    status->irq_vector_status = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_STATUS);
    status->irq_vector_mask = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_MASK);
    status->irq_vector_enable = kvdev_mmio_read32(vdev, AFL_IOCTL_REG_IRQ_VECTOR_ENABLE);
}
