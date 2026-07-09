#ifndef AFL_KERNEL_VIRTUAL_DEVICE_H
#define AFL_KERNEL_VIRTUAL_DEVICE_H

#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/types.h>
#include <linux/wait.h>

#include "afl/ioctl.h"

#define AFL_KERNEL_REG_COUNT AFL_IOCTL_REG_COUNT
#define AFL_KERNEL_REG_DEVICE_STATUS AFL_IOCTL_REG_DEVICE_STATUS
#define AFL_KERNEL_REG_ERROR_CODE AFL_IOCTL_REG_ERROR_CODE
#define AFL_KERNEL_REG_DOORBELL AFL_IOCTL_REG_DOORBELL

#define AFL_KERNEL_QUEUE_DEPTH 16u
#define AFL_KERNEL_SCRATCH_SLOT_COUNT 3u
#define AFL_KERNEL_SCRATCH_SLOT_BYTES AFL_IOCTL_MAX_TRANSFER_BYTES

#define AFL_KERNEL_IRQ_COMPLETION AFL_IOCTL_IRQ_COMPLETION
#define AFL_KERNEL_IRQ_ERROR AFL_IOCTL_IRQ_ERROR
#define AFL_KERNEL_IRQ_RESET_DONE AFL_IOCTL_IRQ_RESET_DONE

#define AFL_KERNEL_DOORBELL_COMMAND_QUEUE AFL_IOCTL_DOORBELL_COMMAND_QUEUE

enum afl_kvdev_opcode {
    AFL_KVDEV_OP_GET_VERSION = 1,
    AFL_KVDEV_OP_GET_TELEMETRY = 2,
    AFL_KVDEV_OP_GET_STATUS = 3,
    AFL_KVDEV_OP_RESET = 4,
    AFL_KVDEV_OP_RUN_SELFTEST = 5,
    AFL_KVDEV_OP_MEMCOPY = 6,
    AFL_KVDEV_OP_VECTOR_ADD = 7,
    AFL_KVDEV_OP_MATRIX_MUL = 8,
    AFL_KVDEV_OP_TRIGGER_FAULT = 9,
    AFL_KVDEV_OP_TRIGGER_TIMEOUT = 10
};

struct afl_kvdev_command {
    enum afl_kvdev_opcode opcode;
    union {
        struct {
            afl_ioctl_u64_t src_dma_addr;
            afl_ioctl_u64_t dst_dma_addr;
            afl_ioctl_u32_t length;
            afl_ioctl_u32_t flags;
        } memcopy;
        struct {
            afl_ioctl_u64_t a_dma_addr;
            afl_ioctl_u64_t b_dma_addr;
            afl_ioctl_u64_t out_dma_addr;
            afl_ioctl_u32_t element_count;
            afl_ioctl_u32_t dtype;
        } vector_add;
        struct {
            afl_ioctl_u64_t a_dma_addr;
            afl_ioctl_u64_t b_dma_addr;
            afl_ioctl_u64_t c_dma_addr;
            afl_ioctl_u32_t m;
            afl_ioctl_u32_t n;
            afl_ioctl_u32_t k;
            afl_ioctl_u32_t dtype;
            afl_ioctl_u32_t flags;
        } matrix_mul;
        struct afl_ioctl_fault fault;
    } payload;
};

struct afl_kvdev_completion {
    afl_ioctl_u32_t status;
    afl_ioctl_u32_t error_code;
    int result;
    union {
        struct afl_ioctl_version version;
        struct afl_ioctl_telemetry telemetry;
        struct afl_ioctl_status device_status;
        struct afl_ioctl_fault fault;
    } payload;
};

struct afl_kernel_vdev {
    struct mutex lock;
    wait_queue_head_t irq_wait;
    afl_ioctl_u32_t registers[AFL_KERNEL_REG_COUNT];
    afl_ioctl_u32_t irq_pending;
    struct afl_kvdev_command command_queue[AFL_KERNEL_QUEUE_DEPTH];
    afl_ioctl_u32_t cq_head;
    afl_ioctl_u32_t cq_tail;
    struct afl_kvdev_completion completion_queue[AFL_KERNEL_QUEUE_DEPTH];
    afl_ioctl_u32_t comp_head;
    afl_ioctl_u32_t comp_tail;
    struct afl_ioctl_telemetry telemetry;
    afl_ioctl_u32_t irq_vector_pending;
    afl_ioctl_u32_t irq_vector_mask;
    afl_ioctl_u32_t irq_vector_enable;
    struct afl_ioctl_trace_entry trace_entries[AFL_IOCTL_TRACE_ENTRY_COUNT];
    afl_ioctl_u32_t trace_head;
    afl_ioctl_u32_t trace_count;
    afl_ioctl_u32_t trace_dropped_count;
    u8 scratch[AFL_KERNEL_SCRATCH_SLOT_COUNT][AFL_KERNEL_SCRATCH_SLOT_BYTES];
    afl_ioctl_u64_t cycle_counter;
};

void afl_kvdev_init(struct afl_kernel_vdev *vdev);

afl_ioctl_u32_t afl_kvdev_mmio_read32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg);
void afl_kvdev_mmio_write32(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t reg, afl_ioctl_u32_t value);
int afl_kvdev_submit_command(struct afl_kernel_vdev *vdev, const struct afl_kvdev_command *command);
int afl_kvdev_wait_irq(struct afl_kernel_vdev *vdev, afl_ioctl_u32_t irq_bits);
int afl_kvdev_pop_completion(struct afl_kernel_vdev *vdev, struct afl_kvdev_completion *completion);
void afl_kvdev_get_trace(struct afl_kernel_vdev *vdev, struct afl_ioctl_trace *trace);

wait_queue_head_t *afl_kvdev_irq_wait_queue(struct afl_kernel_vdev *vdev);
__poll_t afl_kvdev_poll_mask(struct afl_kernel_vdev *vdev);

#endif
