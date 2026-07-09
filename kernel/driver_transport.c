#include "driver_transport.h"

int afl_driver_transport_submit_and_wait(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command, struct afl_kvdev_completion *completion)
{
    int ret;

    ret = afl_driver_transport_submit(vdev, command);
    if (ret != 0)
        return ret;

    ret = afl_kvdev_wait_irq(vdev, AFL_KERNEL_IRQ_COMPLETION);
    if (ret != 0)
        return ret;

    return afl_driver_transport_pop_completion(vdev, completion);
}

int afl_driver_transport_submit(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    int ret;

    ret = afl_kvdev_submit_command(vdev, command);
    if (ret != 0)
        return ret;

    afl_kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DOORBELL, AFL_KERNEL_DOORBELL_COMMAND_QUEUE);
    return 0;
}

int afl_driver_transport_pop_completion(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion)
{
    return afl_kvdev_pop_completion(vdev, completion);
}

wait_queue_head_t *afl_driver_transport_irq_wait_queue(struct afl_kernel_vdev *vdev)
{
    return afl_kvdev_irq_wait_queue(vdev);
}

__poll_t afl_driver_transport_poll_mask(struct afl_kernel_vdev *vdev)
{
    return afl_kvdev_poll_mask(vdev);
}
