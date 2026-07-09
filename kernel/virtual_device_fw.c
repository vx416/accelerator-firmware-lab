#include "virtual_device_internal.h"

#include <linux/errno.h>
#include <linux/string.h>

afl_ioctl_u64_t kvdev_cycle_budget(afl_ioctl_u32_t opcode)
{
    switch (opcode) {
    case AFL_KVDEV_OP_MEMCOPY:
        return 4096;
    case AFL_KVDEV_OP_VECTOR_ADD:
        return 4096;
    case AFL_KVDEV_OP_MATRIX_MUL:
        return 8192;
    case AFL_KVDEV_OP_TRIGGER_TIMEOUT:
        return 100;
    default:
        return 1024;
    }
}

afl_ioctl_u32_t kvdev_errno_to_error(int ret)
{
    switch (ret) {
    case 0:
        return AFL_IOCTL_ERR_NONE;
    case -EFAULT:
    case -EINVAL:
        return AFL_IOCTL_ERR_INVALID_MEMORY_RANGE;
    case -ENOMEM:
    default:
        return AFL_IOCTL_ERR_INVALID_STATE;
    }
}

afl_ioctl_u32_t kvdev_fault_to_error(afl_ioctl_u32_t fault)
{
    switch (fault) {
    case AFL_IOCTL_FAULT_FW_PANIC:
        return AFL_IOCTL_ERR_FW_PANIC;
    case AFL_IOCTL_FAULT_DEVICE_HANG:
        return AFL_IOCTL_ERR_DEVICE_HANG;
    case AFL_IOCTL_FAULT_ECC:
        return AFL_IOCTL_ERR_ECC;
    case AFL_IOCTL_FAULT_RECOVERY_FAILURE:
        return AFL_IOCTL_ERR_RECOVERY_FAILED;
    case AFL_IOCTL_FAULT_INVALID_OPCODE:
    default:
        return AFL_IOCTL_ERR_INVALID_OPCODE;
    }
}

void kvdev_set_ready(struct afl_kernel_vdev *vdev)
{
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DEVICE_STATUS, AFL_IOCTL_STATUS_READY | AFL_IOCTL_STATUS_SELF_TEST_DONE);
    vdev->telemetry.current_fw_state = AFL_IOCTL_FW_STATE_READY;
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_ERROR_CODE, AFL_IOCTL_ERR_NONE);
    vdev->telemetry.last_error_code = AFL_IOCTL_ERR_NONE;
}

void kvdev_set_error(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t error)
{
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DEVICE_STATUS, AFL_IOCTL_STATUS_ERROR);
    vdev->telemetry.current_fw_state = AFL_IOCTL_FW_STATE_ERROR;
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_ERROR_CODE, error);
    vdev->telemetry.last_error_code = error;
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_ERROR, 0, AFL_IOCTL_STATUS_ERROR, error);
    kvdev_irq_raise_locked(vdev, AFL_KERNEL_IRQ_ERROR);
}

static void kvdev_enter_recovery_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t error)
{
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DEVICE_STATUS, AFL_IOCTL_STATUS_RECOVERY_ACTIVE | AFL_IOCTL_STATUS_ERROR);
    vdev->telemetry.current_fw_state = AFL_IOCTL_FW_STATE_RECOVERY;
    vdev->telemetry.recovery_count++;
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_RECOVERY_ENTER, 0, AFL_IOCTL_STATUS_RECOVERY_ACTIVE, error);
}

static void kvdev_complete_recovery_locked(struct afl_kernel_vdev *vdev)
{
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_RECOVERY_COMPLETE, 0, AFL_IOCTL_STATUS_READY, AFL_IOCTL_ERR_NONE);
    kvdev_set_ready(vdev);
}

void kvdev_begin_command_locked(struct afl_kernel_vdev *vdev)
{
    kvdev_irq_clear_locked(vdev, AFL_KERNEL_IRQ_COMPLETION | AFL_KERNEL_IRQ_ERROR | AFL_KERNEL_IRQ_RESET_DONE);
    kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DEVICE_STATUS, AFL_IOCTL_STATUS_RUNNING);
    vdev->telemetry.current_fw_state = AFL_IOCTL_FW_STATE_RUNNING;
}

afl_ioctl_u32_t kvdev_finish_command_locked(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t opcode, afl_ioctl_u32_t error, afl_ioctl_u64_t cycles)
{
    afl_ioctl_u64_t budget = kvdev_cycle_budget(opcode);

    if (error == AFL_IOCTL_ERR_NONE && cycles > budget) {
        error = AFL_IOCTL_ERR_TIMEOUT;
        kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_WATCHDOG_TIMEOUT, opcode, (afl_ioctl_u32_t)budget, error);
    }

    vdev->telemetry.command_count++;
    vdev->telemetry.completion_count++;
    vdev->cycle_counter += cycles;
    vdev->telemetry.last_command_cycles = cycles;
    kvdev_refresh_counter_registers(vdev);

    if (error == AFL_IOCTL_ERR_NONE) {
        kvdev_set_ready(vdev);
        kvdev_irq_raise_locked(vdev, AFL_KERNEL_IRQ_COMPLETION);
        return error;
    }

    vdev->telemetry.failed_command_count++;
    if (error == AFL_IOCTL_ERR_TIMEOUT || error == AFL_IOCTL_ERR_DEVICE_HANG)
        vdev->telemetry.timeout_count++;
    kvdev_set_error(vdev, error);
    if (error == AFL_IOCTL_ERR_TIMEOUT || error == AFL_IOCTL_ERR_DEVICE_HANG) {
        kvdev_enter_recovery_locked(vdev, error);
        kvdev_complete_recovery_locked(vdev);
    }
    kvdev_irq_raise_locked(vdev, AFL_KERNEL_IRQ_COMPLETION);
    return error;
}

void kvdev_complete_locked(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion, afl_ioctl_u32_t opcode, int result, afl_ioctl_u64_t cycles)
{
    afl_ioctl_u32_t error = kvdev_errno_to_error(result);

    error = kvdev_finish_command_locked(vdev, opcode, error, cycles);
    completion->result = result;
    completion->error_code = error;
    completion->status = error == AFL_IOCTL_ERR_NONE ? AFL_IOCTL_CMD_STATUS_SUCCESS :
        (error == AFL_IOCTL_ERR_TIMEOUT ? AFL_IOCTL_CMD_STATUS_TIMEOUT : AFL_IOCTL_CMD_STATUS_FAILED);
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_COMMAND_COMPLETE, opcode, completion->status, error);
    kvdev_push_completion_locked(vdev, completion);
}

void kvdev_execute_command_locked(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command)
{
    struct afl_kvdev_completion completion;
    int ret;

    memset(&completion, 0, sizeof(completion));
    kvdev_begin_command_locked(vdev);
    kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_COMMAND_START, command->opcode, 0, AFL_IOCTL_ERR_NONE);

    switch (command->opcode) {
    case AFL_KVDEV_OP_GET_VERSION:
        ret = 0;
        kvdev_fill_version(&completion.payload.version);
        kvdev_complete_locked(vdev, &completion, command->opcode, ret, 100);
        break;
    case AFL_KVDEV_OP_GET_TELEMETRY:
        kvdev_finish_command_locked(vdev, command->opcode, AFL_IOCTL_ERR_NONE, 100);
        completion.result = 0;
        completion.status = AFL_IOCTL_CMD_STATUS_SUCCESS;
        completion.error_code = AFL_IOCTL_ERR_NONE;
        completion.payload.telemetry = vdev->telemetry;
        kvdev_irq_raise_vector_locked(vdev, AFL_IOCTL_IRQ_VECTOR_TELEMETRY);
        kvdev_push_completion_locked(vdev, &completion);
        break;
    case AFL_KVDEV_OP_GET_STATUS:
        kvdev_finish_command_locked(vdev, command->opcode, AFL_IOCTL_ERR_NONE, 100);
        completion.result = 0;
        completion.status = AFL_IOCTL_CMD_STATUS_SUCCESS;
        completion.error_code = AFL_IOCTL_ERR_NONE;
        kvdev_fill_status_locked(vdev, &completion.payload.device_status);
        kvdev_push_completion_locked(vdev, &completion);
        break;
    case AFL_KVDEV_OP_RESET:
        vdev->telemetry.reset_count++;
        kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_RESET, command->opcode, 0, AFL_IOCTL_ERR_NONE);
        kvdev_complete_locked(vdev, &completion, command->opcode, 0, 100);
        kvdev_set_ready(vdev);
        kvdev_irq_raise_locked(vdev, AFL_KERNEL_IRQ_RESET_DONE);
        break;
    case AFL_KVDEV_OP_RUN_SELFTEST:
        kvdev_mmio_write32(vdev, AFL_KERNEL_REG_DEVICE_STATUS, AFL_IOCTL_STATUS_RUNNING);
        vdev->telemetry.current_fw_state = AFL_IOCTL_FW_STATE_SELF_TEST;
        kvdev_complete_locked(vdev, &completion, command->opcode, 0, 100);
        break;
    case AFL_KVDEV_OP_MEMCOPY:
        ret = kvdev_do_memcopy(vdev, command);
        kvdev_complete_locked(vdev, &completion, command->opcode, ret, 100 + command->payload.memcopy.length);
        break;
    case AFL_KVDEV_OP_VECTOR_ADD:
        ret = kvdev_do_vector_add(vdev, command);
        kvdev_complete_locked(vdev, &completion, command->opcode, ret, 100 + (afl_ioctl_u64_t)command->payload.vector_add.element_count * sizeof(s32));
        break;
    case AFL_KVDEV_OP_MATRIX_MUL:
        ret = kvdev_do_matrix_mul(vdev, command);
        kvdev_complete_locked(vdev, &completion, command->opcode, ret, 100 + (afl_ioctl_u64_t)command->payload.matrix_mul.m * command->payload.matrix_mul.n * sizeof(s32));
        break;
    case AFL_KVDEV_OP_TRIGGER_FAULT:
        completion.payload.fault = command->payload.fault;
        completion.payload.fault.status = AFL_IOCTL_CMD_STATUS_FAILED;
        completion.payload.fault.error_code = kvdev_fault_to_error(command->payload.fault.fault_kind);
        completion.result = 0;
        completion.status = AFL_IOCTL_CMD_STATUS_FAILED;
        completion.error_code = kvdev_finish_command_locked(vdev, command->opcode, completion.payload.fault.error_code, 100);
        kvdev_trace_locked(vdev, AFL_IOCTL_TRACE_COMMAND_COMPLETE, command->opcode, completion.status, completion.error_code);
        kvdev_push_completion_locked(vdev, &completion);
        break;
    case AFL_KVDEV_OP_TRIGGER_TIMEOUT:
        kvdev_complete_locked(vdev, &completion, command->opcode, 0, kvdev_cycle_budget(command->opcode) + 1u);
        break;
    default:
        kvdev_complete_locked(vdev, &completion, command->opcode, -EINVAL, 100);
        break;
    }
}
