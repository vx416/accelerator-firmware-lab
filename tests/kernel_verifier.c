#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "afl/ioctl.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static int g_failures;

static void pass(const char *name)
{
    printf("PASS: %s\n", name);
}

static void fail(const char *name, const char *detail)
{
    printf("FAIL: %s: %s\n", name, detail);
    g_failures++;
}

static int expect_ioctl_errno(int fd, unsigned long request, void *arg, int expected_errno, const char *name)
{
    errno = 0;
    if (ioctl(fd, request, arg) == 0) {
        fail(name, "ioctl unexpectedly succeeded");
        return -1;
    }
    if (errno != expected_errno) {
        char detail[128];

        snprintf(detail, sizeof(detail), "expected errno %d, got %d (%s)", expected_errno, errno, strerror(errno));
        fail(name, detail);
        return -1;
    }

    pass(name);
    return 0;
}

static int reset_device(int fd)
{
    if (ioctl(fd, AFL_IOCTL_RESET) != 0) {
        printf("FAIL: reset fixture: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int test_invalid_pointer(int fd)
{
    int32_t b[] = {1, 2, 3, 4};
    int32_t out[] = {0, 0, 0, 0};
    struct afl_ioctl_vector_add req;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = 1;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.out_user_ptr = (uint64_t)(uintptr_t)out;
    req.element_count = ARRAY_SIZE(b);
    req.dtype = AFL_IOCTL_DTYPE_I32;

    return expect_ioctl_errno(fd, AFL_IOCTL_VECTOR_ADD, &req, EFAULT, "invalid pointer is rejected");
}

static int test_oversized_dma(int fd)
{
    uint8_t byte = 0;
    struct afl_ioctl_memcopy req;

    memset(&req, 0, sizeof(req));
    req.src_user_ptr = (uint64_t)(uintptr_t)&byte;
    req.dst_user_ptr = (uint64_t)(uintptr_t)&byte;
    req.length = AFL_IOCTL_MAX_TRANSFER_BYTES + 1u;

    return expect_ioctl_errno(fd, AFL_IOCTL_MEMCOPY, &req, EINVAL, "oversized DMA request is rejected");
}

static int test_async_completion_and_backpressure(int fd)
{
    int32_t a[] = {2, 4, 6};
    int32_t b[] = {20, 40, 60};
    int32_t out[] = {0, 0, 0};
    struct afl_ioctl_vector_add req;
    struct afl_ioctl_completion completion;
    struct pollfd pfd;
    int ret;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = (uint64_t)(uintptr_t)a;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.out_user_ptr = (uint64_t)(uintptr_t)out;
    req.element_count = ARRAY_SIZE(a);
    req.dtype = AFL_IOCTL_DTYPE_I32;

    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD_ASYNC, &req) != 0) {
        fail("async vector-add submit", strerror(errno));
        return -1;
    }
    pass("async vector-add submit");

    expect_ioctl_errno(fd, AFL_IOCTL_VECTOR_ADD_ASYNC, &req, EBUSY, "async backpressure rejects second outstanding command");

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, 3000);
    if (ret <= 0) {
        fail("poll reports async completion", ret == 0 ? "timed out" : strerror(errno));
        return -1;
    }
    if ((pfd.revents & POLLIN) == 0) {
        fail("poll reports async completion", "POLLIN was not set");
        return -1;
    }
    pass("poll reports async completion");

    memset(&completion, 0, sizeof(completion));
    if (ioctl(fd, AFL_IOCTL_GET_COMPLETION, &completion) != 0) {
        fail("async completion ioctl", strerror(errno));
        return -1;
    }
    if (completion.status != AFL_IOCTL_CMD_STATUS_SUCCESS || completion.error_code != AFL_IOCTL_ERR_NONE || completion.result != 0) {
        fail("async completion status", "completion was not successful");
        return -1;
    }
    if (out[0] != 22 || out[1] != 44 || out[2] != 66) {
        fail("async completion result", "vector output mismatch");
        return -1;
    }
    pass("async completion result");

    expect_ioctl_errno(fd, AFL_IOCTL_GET_COMPLETION, &completion, ENOENT, "empty completion queue is reported");
    return 0;
}

static int test_timeout_recovery(int fd)
{
    struct afl_ioctl_telemetry before;
    struct afl_ioctl_telemetry after;
    struct afl_ioctl_completion completion;

    memset(&before, 0, sizeof(before));
    if (ioctl(fd, AFL_IOCTL_GET_TELEMETRY, &before) != 0) {
        fail("timeout fixture telemetry before", strerror(errno));
        return -1;
    }

    memset(&completion, 0, sizeof(completion));
    if (ioctl(fd, AFL_IOCTL_TRIGGER_TIMEOUT, &completion) != 0) {
        fail("trigger timeout", strerror(errno));
        return -1;
    }
    if (completion.status != AFL_IOCTL_CMD_STATUS_TIMEOUT || completion.error_code != AFL_IOCTL_ERR_TIMEOUT) {
        fail("timeout completion status", "expected timeout completion");
        return -1;
    }
    pass("timeout completion status");

    memset(&after, 0, sizeof(after));
    if (ioctl(fd, AFL_IOCTL_GET_TELEMETRY, &after) != 0) {
        fail("timeout fixture telemetry after", strerror(errno));
        return -1;
    }
    if (after.current_fw_state != AFL_IOCTL_FW_STATE_READY) {
        fail("timeout recovery returns READY", "firmware did not return to READY");
        return -1;
    }
    if (after.timeout_count <= before.timeout_count || after.recovery_count <= before.recovery_count) {
        fail("timeout telemetry counters", "timeout/recovery counters did not increase");
        return -1;
    }
    pass("timeout recovery returns READY");
    return 0;
}

static int test_reset_after_fault(int fd)
{
    struct afl_ioctl_fault fault;
    struct afl_ioctl_status status;

    memset(&fault, 0, sizeof(fault));
    fault.fault_kind = AFL_IOCTL_FAULT_ECC;
    if (ioctl(fd, AFL_IOCTL_TRIGGER_FAULT, &fault) != 0) {
        fail("trigger ECC fault", strerror(errno));
        return -1;
    }
    if (fault.error_code != AFL_IOCTL_ERR_ECC) {
        fail("trigger ECC fault", "unexpected error code");
        return -1;
    }
    pass("trigger ECC fault");

    if (reset_device(fd) != 0)
        return -1;

    memset(&status, 0, sizeof(status));
    if (ioctl(fd, AFL_IOCTL_GET_STATUS, &status) != 0) {
        fail("status after reset", strerror(errno));
        return -1;
    }
    if ((status.device_status & AFL_IOCTL_STATUS_READY) == 0 || status.error_code != AFL_IOCTL_ERR_NONE) {
        fail("reset after fault restores READY", "device status/error did not reset");
        return -1;
    }
    pass("reset after fault restores READY");
    return 0;
}

static int test_interrupt_vectors(int fd)
{
    struct afl_ioctl_fault fault;
    struct afl_ioctl_status status;
    struct afl_ioctl_telemetry telemetry;
    uint32_t error_vector_bit = 1u << AFL_IOCTL_IRQ_VECTOR_ERROR;
    uint32_t admin_vector_bit = 1u << AFL_IOCTL_IRQ_VECTOR_ADMIN;
    uint32_t telemetry_vector_bit = 1u << AFL_IOCTL_IRQ_VECTOR_TELEMETRY;

    if (reset_device(fd) != 0)
        return -1;

    memset(&fault, 0, sizeof(fault));
    fault.fault_kind = AFL_IOCTL_FAULT_ECC;
    if (ioctl(fd, AFL_IOCTL_TRIGGER_FAULT, &fault) != 0) {
        fail("interrupt vector fixture fault", strerror(errno));
        return -1;
    }

    memset(&status, 0, sizeof(status));
    if (ioctl(fd, AFL_IOCTL_GET_STATUS, &status) != 0) {
        fail("interrupt vector status after fault", strerror(errno));
        return -1;
    }
    if ((status.irq_vector_status & error_vector_bit) == 0) {
        fail("error interrupt vector is raised", "ERROR vector bit was not pending");
        return -1;
    }
    pass("error interrupt vector is raised");

    if (reset_device(fd) != 0)
        return -1;

    memset(&status, 0, sizeof(status));
    if (ioctl(fd, AFL_IOCTL_GET_STATUS, &status) != 0) {
        fail("interrupt vector status after reset", strerror(errno));
        return -1;
    }
    if ((status.irq_vector_status & admin_vector_bit) == 0) {
        fail("admin interrupt vector is raised", "ADMIN vector bit was not pending");
        return -1;
    }
    pass("admin interrupt vector is raised");

    memset(&telemetry, 0, sizeof(telemetry));
    if (ioctl(fd, AFL_IOCTL_GET_TELEMETRY, &telemetry) != 0) {
        fail("interrupt vector telemetry command", strerror(errno));
        return -1;
    }

    memset(&status, 0, sizeof(status));
    if (ioctl(fd, AFL_IOCTL_GET_STATUS, &status) != 0) {
        fail("interrupt vector status after telemetry", strerror(errno));
        return -1;
    }
    if ((status.irq_vector_status & telemetry_vector_bit) == 0) {
        fail("telemetry interrupt vector is raised", "TELEMETRY vector bit was not pending");
        return -1;
    }
    pass("telemetry interrupt vector is raised");
    return 0;
}

static int find_event_after(const struct afl_ioctl_trace *trace, uint32_t start, uint32_t event, uint32_t opcode)
{
    uint32_t i;

    for (i = start; i < trace->count; ++i) {
        if (trace->entries[i].event != event)
            continue;
        if (opcode != UINT32_MAX && trace->entries[i].opcode != opcode)
            continue;
        return (int)i;
    }

    return -1;
}

static int test_trace_event_order(int fd)
{
    int32_t a[] = {1, 2};
    int32_t b[] = {10, 20};
    int32_t out[] = {0, 0};
    struct afl_ioctl_vector_add req;
    struct afl_ioctl_trace trace;
    int queued;
    int doorbell;
    int start;
    int complete;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = (uint64_t)(uintptr_t)a;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.out_user_ptr = (uint64_t)(uintptr_t)out;
    req.element_count = ARRAY_SIZE(a);
    req.dtype = AFL_IOCTL_DTYPE_I32;

    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD, &req) != 0) {
        fail("trace fixture vector-add", strerror(errno));
        return -1;
    }

    memset(&trace, 0, sizeof(trace));
    if (ioctl(fd, AFL_IOCTL_GET_TRACE, &trace) != 0) {
        fail("get trace", strerror(errno));
        return -1;
    }

    queued = find_event_after(&trace, 0, AFL_IOCTL_TRACE_COMMAND_QUEUED, 7);
    while (queued >= 0) {
        doorbell = find_event_after(&trace, (uint32_t)queued + 1u, AFL_IOCTL_TRACE_DOORBELL, UINT32_MAX);
        start = doorbell >= 0 ? find_event_after(&trace, (uint32_t)doorbell + 1u, AFL_IOCTL_TRACE_COMMAND_START, 7) : -1;
        complete = start >= 0 ? find_event_after(&trace, (uint32_t)start + 1u, AFL_IOCTL_TRACE_COMMAND_COMPLETE, 7) : -1;
        if (doorbell >= 0 && start >= 0 && complete >= 0) {
            pass("trace records vector-add queue doorbell start complete order");
            return 0;
        }
        queued = find_event_after(&trace, (uint32_t)queued + 1u, AFL_IOCTL_TRACE_COMMAND_QUEUED, 7);
    }

    fail("trace records vector-add queue doorbell start complete order", "ordered event sequence not found");
    return -1;
}

int main(void)
{
    int fd = open("/dev/afl0", O_RDWR);

    if (fd < 0) {
        printf("FAIL: open /dev/afl0: %s\n", strerror(errno));
        return 1;
    }

    if (reset_device(fd) == 0)
        pass("reset fixture");

    test_invalid_pointer(fd);
    test_oversized_dma(fd);
    test_async_completion_and_backpressure(fd);
    test_timeout_recovery(fd);
    test_reset_after_fault(fd);
    test_interrupt_vectors(fd);
    test_trace_event_order(fd);

    close(fd);

    if (g_failures != 0) {
        printf("kernel verifier: FAIL (%d failures)\n", g_failures);
        return 1;
    }

    printf("kernel verifier: PASS\n");
    return 0;
}
