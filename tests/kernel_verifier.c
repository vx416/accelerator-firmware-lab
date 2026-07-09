#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
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

static int alloc_buffer(int fd, uint32_t size, struct afl_ioctl_alloc_buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->size = size;
    return ioctl(fd, AFL_IOCTL_ALLOC_BUFFER, buffer);
}

static int free_buffer(int fd, uint32_t handle)
{
    struct afl_ioctl_free_buffer req;

    memset(&req, 0, sizeof(req));
    req.handle = handle;
    return ioctl(fd, AFL_IOCTL_FREE_BUFFER, &req);
}

static int test_buffer_vector_add(int fd)
{
    struct afl_ioctl_alloc_buffer a_buf;
    struct afl_ioctl_alloc_buffer b_buf;
    struct afl_ioctl_alloc_buffer out_buf;
    struct afl_ioctl_vector_add_buffer req;
    int32_t *a_map;
    int32_t *b_map;
    int32_t *out_map;
    uint32_t bytes = 3u * sizeof(int32_t);

    if (alloc_buffer(fd, bytes, &a_buf) != 0 ||
        alloc_buffer(fd, bytes, &b_buf) != 0 ||
        alloc_buffer(fd, bytes, &out_buf) != 0) {
        fail("buffer allocation for vector-add", strerror(errno));
        return -1;
    }

    a_map = mmap(NULL, a_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a_buf.mmap_offset);
    b_map = mmap(NULL, b_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b_buf.mmap_offset);
    out_map = mmap(NULL, out_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, out_buf.mmap_offset);
    if (a_map == MAP_FAILED || b_map == MAP_FAILED || out_map == MAP_FAILED) {
        fail("buffer mmap for vector-add", strerror(errno));
        return -1;
    }

    a_map[0] = 4;
    a_map[1] = 8;
    a_map[2] = 12;
    b_map[0] = 40;
    b_map[1] = 80;
    b_map[2] = 120;
    memset(out_map, 0, bytes);

    memset(&req, 0, sizeof(req));
    req.a_handle = a_buf.handle;
    req.b_handle = b_buf.handle;
    req.out_handle = out_buf.handle;
    req.element_count = 3;
    req.dtype = AFL_IOCTL_DTYPE_I32;
    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD_BUFFER, &req) != 0) {
        fail("buffer vector-add ioctl", strerror(errno));
        return -1;
    }
    if (out_map[0] != 44 || out_map[1] != 88 || out_map[2] != 132) {
        fail("buffer vector-add result", "vector output mismatch");
        return -1;
    }

    munmap(out_map, out_buf.allocated_size);
    munmap(b_map, b_buf.allocated_size);
    munmap(a_map, a_buf.allocated_size);
    free_buffer(fd, out_buf.handle);
    free_buffer(fd, b_buf.handle);
    free_buffer(fd, a_buf.handle);
    pass("buffer vector-add result");
    return 0;
}

static int test_buffer_matrix_mul(int fd)
{
    struct afl_ioctl_alloc_buffer a_buf;
    struct afl_ioctl_alloc_buffer b_buf;
    struct afl_ioctl_alloc_buffer c_buf;
    struct afl_ioctl_matrix_mul_buffer req;
    int32_t *a_map;
    int32_t *b_map;
    int32_t *c_map;
    int32_t expected[] = {58, 64, 139, 154};

    if (alloc_buffer(fd, 6u * sizeof(int32_t), &a_buf) != 0 ||
        alloc_buffer(fd, 6u * sizeof(int32_t), &b_buf) != 0 ||
        alloc_buffer(fd, sizeof(expected), &c_buf) != 0) {
        fail("buffer allocation for matrix-mul", strerror(errno));
        return -1;
    }

    a_map = mmap(NULL, a_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a_buf.mmap_offset);
    b_map = mmap(NULL, b_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b_buf.mmap_offset);
    c_map = mmap(NULL, c_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, c_buf.mmap_offset);
    if (a_map == MAP_FAILED || b_map == MAP_FAILED || c_map == MAP_FAILED) {
        fail("buffer mmap for matrix-mul", strerror(errno));
        return -1;
    }

    memcpy(a_map, (int32_t[]){1, 2, 3, 4, 5, 6}, 6u * sizeof(int32_t));
    memcpy(b_map, (int32_t[]){7, 8, 9, 10, 11, 12}, 6u * sizeof(int32_t));
    memset(c_map, 0, sizeof(expected));

    memset(&req, 0, sizeof(req));
    req.a_handle = a_buf.handle;
    req.b_handle = b_buf.handle;
    req.c_handle = c_buf.handle;
    req.m = 2;
    req.n = 2;
    req.k = 3;
    req.dtype = AFL_IOCTL_DTYPE_I32;
    if (ioctl(fd, AFL_IOCTL_MATRIX_MUL_BUFFER, &req) != 0) {
        fail("buffer matrix-mul ioctl", strerror(errno));
        return -1;
    }
    if (memcmp(c_map, expected, sizeof(expected)) != 0) {
        fail("buffer matrix-mul result", "matrix output mismatch");
        return -1;
    }

    munmap(c_map, c_buf.allocated_size);
    munmap(b_map, b_buf.allocated_size);
    munmap(a_map, a_buf.allocated_size);
    free_buffer(fd, c_buf.handle);
    free_buffer(fd, b_buf.handle);
    free_buffer(fd, a_buf.handle);
    pass("buffer matrix-mul result");
    return 0;
}

static int test_buffer_allocator_limits(int fd)
{
    struct afl_ioctl_alloc_buffer bufs[5];
    struct afl_ioctl_alloc_buffer replacement;
    struct afl_ioctl_vector_add_buffer invalid;
    uint32_t i;

    memset(bufs, 0, sizeof(bufs));
    for (i = 0; i < 4; ++i) {
        if (alloc_buffer(fd, AFL_IOCTL_BUFFER_MAX_BYTES, &bufs[i]) != 0) {
            fail("buffer quota fixture allocations", strerror(errno));
            return -1;
        }
    }
    if (alloc_buffer(fd, AFL_IOCTL_BUFFER_MAX_BYTES, &bufs[4]) == 0) {
        fail("per-fd buffer quota is enforced", "fifth 1MB allocation unexpectedly succeeded");
        return -1;
    }
    if (errno != EDQUOT) {
        fail("per-fd buffer quota is enforced", strerror(errno));
        return -1;
    }
    pass("per-fd buffer quota is enforced");

    if (free_buffer(fd, bufs[1].handle) != 0) {
        fail("buffer free releases quota fixture", strerror(errno));
        return -1;
    }
    if (alloc_buffer(fd, AFL_IOCTL_BUFFER_MAX_BYTES, &replacement) != 0) {
        fail("buffer free releases quota", strerror(errno));
        return -1;
    }
    pass("buffer free releases quota");

    memset(&invalid, 0, sizeof(invalid));
    invalid.a_handle = 0x00ffffffu;
    invalid.b_handle = replacement.handle;
    invalid.out_handle = bufs[0].handle;
    invalid.element_count = 1;
    invalid.dtype = AFL_IOCTL_DTYPE_I32;
    expect_ioctl_errno(fd, AFL_IOCTL_VECTOR_ADD_BUFFER, &invalid, ENOENT, "invalid buffer handle is rejected");

    free_buffer(fd, replacement.handle);
    free_buffer(fd, bufs[3].handle);
    free_buffer(fd, bufs[2].handle);
    free_buffer(fd, bufs[0].handle);
    return 0;
}

static int test_buffer_allocations_do_not_overlap_across_fds(void)
{
    int fd_a;
    int fd_b;
    struct afl_ioctl_alloc_buffer a;
    struct afl_ioctl_alloc_buffer b;

    fd_a = open("/dev/afl0", O_RDWR);
    fd_b = open("/dev/afl0", O_RDWR);
    if (fd_a < 0 || fd_b < 0) {
        fail("buffer overlap fixture opens", strerror(errno));
        if (fd_a >= 0)
            close(fd_a);
        if (fd_b >= 0)
            close(fd_b);
        return -1;
    }

    if (alloc_buffer(fd_a, AFL_IOCTL_BUFFER_MAX_BYTES, &a) != 0 ||
        alloc_buffer(fd_b, AFL_IOCTL_BUFFER_MAX_BYTES, &b) != 0) {
        fail("buffer overlap fixture allocations", strerror(errno));
        close(fd_b);
        close(fd_a);
        return -1;
    }

    if (a.mmap_offset == b.mmap_offset) {
        fail("buffer allocations across fds do not overlap", "two live buffers received the same mmap offset");
        close(fd_b);
        close(fd_a);
        return -1;
    }

    close(fd_b);
    close(fd_a);
    pass("buffer allocations across fds do not overlap");
    return 0;
}

static int test_buffer_release_reclaims_unfreed_buffers(void)
{
    int fds[4];
    int fd_replacement;
    struct afl_ioctl_alloc_buffer buffers[4][4];
    struct afl_ioctl_alloc_buffer replacement[4];
    int i;
    int j;

    memset(fds, -1, sizeof(fds));
    memset(buffers, 0, sizeof(buffers));
    memset(replacement, 0, sizeof(replacement));

    for (i = 0; i < 4; ++i) {
        fds[i] = open("/dev/afl0", O_RDWR);
        if (fds[i] < 0) {
            fail("release reclaim fixture opens", strerror(errno));
            while (i >= 0) {
                if (fds[i] >= 0)
                    close(fds[i]);
                --i;
            }
            return -1;
        }
        for (j = 0; j < 4; ++j) {
            if (alloc_buffer(fds[i], AFL_IOCTL_BUFFER_MAX_BYTES, &buffers[i][j]) != 0) {
                fail("release reclaim fixture allocations", strerror(errno));
                while (i >= 0) {
                    if (fds[i] >= 0)
                        close(fds[i]);
                    --i;
                }
                return -1;
            }
        }
    }

    close(fds[0]);
    fds[0] = -1;

    fd_replacement = open("/dev/afl0", O_RDWR);
    if (fd_replacement < 0) {
        fail("release reclaim replacement open", strerror(errno));
        goto out_close;
    }
    for (j = 0; j < 4; ++j) {
        if (alloc_buffer(fd_replacement, AFL_IOCTL_BUFFER_MAX_BYTES, &replacement[j]) != 0) {
            fail("release reclaims unfreed buffers", strerror(errno));
            close(fd_replacement);
            goto out_close;
        }
    }

    close(fd_replacement);
    pass("release reclaims unfreed buffers");

out_close:
    for (i = 0; i < 4; ++i) {
        if (fds[i] >= 0)
            close(fds[i]);
    }
    return 0;
}

static int run_child_buffer_vector_add(void)
{
    int fd;
    struct afl_ioctl_alloc_buffer a_buf;
    struct afl_ioctl_alloc_buffer b_buf;
    struct afl_ioctl_alloc_buffer out_buf;
    struct afl_ioctl_vector_add_buffer req;
    int32_t *a_map;
    int32_t *b_map;
    int32_t *out_map;
    uint32_t bytes = 2u * sizeof(int32_t);
    int ok;

    fd = open("/dev/afl0", O_RDWR);
    if (fd < 0)
        return 10;
    if (alloc_buffer(fd, bytes, &a_buf) != 0 ||
        alloc_buffer(fd, bytes, &b_buf) != 0 ||
        alloc_buffer(fd, bytes, &out_buf) != 0) {
        close(fd);
        return 11;
    }

    a_map = mmap(NULL, a_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, a_buf.mmap_offset);
    b_map = mmap(NULL, b_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b_buf.mmap_offset);
    out_map = mmap(NULL, out_buf.allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, out_buf.mmap_offset);
    if (a_map == MAP_FAILED || b_map == MAP_FAILED || out_map == MAP_FAILED) {
        close(fd);
        return 12;
    }

    a_map[0] = 7;
    a_map[1] = 8;
    b_map[0] = 70;
    b_map[1] = 80;
    memset(out_map, 0, bytes);

    memset(&req, 0, sizeof(req));
    req.a_handle = a_buf.handle;
    req.b_handle = b_buf.handle;
    req.out_handle = out_buf.handle;
    req.element_count = 2;
    req.dtype = AFL_IOCTL_DTYPE_I32;
    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD_BUFFER, &req) != 0) {
        close(fd);
        return 13;
    }

    ok = out_map[0] == 77 && out_map[1] == 88;
    close(fd);
    return ok ? 0 : 14;
}

static int test_concurrent_buffer_commands(void)
{
    pid_t child_a;
    pid_t child_b;
    int status_a;
    int status_b;

    child_a = fork();
    if (child_a == 0)
        _exit(run_child_buffer_vector_add());
    if (child_a < 0) {
        fail("concurrent buffer command fork A", strerror(errno));
        return -1;
    }

    child_b = fork();
    if (child_b == 0)
        _exit(run_child_buffer_vector_add());
    if (child_b < 0) {
        fail("concurrent buffer command fork B", strerror(errno));
        waitpid(child_a, &status_a, 0);
        return -1;
    }

    if (waitpid(child_a, &status_a, 0) < 0 || waitpid(child_b, &status_b, 0) < 0) {
        fail("concurrent buffer command wait", strerror(errno));
        return -1;
    }
    if (!WIFEXITED(status_a) || WEXITSTATUS(status_a) != 0 ||
        !WIFEXITED(status_b) || WEXITSTATUS(status_b) != 0) {
        fail("concurrent buffer commands use isolated buffers", "one child returned a bad result");
        return -1;
    }

    pass("concurrent buffer commands use isolated buffers");
    return 0;
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
    test_buffer_vector_add(fd);
    test_buffer_matrix_mul(fd);
    test_buffer_allocator_limits(fd);
    test_buffer_allocations_do_not_overlap_across_fds();
    test_buffer_release_reclaims_unfreed_buffers();
    test_concurrent_buffer_commands();
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
