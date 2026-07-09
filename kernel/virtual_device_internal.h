#ifndef AFL_KERNEL_VIRTUAL_DEVICE_INTERNAL_H
#define AFL_KERNEL_VIRTUAL_DEVICE_INTERNAL_H

#include "virtual_device.h"

afl_ioctl_u32_t kvdev_abi_version(void);
afl_ioctl_u32_t kvdev_fw_version(void);
afl_ioctl_u32_t kvdev_mmio_read32(const struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg);
void kvdev_mmio_write32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg, afl_ioctl_u32_t value);
void kvdev_refresh_counter_registers(struct afl_kernel_vdev *vdev);
void kvdev_init_identity_registers(struct afl_kernel_vdev *vdev);
void kvdev_fill_version(struct afl_ioctl_version *version);
void kvdev_fill_status_locked(struct afl_kernel_vdev *vdev, struct afl_ioctl_status *status);

void kvdev_trace_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t event, afl_ioctl_u32_t opcode, afl_ioctl_u32_t status, afl_ioctl_u32_t error);

void kvdev_irq_raise_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits);
void kvdev_irq_raise_vector_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t vector);
void kvdev_irq_clear_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits);

afl_ioctl_u64_t kvdev_cycle_budget(afl_ioctl_u32_t opcode);
afl_ioctl_u32_t kvdev_errno_to_error(int ret);
afl_ioctl_u32_t kvdev_fault_to_error(afl_ioctl_u32_t fault);
void kvdev_set_ready(struct afl_kernel_vdev *vdev);
void kvdev_set_error(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t error);
void kvdev_begin_command_locked(struct afl_kernel_vdev *vdev);
afl_ioctl_u32_t kvdev_finish_command_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t opcode, afl_ioctl_u32_t error, afl_ioctl_u64_t cycles);
void kvdev_complete_locked(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion, afl_ioctl_u32_t opcode, int result, afl_ioctl_u64_t cycles);
void kvdev_execute_command_locked(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);

int kvdev_do_memcopy(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);
int kvdev_do_vector_add(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);
int kvdev_do_matrix_mul(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);

void kvdev_push_completion_locked(struct afl_kernel_vdev *vdev, const struct afl_kvdev_completion *completion);
int kvdev_pop_completion_locked(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion);
void kvdev_process_command_queue_locked(struct afl_kernel_vdev *vdev);
void kvdev_ring_doorbell_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t doorbell_bits);

#endif
