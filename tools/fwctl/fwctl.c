#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "afl/ioctl.h"

static void print_usage(const char *argv0)
{
    fprintf(stderr, "usage: %s <command>\n", argv0);
    fprintf(stderr, "\ncommands:\n");
    fprintf(stderr, "  version\n");
    fprintf(stderr, "  selftest\n");
    fprintf(stderr, "  run-vector-add [a0,a1,... b0,b1,...]\n");
    fprintf(stderr, "  run-vector-add-buffer [a0,a1,... b0,b1,...]\n");
    fprintf(stderr, "  run-vector-add-async [a0,a1,... b0,b1,...]\n");
    fprintf(stderr, "  run-matrix-mul [m n k a0,a1,... b0,b1,...]\n");
    fprintf(stderr, "  run-matrix-mul-buffer [m n k a0,a1,... b0,b1,...]\n");
    fprintf(stderr, "  memcopy\n");
    fprintf(stderr, "  telemetry\n");
    fprintf(stderr, "  trigger-fault\n");
    fprintf(stderr, "  trigger-timeout\n");
    fprintf(stderr, "  reset\n");
    fprintf(stderr, "  dump-status\n");
    fprintf(stderr, "  dump-trace\n");
}

static int kernel_open(void)
{
    int fd = open("/dev/afl0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "failed to open /dev/afl0: %s\n", strerror(errno));
        fprintf(stderr, "hint: build and load the kernel module with `make -C kernel` and `sudo insmod kernel/afl_kernel.ko`\n");
    }
    return fd;
}

static int kernel_alloc_buffer(int fd, uint32_t size, struct afl_ioctl_alloc_buffer *buffer)
{
    memset(buffer, 0, sizeof(*buffer));
    buffer->size = size;
    if (ioctl(fd, AFL_IOCTL_ALLOC_BUFFER, buffer) != 0) {
        fprintf(stderr, "AFL_IOCTL_ALLOC_BUFFER failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int kernel_free_buffer(int fd, uint32_t handle)
{
    struct afl_ioctl_free_buffer req;

    if (handle == 0)
        return 0;

    memset(&req, 0, sizeof(req));
    req.handle = handle;
    if (ioctl(fd, AFL_IOCTL_FREE_BUFFER, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_FREE_BUFFER failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static void *kernel_mmap_buffer(int fd, const struct afl_ioctl_alloc_buffer *buffer)
{
    void *ptr = mmap(NULL, buffer->allocated_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer->mmap_offset);

    if (ptr == MAP_FAILED) {
        fprintf(stderr, "mmap(buffer handle=%u) failed: %s\n", buffer->handle, strerror(errno));
        return NULL;
    }
    return ptr;
}

static int kernel_cmd_version(void)
{
    struct afl_ioctl_version version;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&version, 0, sizeof(version));
    if (ioctl(fd, AFL_IOCTL_GET_VERSION, &version) != 0) {
        fprintf(stderr, "AFL_IOCTL_GET_VERSION failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("device_id: 0x%08x\n", version.device_id);
    printf("vendor_id: 0x%08x\n", version.vendor_id);
    printf("abi_version: %u.%u.%u\n",
           (version.abi_version >> 24) & 0xffu,
           (version.abi_version >> 16) & 0xffu,
           version.abi_version & 0xffffu);
    printf("firmware version: %u.%u.%u\n",
           (version.fw_version >> 24) & 0xffu,
           (version.fw_version >> 16) & 0xffu,
           version.fw_version & 0xffffu);

    close(fd);
    return 0;
}

static int kernel_cmd_selftest(void)
{
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    if (ioctl(fd, AFL_IOCTL_RUN_SELFTEST) != 0) {
        fprintf(stderr, "AFL_IOCTL_RUN_SELFTEST failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("selftest: PASS\n");
    close(fd);
    return 0;
}

static int parse_i32_vector(const char *text, int32_t **values_out, uint32_t *count_out)
{
    const char *p = text;
    uint32_t count = 0;
    int32_t *values;
    uint32_t i;

    if (text == NULL || *text == '\0')
        return -EINVAL;

    while (*p != '\0') {
        char *end;
        long value;

        errno = 0;
        value = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || value < INT32_MIN || value > INT32_MAX)
            return -EINVAL;
        count++;

        if (*end == '\0')
            break;
        if (*end != ',' || end[1] == '\0')
            return -EINVAL;
        p = end + 1;
    }

    if (count == 0 || count > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(int32_t))
        return -EINVAL;

    values = calloc(count, sizeof(*values));
    if (values == NULL)
        return -ENOMEM;

    p = text;
    for (i = 0; i < count; ++i) {
        char *end;
        long value;

        errno = 0;
        value = strtol(p, &end, 10);
        if (end == p || errno == ERANGE || value < INT32_MIN || value > INT32_MAX) {
            free(values);
            return -EINVAL;
        }
        values[i] = (int32_t)value;
        p = *end == ',' ? end + 1 : end;
    }

    *values_out = values;
    *count_out = count;
    return 0;
}

static int parse_u32_arg(const char *text, uint32_t *value_out)
{
    char *end;
    unsigned long value;

    if (text == NULL || *text == '\0')
        return -EINVAL;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || errno == ERANGE || value == 0 || value > UINT32_MAX)
        return -EINVAL;

    *value_out = (uint32_t)value;
    return 0;
}

static int checked_matrix_element_count(uint32_t rows, uint32_t cols, uint32_t *count_out)
{
    if (rows == 0 || cols == 0)
        return -EINVAL;
    if (rows > UINT32_MAX / cols)
        return -EINVAL;
    if (rows > AFL_IOCTL_MAX_TRANSFER_BYTES / sizeof(int32_t) / cols)
        return -EINVAL;

    *count_out = rows * cols;
    return 0;
}

static int kernel_run_vector_add(int32_t *a, int32_t *b, int32_t *out, uint32_t element_count)
{
    struct afl_ioctl_vector_add req;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = (uint64_t)(uintptr_t)a;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.out_user_ptr = (uint64_t)(uintptr_t)out;
    req.element_count = element_count;
    req.dtype = AFL_IOCTL_DTYPE_I32;

    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_VECTOR_ADD failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int kernel_run_vector_add_buffer(int32_t *a, int32_t *b, int32_t *out, uint32_t element_count)
{
    struct afl_ioctl_alloc_buffer a_buf;
    struct afl_ioctl_alloc_buffer b_buf;
    struct afl_ioctl_alloc_buffer out_buf;
    struct afl_ioctl_vector_add_buffer req;
    int32_t *a_map = NULL;
    int32_t *b_map = NULL;
    int32_t *out_map = NULL;
    uint32_t bytes = element_count * (uint32_t)sizeof(int32_t);
    int fd = kernel_open();
    int rc = 0;

    if (fd < 0)
        return 1;
    memset(&a_buf, 0, sizeof(a_buf));
    memset(&b_buf, 0, sizeof(b_buf));
    memset(&out_buf, 0, sizeof(out_buf));
    if (kernel_alloc_buffer(fd, bytes, &a_buf) != 0 ||
        kernel_alloc_buffer(fd, bytes, &b_buf) != 0 ||
        kernel_alloc_buffer(fd, bytes, &out_buf) != 0) {
        rc = 1;
        goto out_free;
    }

    a_map = kernel_mmap_buffer(fd, &a_buf);
    b_map = kernel_mmap_buffer(fd, &b_buf);
    out_map = kernel_mmap_buffer(fd, &out_buf);
    if (a_map == NULL || b_map == NULL || out_map == NULL) {
        rc = 1;
        goto out_free;
    }

    memcpy(a_map, a, bytes);
    memcpy(b_map, b, bytes);
    memset(out_map, 0, bytes);

    memset(&req, 0, sizeof(req));
    req.a_handle = a_buf.handle;
    req.b_handle = b_buf.handle;
    req.out_handle = out_buf.handle;
    req.element_count = element_count;
    req.dtype = AFL_IOCTL_DTYPE_I32;
    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD_BUFFER, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_VECTOR_ADD_BUFFER failed: %s\n", strerror(errno));
        rc = 1;
    } else {
        memcpy(out, out_map, bytes);
    }

out_free:
    if (out_map != NULL)
        munmap(out_map, out_buf.allocated_size);
    if (b_map != NULL)
        munmap(b_map, b_buf.allocated_size);
    if (a_map != NULL)
        munmap(a_map, a_buf.allocated_size);
    kernel_free_buffer(fd, out_buf.handle);
    kernel_free_buffer(fd, b_buf.handle);
    kernel_free_buffer(fd, a_buf.handle);
    close(fd);
    return rc;
}

static void print_i32_vector(const char *label, const int32_t *values, uint32_t count)
{
    uint32_t i;

    printf("%s: [", label);
    for (i = 0; i < count; ++i)
        printf("%s%d", i == 0 ? "" : ", ", values[i]);
    printf("]\n");
}

static void print_i32_matrix(const char *label, const int32_t *values, uint32_t rows, uint32_t cols)
{
    uint32_t row;
    uint32_t col;

    printf("%s %ux%u:\n", label, rows, cols);
    for (row = 0; row < rows; ++row) {
        printf("  [");
        for (col = 0; col < cols; ++col)
            printf("%s%d", col == 0 ? "" : ", ", values[row * cols + col]);
        printf("]\n");
    }
}

static int kernel_cmd_vector_add(int arg_count, char **args)
{
    int32_t default_a[] = {1, 2, 3, 4};
    int32_t default_b[] = {10, 20, 30, 40};
    int32_t default_out[] = {0, 0, 0, 0};
    int32_t *a = default_a;
    int32_t *b = default_b;
    int32_t *out = default_out;
    uint32_t a_count = 4;
    uint32_t b_count = 4;
    int rc;

    if (arg_count != 0 && arg_count != 2) {
        fprintf(stderr, "usage: fwctl run-vector-add [a0,a1,... b0,b1,...]\n");
        return 2;
    }

    if (arg_count == 2) {
        rc = parse_i32_vector(args[0], &a, &a_count);
        if (rc != 0) {
            fprintf(stderr, "invalid vector A `%s`\n", args[0]);
            return 2;
        }
        rc = parse_i32_vector(args[1], &b, &b_count);
        if (rc != 0) {
            fprintf(stderr, "invalid vector B `%s`\n", args[1]);
            free(a);
            return 2;
        }
        if (a_count != b_count) {
            fprintf(stderr, "vector lengths must match\n");
            free(b);
            free(a);
            return 2;
        }

        out = calloc(a_count, sizeof(*out));
        if (out == NULL) {
            free(b);
            free(a);
            return 1;
        }
    }

    rc = kernel_run_vector_add(a, b, out, a_count);
    if (rc == 0)
        print_i32_vector("vector-add i32", out, a_count);

    if (arg_count == 2) {
        free(out);
        free(b);
        free(a);
    }
    return rc;
}

static int kernel_cmd_vector_add_buffer(int arg_count, char **args)
{
    int32_t default_a[] = {1, 2, 3, 4};
    int32_t default_b[] = {10, 20, 30, 40};
    int32_t default_out[] = {0, 0, 0, 0};
    int32_t *a = default_a;
    int32_t *b = default_b;
    int32_t *out = default_out;
    uint32_t a_count = 4;
    uint32_t b_count = 4;
    int rc;

    if (arg_count != 0 && arg_count != 2) {
        fprintf(stderr, "usage: fwctl run-vector-add-buffer [a0,a1,... b0,b1,...]\n");
        return 2;
    }

    if (arg_count == 2) {
        rc = parse_i32_vector(args[0], &a, &a_count);
        if (rc != 0)
            return 2;
        rc = parse_i32_vector(args[1], &b, &b_count);
        if (rc != 0) {
            free(a);
            return 2;
        }
        if (a_count != b_count) {
            free(b);
            free(a);
            return 2;
        }
        out = calloc(a_count, sizeof(*out));
        if (out == NULL) {
            free(b);
            free(a);
            return 1;
        }
    }

    rc = kernel_run_vector_add_buffer(a, b, out, a_count);
    if (rc == 0)
        print_i32_vector("vector-add buffer i32", out, a_count);

    if (arg_count == 2) {
        free(out);
        free(b);
        free(a);
    }
    return rc;
}

static int kernel_run_vector_add_async(int32_t *a, int32_t *b, int32_t *out, uint32_t element_count)
{
    struct afl_ioctl_vector_add req;
    struct afl_ioctl_completion completion;
    struct pollfd pfd;
    int fd = kernel_open();
    int ret;

    if (fd < 0)
        return 1;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = (uint64_t)(uintptr_t)a;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.out_user_ptr = (uint64_t)(uintptr_t)out;
    req.element_count = element_count;
    req.dtype = AFL_IOCTL_DTYPE_I32;

    if (ioctl(fd, AFL_IOCTL_VECTOR_ADD_ASYNC, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_VECTOR_ADD_ASYNC failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("submitted vector-add async\n");

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;

    ret = poll(&pfd, 1, 3000);
    if (ret < 0) {
        fprintf(stderr, "poll(/dev/afl0) failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if (ret == 0) {
        fprintf(stderr, "poll(/dev/afl0) timed out\n");
        close(fd);
        return 1;
    }

    printf("poll: completion ready\n");

    memset(&completion, 0, sizeof(completion));
    if (ioctl(fd, AFL_IOCTL_GET_COMPLETION, &completion) != 0) {
        fprintf(stderr, "AFL_IOCTL_GET_COMPLETION failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);

    if (completion.status != AFL_IOCTL_CMD_STATUS_SUCCESS || completion.error_code != AFL_IOCTL_ERR_NONE) {
        fprintf(stderr, "completion failed: status=%u error=%u result=%u\n",
                completion.status,
                completion.error_code,
                completion.result);
        return 1;
    }

    return 0;
}

static int kernel_cmd_vector_add_async(int arg_count, char **args)
{
    int32_t default_a[] = {1, 2, 3, 4};
    int32_t default_b[] = {10, 20, 30, 40};
    int32_t default_out[] = {0, 0, 0, 0};
    int32_t *a = default_a;
    int32_t *b = default_b;
    int32_t *out = default_out;
    uint32_t a_count = 4;
    uint32_t b_count = 4;
    int rc;

    if (arg_count != 0 && arg_count != 2) {
        fprintf(stderr, "usage: fwctl run-vector-add-async [a0,a1,... b0,b1,...]\n");
        return 2;
    }

    if (arg_count == 2) {
        rc = parse_i32_vector(args[0], &a, &a_count);
        if (rc != 0) {
            fprintf(stderr, "invalid vector A `%s`\n", args[0]);
            return 2;
        }
        rc = parse_i32_vector(args[1], &b, &b_count);
        if (rc != 0) {
            fprintf(stderr, "invalid vector B `%s`\n", args[1]);
            free(a);
            return 2;
        }
        if (a_count != b_count) {
            fprintf(stderr, "vector lengths must match\n");
            free(b);
            free(a);
            return 2;
        }

        out = calloc(a_count, sizeof(*out));
        if (out == NULL) {
            free(b);
            free(a);
            return 1;
        }
    }

    rc = kernel_run_vector_add_async(a, b, out, a_count);
    if (rc == 0)
        print_i32_vector("vector-add async i32", out, a_count);

    if (arg_count == 2) {
        free(out);
        free(b);
        free(a);
    }
    return rc;
}

static int kernel_run_matrix_mul(int32_t *a, int32_t *b, int32_t *c, uint32_t m, uint32_t n, uint32_t k)
{
    struct afl_ioctl_matrix_mul req;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&req, 0, sizeof(req));
    req.a_user_ptr = (uint64_t)(uintptr_t)a;
    req.b_user_ptr = (uint64_t)(uintptr_t)b;
    req.c_user_ptr = (uint64_t)(uintptr_t)c;
    req.m = m;
    req.n = n;
    req.k = k;
    req.dtype = AFL_IOCTL_DTYPE_I32;

    if (ioctl(fd, AFL_IOCTL_MATRIX_MUL, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_MATRIX_MUL failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int kernel_run_matrix_mul_buffer(int32_t *a, int32_t *b, int32_t *c, uint32_t m, uint32_t n, uint32_t k)
{
    struct afl_ioctl_alloc_buffer a_buf;
    struct afl_ioctl_alloc_buffer b_buf;
    struct afl_ioctl_alloc_buffer c_buf;
    struct afl_ioctl_matrix_mul_buffer req;
    int32_t *a_map = NULL;
    int32_t *b_map = NULL;
    int32_t *c_map = NULL;
    uint32_t a_bytes = m * k * (uint32_t)sizeof(int32_t);
    uint32_t b_bytes = k * n * (uint32_t)sizeof(int32_t);
    uint32_t c_bytes = m * n * (uint32_t)sizeof(int32_t);
    int fd = kernel_open();
    int rc = 0;

    if (fd < 0)
        return 1;
    memset(&a_buf, 0, sizeof(a_buf));
    memset(&b_buf, 0, sizeof(b_buf));
    memset(&c_buf, 0, sizeof(c_buf));
    if (kernel_alloc_buffer(fd, a_bytes, &a_buf) != 0 ||
        kernel_alloc_buffer(fd, b_bytes, &b_buf) != 0 ||
        kernel_alloc_buffer(fd, c_bytes, &c_buf) != 0) {
        rc = 1;
        goto out_free;
    }

    a_map = kernel_mmap_buffer(fd, &a_buf);
    b_map = kernel_mmap_buffer(fd, &b_buf);
    c_map = kernel_mmap_buffer(fd, &c_buf);
    if (a_map == NULL || b_map == NULL || c_map == NULL) {
        rc = 1;
        goto out_free;
    }

    memcpy(a_map, a, a_bytes);
    memcpy(b_map, b, b_bytes);
    memset(c_map, 0, c_bytes);

    memset(&req, 0, sizeof(req));
    req.a_handle = a_buf.handle;
    req.b_handle = b_buf.handle;
    req.c_handle = c_buf.handle;
    req.m = m;
    req.n = n;
    req.k = k;
    req.dtype = AFL_IOCTL_DTYPE_I32;
    if (ioctl(fd, AFL_IOCTL_MATRIX_MUL_BUFFER, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_MATRIX_MUL_BUFFER failed: %s\n", strerror(errno));
        rc = 1;
    } else {
        memcpy(c, c_map, c_bytes);
    }

out_free:
    if (c_map != NULL)
        munmap(c_map, c_buf.allocated_size);
    if (b_map != NULL)
        munmap(b_map, b_buf.allocated_size);
    if (a_map != NULL)
        munmap(a_map, a_buf.allocated_size);
    kernel_free_buffer(fd, c_buf.handle);
    kernel_free_buffer(fd, b_buf.handle);
    kernel_free_buffer(fd, a_buf.handle);
    close(fd);
    return rc;
}

static int kernel_cmd_matrix_mul(int arg_count, char **args)
{
    int32_t a[] = {
        1, 2, 3,
        4, 5, 6
    };
    int32_t b[] = {
        7, 8,
        9, 10,
        11, 12
    };
    int32_t c[] = {0, 0, 0, 0};
    int32_t *a_values = a;
    int32_t *b_values = b;
    int32_t *c_values = c;
    uint32_t m = 2;
    uint32_t n = 2;
    uint32_t k = 3;
    uint32_t a_count = 6;
    uint32_t b_count = 6;
    uint32_t c_count = 4;
    uint32_t expected_a_count;
    uint32_t expected_b_count;
    uint32_t expected_c_count;
    int rc;

    if (arg_count != 0 && arg_count != 5) {
        fprintf(stderr, "usage: fwctl run-matrix-mul [m n k a0,a1,... b0,b1,...]\n");
        return 2;
    }

    if (arg_count == 5) {
        rc = parse_u32_arg(args[0], &m);
        if (rc != 0) {
            fprintf(stderr, "invalid matrix row count `%s`\n", args[0]);
            return 2;
        }
        rc = parse_u32_arg(args[1], &n);
        if (rc != 0) {
            fprintf(stderr, "invalid matrix output column count `%s`\n", args[1]);
            return 2;
        }
        rc = parse_u32_arg(args[2], &k);
        if (rc != 0) {
            fprintf(stderr, "invalid matrix inner dimension `%s`\n", args[2]);
            return 2;
        }

        rc = checked_matrix_element_count(m, k, &expected_a_count);
        if (rc != 0) {
            fprintf(stderr, "invalid A matrix shape %ux%u\n", m, k);
            return 2;
        }
        rc = checked_matrix_element_count(k, n, &expected_b_count);
        if (rc != 0) {
            fprintf(stderr, "invalid B matrix shape %ux%u\n", k, n);
            return 2;
        }
        rc = checked_matrix_element_count(m, n, &expected_c_count);
        if (rc != 0) {
            fprintf(stderr, "invalid C matrix shape %ux%u\n", m, n);
            return 2;
        }

        rc = parse_i32_vector(args[3], &a_values, &a_count);
        if (rc != 0) {
            fprintf(stderr, "invalid matrix A values `%s`\n", args[3]);
            return 2;
        }
        rc = parse_i32_vector(args[4], &b_values, &b_count);
        if (rc != 0) {
            fprintf(stderr, "invalid matrix B values `%s`\n", args[4]);
            free(a_values);
            return 2;
        }
        if (a_count != expected_a_count || b_count != expected_b_count) {
            fprintf(stderr,
                    "matrix value count mismatch: A expects %u values, got %u; B expects %u values, got %u\n",
                    expected_a_count,
                    a_count,
                    expected_b_count,
                    b_count);
            free(b_values);
            free(a_values);
            return 2;
        }

        c_count = expected_c_count;
        c_values = calloc(c_count, sizeof(*c_values));
        if (c_values == NULL) {
            free(b_values);
            free(a_values);
            return 1;
        }
    }

    rc = kernel_run_matrix_mul(a_values, b_values, c_values, m, n, k);
    if (rc == 0)
        print_i32_matrix("matrix-mul i32", c_values, m, n);

    if (arg_count == 5) {
        free(c_values);
        free(b_values);
        free(a_values);
    }
    return rc;
}

static int kernel_cmd_matrix_mul_buffer(int arg_count, char **args)
{
    int32_t a[] = {1, 2, 3, 4, 5, 6};
    int32_t b[] = {7, 8, 9, 10, 11, 12};
    int32_t c[] = {0, 0, 0, 0};
    int32_t *a_values = a;
    int32_t *b_values = b;
    int32_t *c_values = c;
    uint32_t m = 2;
    uint32_t n = 2;
    uint32_t k = 3;
    uint32_t a_count = 6;
    uint32_t b_count = 6;
    uint32_t c_count = 4;
    uint32_t expected_a_count;
    uint32_t expected_b_count;
    uint32_t expected_c_count;
    int rc;

    if (arg_count != 0 && arg_count != 5) {
        fprintf(stderr, "usage: fwctl run-matrix-mul-buffer [m n k a0,a1,... b0,b1,...]\n");
        return 2;
    }

    if (arg_count == 5) {
        if (parse_u32_arg(args[0], &m) != 0 || parse_u32_arg(args[1], &n) != 0 || parse_u32_arg(args[2], &k) != 0)
            return 2;
        if (checked_matrix_element_count(m, k, &expected_a_count) != 0 ||
            checked_matrix_element_count(k, n, &expected_b_count) != 0 ||
            checked_matrix_element_count(m, n, &expected_c_count) != 0)
            return 2;
        rc = parse_i32_vector(args[3], &a_values, &a_count);
        if (rc != 0)
            return 2;
        rc = parse_i32_vector(args[4], &b_values, &b_count);
        if (rc != 0) {
            free(a_values);
            return 2;
        }
        if (a_count != expected_a_count || b_count != expected_b_count) {
            free(b_values);
            free(a_values);
            return 2;
        }
        c_count = expected_c_count;
        c_values = calloc(c_count, sizeof(*c_values));
        if (c_values == NULL) {
            free(b_values);
            free(a_values);
            return 1;
        }
    }

    rc = kernel_run_matrix_mul_buffer(a_values, b_values, c_values, m, n, k);
    if (rc == 0)
        print_i32_matrix("matrix-mul buffer i32", c_values, m, n);

    if (arg_count == 5) {
        free(c_values);
        free(b_values);
        free(a_values);
    }
    return rc;
}

static int kernel_cmd_memcopy(void)
{
    char src[] = "accelerator";
    char dst[sizeof(src)];
    struct afl_ioctl_memcopy req;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(dst, 0, sizeof(dst));
    memset(&req, 0, sizeof(req));
    req.src_user_ptr = (uint64_t)(uintptr_t)src;
    req.dst_user_ptr = (uint64_t)(uintptr_t)dst;
    req.length = sizeof(src);

    if (ioctl(fd, AFL_IOCTL_MEMCOPY, &req) != 0) {
        fprintf(stderr, "AFL_IOCTL_MEMCOPY failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("memcopy: %s\n", dst);
    close(fd);
    return 0;
}

static int kernel_cmd_telemetry(void)
{
    struct afl_ioctl_telemetry telemetry;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&telemetry, 0, sizeof(telemetry));
    if (ioctl(fd, AFL_IOCTL_GET_TELEMETRY, &telemetry) != 0) {
        fprintf(stderr, "AFL_IOCTL_GET_TELEMETRY failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("boot_count: %llu\n", (unsigned long long)telemetry.boot_count);
    printf("reset_count: %llu\n", (unsigned long long)telemetry.reset_count);
    printf("command_count: %llu\n", (unsigned long long)telemetry.command_count);
    printf("completion_count: %llu\n", (unsigned long long)telemetry.completion_count);
    printf("failed_command_count: %llu\n", (unsigned long long)telemetry.failed_command_count);
    printf("current_fw_state: %u\n", telemetry.current_fw_state);
    printf("last_error_code: %u\n", telemetry.last_error_code);

    close(fd);
    return 0;
}

static int kernel_cmd_trigger_fault(void)
{
    struct afl_ioctl_fault fault;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&fault, 0, sizeof(fault));
    fault.fault_kind = AFL_IOCTL_FAULT_ECC;
    if (ioctl(fd, AFL_IOCTL_TRIGGER_FAULT, &fault) != 0) {
        fprintf(stderr, "AFL_IOCTL_TRIGGER_FAULT failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("trigger-fault completion: status=%u error=%u\n", fault.status, fault.error_code);
    close(fd);
    return fault.error_code == AFL_IOCTL_ERR_ECC ? 0 : 1;
}

static int kernel_cmd_trigger_timeout(void)
{
    struct afl_ioctl_completion completion;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&completion, 0, sizeof(completion));
    if (ioctl(fd, AFL_IOCTL_TRIGGER_TIMEOUT, &completion) != 0) {
        fprintf(stderr, "AFL_IOCTL_TRIGGER_TIMEOUT failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("trigger-timeout completion: status=%u error=%u result=%u\n",
           completion.status,
           completion.error_code,
           completion.result);
    close(fd);
    return completion.error_code == AFL_IOCTL_ERR_TIMEOUT ? 0 : 1;
}

static int kernel_cmd_reset(void)
{
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    if (ioctl(fd, AFL_IOCTL_RESET) != 0) {
        fprintf(stderr, "AFL_IOCTL_RESET failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("reset: PASS\n");
    close(fd);
    return 0;
}

static int kernel_cmd_dump_status(void)
{
    struct afl_ioctl_status status;
    const struct {
        uint32_t offset;
        const char *name;
        uint32_t value;
    } regs[] = {
        {AFL_IOCTL_REG_DEVICE_ID, "DEVICE_ID", 0},
        {AFL_IOCTL_REG_VENDOR_ID, "VENDOR_ID", 0},
        {AFL_IOCTL_REG_ABI_VERSION, "ABI_VERSION", 0},
        {AFL_IOCTL_REG_FW_VERSION, "FW_VERSION", 0},
        {AFL_IOCTL_REG_DEVICE_STATUS, "DEVICE_STATUS", 0},
        {AFL_IOCTL_REG_ERROR_CODE, "ERROR_CODE", 0},
        {AFL_IOCTL_REG_DOORBELL, "DOORBELL", 0},
        {AFL_IOCTL_REG_IRQ_STATUS, "IRQ_STATUS", 0},
        {AFL_IOCTL_REG_COMMAND_QUEUE_HEAD, "COMMAND_QUEUE_HEAD", 0},
        {AFL_IOCTL_REG_COMMAND_QUEUE_TAIL, "COMMAND_QUEUE_TAIL", 0},
        {AFL_IOCTL_REG_COMPLETION_QUEUE_HEAD, "COMPLETION_QUEUE_HEAD", 0},
        {AFL_IOCTL_REG_COMPLETION_QUEUE_TAIL, "COMPLETION_QUEUE_TAIL", 0},
        {AFL_IOCTL_REG_COMMAND_COUNT_LO, "COMMAND_COUNT_LO", 0},
        {AFL_IOCTL_REG_RESET_COUNT_LO, "RESET_COUNT_LO", 0},
        {AFL_IOCTL_REG_TRACE_HEAD, "TRACE_HEAD", 0},
        {AFL_IOCTL_REG_TRACE_COUNT, "TRACE_COUNT", 0},
        {AFL_IOCTL_REG_IRQ_VECTOR_STATUS, "IRQ_VECTOR_STATUS", 0},
        {AFL_IOCTL_REG_IRQ_VECTOR_MASK, "IRQ_VECTOR_MASK", 0},
        {AFL_IOCTL_REG_IRQ_VECTOR_ENABLE, "IRQ_VECTOR_ENABLE", 0},
    };
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&status, 0, sizeof(status));
    if (ioctl(fd, AFL_IOCTL_GET_STATUS, &status) != 0) {
        fprintf(stderr, "AFL_IOCTL_GET_STATUS failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("register map:\n");
    printf("  [%02u] %-24s 0x%08x\n", regs[0].offset, regs[0].name, status.device_id);
    printf("  [%02u] %-24s 0x%08x\n", regs[1].offset, regs[1].name, status.vendor_id);
    printf("  [%02u] %-24s 0x%08x\n", regs[2].offset, regs[2].name, status.abi_version);
    printf("  [%02u] %-24s 0x%08x\n", regs[3].offset, regs[3].name, status.fw_version);
    printf("  [%02u] %-24s 0x%08x\n", regs[4].offset, regs[4].name, status.device_status);
    printf("  [%02u] %-24s 0x%08x\n", regs[5].offset, regs[5].name, status.error_code);
    printf("  [%02u] %-24s 0x%08x\n", regs[6].offset, regs[6].name, status.doorbell);
    printf("  [%02u] %-24s 0x%08x\n", regs[7].offset, regs[7].name, status.irq_status);
    printf("  [%02u] %-24s 0x%08x\n", regs[8].offset, regs[8].name, status.command_queue_head);
    printf("  [%02u] %-24s 0x%08x\n", regs[9].offset, regs[9].name, status.command_queue_tail);
    printf("  [%02u] %-24s 0x%08x\n", regs[10].offset, regs[10].name, status.completion_queue_head);
    printf("  [%02u] %-24s 0x%08x\n", regs[11].offset, regs[11].name, status.completion_queue_tail);
    printf("  [%02u] %-24s 0x%08x\n", regs[12].offset, regs[12].name, status.command_count);
    printf("  [%02u] %-24s 0x%08x\n", regs[13].offset, regs[13].name, status.reset_count);
    printf("  [%02u] %-24s 0x%08x\n", regs[14].offset, regs[14].name, status.trace_head);
    printf("  [%02u] %-24s 0x%08x\n", regs[15].offset, regs[15].name, status.trace_count);
    printf("  [%02u] %-24s 0x%08x\n", regs[16].offset, regs[16].name, status.irq_vector_status);
    printf("  [%02u] %-24s 0x%08x\n", regs[17].offset, regs[17].name, status.irq_vector_mask);
    printf("  [%02u] %-24s 0x%08x\n", regs[18].offset, regs[18].name, status.irq_vector_enable);

    close(fd);
    return 0;
}

static const char *trace_event_name(uint32_t event)
{
    switch (event) {
    case AFL_IOCTL_TRACE_BOOT:
        return "BOOT";
    case AFL_IOCTL_TRACE_COMMAND_QUEUED:
        return "COMMAND_QUEUED";
    case AFL_IOCTL_TRACE_DOORBELL:
        return "DOORBELL";
    case AFL_IOCTL_TRACE_COMMAND_START:
        return "COMMAND_START";
    case AFL_IOCTL_TRACE_COMMAND_COMPLETE:
        return "COMMAND_COMPLETE";
    case AFL_IOCTL_TRACE_IRQ_RAISE:
        return "IRQ_RAISE";
    case AFL_IOCTL_TRACE_ERROR:
        return "ERROR";
    case AFL_IOCTL_TRACE_RESET:
        return "RESET";
    case AFL_IOCTL_TRACE_WATCHDOG_TIMEOUT:
        return "WATCHDOG_TIMEOUT";
    case AFL_IOCTL_TRACE_RECOVERY_ENTER:
        return "RECOVERY_ENTER";
    case AFL_IOCTL_TRACE_RECOVERY_COMPLETE:
        return "RECOVERY_COMPLETE";
    default:
        return "UNKNOWN";
    }
}

static const char *trace_opcode_name(uint32_t opcode)
{
    switch (opcode) {
    case 0:
        return "-";
    case 1:
        return "GET_VERSION";
    case 2:
        return "GET_TELEMETRY";
    case 3:
        return "GET_STATUS";
    case 4:
        return "RESET";
    case 5:
        return "RUN_SELFTEST";
    case 6:
        return "MEMCOPY";
    case 7:
        return "VECTOR_ADD";
    case 8:
        return "MATRIX_MUL";
    case 9:
        return "TRIGGER_FAULT";
    case 10:
        return "TRIGGER_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

static int kernel_cmd_dump_trace(void)
{
    struct afl_ioctl_trace trace;
    uint32_t i;
    int fd = kernel_open();
    if (fd < 0)
        return 1;

    memset(&trace, 0, sizeof(trace));
    if (ioctl(fd, AFL_IOCTL_GET_TRACE, &trace) != 0) {
        fprintf(stderr, "AFL_IOCTL_GET_TRACE failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    printf("trace_count: %u\n", trace.count);
    printf("trace_dropped_count: %u\n", trace.dropped_count);
    for (i = 0; i < trace.count; ++i) {
        const struct afl_ioctl_trace_entry *entry = &trace.entries[i];
        printf("%02u cycle=%llu event=%s opcode=%s status=%u error=%u\n",
               i,
               (unsigned long long)entry->cycle,
               trace_event_name(entry->event),
               trace_opcode_name(entry->opcode),
               entry->status,
               entry->error_code);
    }

    close(fd);
    return 0;
}

static int run_kernel_command(const char *command, int arg_count, char **args)
{
    if (arg_count != 0 &&
        strcmp(command, "run-vector-add") != 0 &&
        strcmp(command, "run-vector-add-buffer") != 0 &&
        strcmp(command, "run-vector-add-async") != 0 &&
        strcmp(command, "run-matrix-mul") != 0 &&
        strcmp(command, "run-matrix-mul-buffer") != 0)
        return 2;

    if (strcmp(command, "version") == 0)
        return kernel_cmd_version();
    if (strcmp(command, "selftest") == 0)
        return kernel_cmd_selftest();
    if (strcmp(command, "run-vector-add") == 0)
        return kernel_cmd_vector_add(arg_count, args);
    if (strcmp(command, "run-vector-add-buffer") == 0)
        return kernel_cmd_vector_add_buffer(arg_count, args);
    if (strcmp(command, "run-vector-add-async") == 0)
        return kernel_cmd_vector_add_async(arg_count, args);
    if (strcmp(command, "run-matrix-mul") == 0)
        return kernel_cmd_matrix_mul(arg_count, args);
    if (strcmp(command, "run-matrix-mul-buffer") == 0)
        return kernel_cmd_matrix_mul_buffer(arg_count, args);
    if (strcmp(command, "memcopy") == 0)
        return kernel_cmd_memcopy();
    if (strcmp(command, "telemetry") == 0)
        return kernel_cmd_telemetry();
    if (strcmp(command, "trigger-fault") == 0)
        return kernel_cmd_trigger_fault();
    if (strcmp(command, "trigger-timeout") == 0)
        return kernel_cmd_trigger_timeout();
    if (strcmp(command, "reset") == 0)
        return kernel_cmd_reset();
    if (strcmp(command, "dump-status") == 0)
        return kernel_cmd_dump_status();
    if (strcmp(command, "dump-trace") == 0)
        return kernel_cmd_dump_trace();
    return 2;
}

int main(int argc, char **argv)
{
    int rc;

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    rc = run_kernel_command(argv[1], argc - 2, &argv[2]);
    if (rc == 2)
        print_usage(argv[0]);
    return rc;
}
