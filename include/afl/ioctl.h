#ifndef AFL_IOCTL_H
#define AFL_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#include "afl/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AFL_IOCTL_MAGIC 'a'

struct afl_ioctl_version {
    afl_ioctl_u32_t device_id;
    afl_ioctl_u32_t vendor_id;
    afl_ioctl_u32_t abi_version;
    afl_ioctl_u32_t fw_version;
};

struct afl_ioctl_status {
    afl_ioctl_u32_t device_id;
    afl_ioctl_u32_t vendor_id;
    afl_ioctl_u32_t abi_version;
    afl_ioctl_u32_t fw_version;
    afl_ioctl_u32_t device_status;
    afl_ioctl_u32_t error_code;
    afl_ioctl_u32_t command_count;
    afl_ioctl_u32_t reset_count;
    afl_ioctl_u32_t doorbell;
    afl_ioctl_u32_t irq_status;
    afl_ioctl_u32_t command_queue_head;
    afl_ioctl_u32_t command_queue_tail;
    afl_ioctl_u32_t completion_queue_head;
    afl_ioctl_u32_t completion_queue_tail;
    afl_ioctl_u32_t trace_head;
    afl_ioctl_u32_t trace_count;
    afl_ioctl_u32_t irq_vector_status;
    afl_ioctl_u32_t irq_vector_mask;
    afl_ioctl_u32_t irq_vector_enable;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_telemetry {
    afl_ioctl_u64_t boot_count;
    afl_ioctl_u64_t reset_count;
    afl_ioctl_u64_t command_count;
    afl_ioctl_u64_t completion_count;
    afl_ioctl_u64_t failed_command_count;
    afl_ioctl_u64_t timeout_count;
    afl_ioctl_u64_t recovery_count;
    afl_ioctl_u64_t last_command_cycles;
    afl_ioctl_u32_t current_fw_state;
    afl_ioctl_u32_t last_error_code;
};

struct afl_ioctl_memcopy {
    afl_ioctl_u64_t src_user_ptr;
    afl_ioctl_u64_t dst_user_ptr;
    afl_ioctl_u32_t length;
    afl_ioctl_u32_t flags;
};

struct afl_ioctl_vector_add {
    afl_ioctl_u64_t a_user_ptr;
    afl_ioctl_u64_t b_user_ptr;
    afl_ioctl_u64_t out_user_ptr;
    afl_ioctl_u32_t element_count;
    afl_ioctl_u32_t dtype;
};

struct afl_ioctl_matrix_mul {
    afl_ioctl_u64_t a_user_ptr;
    afl_ioctl_u64_t b_user_ptr;
    afl_ioctl_u64_t c_user_ptr;
    afl_ioctl_u32_t m;
    afl_ioctl_u32_t n;
    afl_ioctl_u32_t k;
    afl_ioctl_u32_t dtype;
    afl_ioctl_u32_t flags;
};

struct afl_ioctl_alloc_buffer {
    afl_ioctl_u32_t size;
    afl_ioctl_u32_t flags;
    afl_ioctl_u32_t handle;
    afl_ioctl_u32_t mmap_offset;
    afl_ioctl_u32_t allocated_size;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_free_buffer {
    afl_ioctl_u32_t handle;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_vector_add_buffer {
    afl_ioctl_u32_t a_handle;
    afl_ioctl_u32_t a_offset;
    afl_ioctl_u32_t b_handle;
    afl_ioctl_u32_t b_offset;
    afl_ioctl_u32_t out_handle;
    afl_ioctl_u32_t out_offset;
    afl_ioctl_u32_t element_count;
    afl_ioctl_u32_t dtype;
};

struct afl_ioctl_matrix_mul_buffer {
    afl_ioctl_u32_t a_handle;
    afl_ioctl_u32_t a_offset;
    afl_ioctl_u32_t b_handle;
    afl_ioctl_u32_t b_offset;
    afl_ioctl_u32_t c_handle;
    afl_ioctl_u32_t c_offset;
    afl_ioctl_u32_t m;
    afl_ioctl_u32_t n;
    afl_ioctl_u32_t k;
    afl_ioctl_u32_t dtype;
    afl_ioctl_u32_t flags;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_fault {
    afl_ioctl_u32_t fault_kind;
    afl_ioctl_u32_t status;
    afl_ioctl_u32_t error_code;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_completion {
    afl_ioctl_u32_t status;
    afl_ioctl_u32_t error_code;
    afl_ioctl_u32_t result;
    afl_ioctl_u32_t reserved;
};

struct afl_ioctl_trace_entry {
    afl_ioctl_u64_t cycle;
    afl_ioctl_u32_t event;
    afl_ioctl_u32_t opcode;
    afl_ioctl_u32_t status;
    afl_ioctl_u32_t error_code;
};

struct afl_ioctl_trace {
    afl_ioctl_u32_t count;
    afl_ioctl_u32_t dropped_count;
    afl_ioctl_u32_t reserved[2];
    struct afl_ioctl_trace_entry entries[AFL_IOCTL_TRACE_ENTRY_COUNT];
};

#define AFL_IOCTL_GET_VERSION _IOR(AFL_IOCTL_MAGIC, 0x01, struct afl_ioctl_version)
#define AFL_IOCTL_GET_TELEMETRY _IOR(AFL_IOCTL_MAGIC, 0x02, struct afl_ioctl_telemetry)
#define AFL_IOCTL_RESET _IO(AFL_IOCTL_MAGIC, 0x03)
#define AFL_IOCTL_GET_STATUS _IOR(AFL_IOCTL_MAGIC, 0x04, struct afl_ioctl_status)
#define AFL_IOCTL_RUN_SELFTEST _IO(AFL_IOCTL_MAGIC, 0x05)
#define AFL_IOCTL_MEMCOPY _IOW(AFL_IOCTL_MAGIC, 0x06, struct afl_ioctl_memcopy)
#define AFL_IOCTL_VECTOR_ADD _IOW(AFL_IOCTL_MAGIC, 0x07, struct afl_ioctl_vector_add)
#define AFL_IOCTL_MATRIX_MUL _IOW(AFL_IOCTL_MAGIC, 0x08, struct afl_ioctl_matrix_mul)
#define AFL_IOCTL_TRIGGER_FAULT _IOWR(AFL_IOCTL_MAGIC, 0x09, struct afl_ioctl_fault)
#define AFL_IOCTL_VECTOR_ADD_ASYNC _IOW(AFL_IOCTL_MAGIC, 0x0a, struct afl_ioctl_vector_add)
#define AFL_IOCTL_GET_COMPLETION _IOR(AFL_IOCTL_MAGIC, 0x0b, struct afl_ioctl_completion)
#define AFL_IOCTL_GET_TRACE _IOR(AFL_IOCTL_MAGIC, 0x0c, struct afl_ioctl_trace)
#define AFL_IOCTL_TRIGGER_TIMEOUT _IOR(AFL_IOCTL_MAGIC, 0x0d, struct afl_ioctl_completion)
#define AFL_IOCTL_ALLOC_BUFFER _IOWR(AFL_IOCTL_MAGIC, 0x10, struct afl_ioctl_alloc_buffer)
#define AFL_IOCTL_FREE_BUFFER _IOW(AFL_IOCTL_MAGIC, 0x11, struct afl_ioctl_free_buffer)
#define AFL_IOCTL_VECTOR_ADD_BUFFER _IOW(AFL_IOCTL_MAGIC, 0x12, struct afl_ioctl_vector_add_buffer)
#define AFL_IOCTL_MATRIX_MUL_BUFFER _IOW(AFL_IOCTL_MAGIC, 0x13, struct afl_ioctl_matrix_mul_buffer)

#ifdef __cplusplus
}
#endif

#endif
