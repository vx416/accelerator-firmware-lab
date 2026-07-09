#ifndef AFL_KERNEL_DRIVER_TRANSPORT_H
#define AFL_KERNEL_DRIVER_TRANSPORT_H

#include <linux/poll.h>
#include <linux/wait.h>

#include "virtual_device.h"

int afl_driver_transport_submit_and_wait(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command, struct afl_kvdev_completion *completion);
int afl_driver_transport_submit(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);
int afl_driver_transport_pop_completion(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion);
wait_queue_head_t *afl_driver_transport_irq_wait_queue(struct afl_kernel_vdev *vdev);
__poll_t afl_driver_transport_poll_mask(struct afl_kernel_vdev *vdev);

#endif
