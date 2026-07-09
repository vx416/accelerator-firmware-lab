# accelerator-firmware-lab

## Overview

`accelerator-firmware-lab` is a Linux kernel driver lab for a simplified
accelerator platform. It models the contract between a userspace debug tool, a
kernel driver, firmware-style device logic, and simulated accelerator hardware.

The project does not implement a real TPU, GPU, NPU, SmartNIC, or DPU. The
virtual accelerator lives inside an out-of-tree Linux kernel module and exposes
a `/dev/afl0` control interface through ioctl. The implementation focuses on
the mechanics that accelerator platforms commonly need: ABI design, register
maps, command submission, completion handling, DMA-like buffer ownership,
interrupt-like notification, telemetry, trace capture, watchdog timeout, and
reset/recovery flows.

## Why This Exists

The goal is to make accelerator platform concepts inspectable without requiring
real PCIe hardware or a QEMU device model on day one.

Device drivers sit at the boundary between the kernel and hardware. They turn
kernel or userspace requests into device-specific control, data movement, and
completion handling. For accelerator-style devices, that usually means
programming registers, staging buffers, submitting compute work, handling
completion queues, reacting to interrupts, and recovering from firmware or
device faults.

This lab exists to make those ideas concrete in a small codebase. Instead of
starting with a full PCIe device model, it first builds a kernel-resident
virtual accelerator so the driver path, firmware path, and debug path can be
read together. That makes it easier to understand how a kernel module can model
I/O and compute submission, how completion notification works, and where a real
system would later replace fake MMIO, fake DMA, and fake IRQs with PCIe BARs,
DMA mappings, and MSI/MSI-X handlers.

This repository is intended to exercise:

- kernel/user ABI design;
- Linux misc/character driver structure;
- driver-owned buffer staging;
- command queue and completion queue flow;
- MMIO-style control/status registers;
- firmware-style state transitions;
- fault injection, watchdog timeout, recovery, and telemetry;
- debug tooling similar to early bring-up utilities.

## Table of Contents

- [Architecture and Mental Model](#architecture-and-mental-model)
  - [Driver/User Contract](#driveruser-contract)
  - [Kernel Driver Path](#kernel-driver-path)
  - [DMA Model](#dma-model)
  - [MMIO Register Map](#mmio-register-map)
  - [Interrupt and Completion Model](#interrupt-and-completion-model)
  - [Firmware State Machine](#firmware-state-machine)
  - [Trace, Telemetry, and Recovery](#trace-telemetry-and-recovery)
  - [Virtual Accelerator Model](#virtual-accelerator-model)
  - [Repository Layout](#repository-layout)
- [How to Use](#how-to-use)
  - [Common Commands](#common-commands)
  - [Build and Run](#build-and-run)
  - [Test](#test)
  - [Example Commands](#example-commands)
  - [Development VM](#development-vm)
- [Limitations](#limitations)
  - [Future Plan: QEMU PCIe Device](#future-plan-qemu-pcie-device)

## Architecture and Mental Model

At a high level, an accelerator driver sits between userspace software and a
device-specific hardware/firmware interface:

```text
userspace runtime / debug tool
  |
  | ioctl / mmap / poll
  v
Linux kernel driver
  |
  +-- MMIO registers: control, status, doorbells
  +-- DMA-visible memory: commands, input/output buffers, completions
  +-- IRQ/MSI-X: device-to-host completion and fault notification
  v
device firmware / hardware
```

The important split is:

- **ioctl** is how userspace enters the driver.
- **MMIO** is how the driver programs small device control/status registers.
- **DMA** is how command data, tensors, and completion entries move through
  memory visible to the device.
- **IRQ** is how the device tells the driver that work completed or failed.

This project keeps that same shape, but implements the device side inside the
kernel module:

```text
fwctl
  |
  | ioctl(/dev/afl0)
  v
kernel driver layer
  |
  +-- validate ioctl request
  +-- stage userspace buffers
  +-- build command descriptor
  +-- ring fake MMIO doorbell
  +-- wait for fake IRQ completion
  v
kernel virtual accelerator layer
  |
  +-- fake MMIO register table
  +-- fake DMA buffer access
  +-- command queue and completion queue
  +-- firmware state machine
  +-- trace, telemetry, watchdog, recovery
```

The following sections expand that flow from the userspace ABI down to the
virtual device model.

### Driver/User Contract

The contract is intentionally small and visible in `include/afl/protocol.h` and
`include/afl/ioctl.h`.

`protocol.h` defines protocol-level constants and enums:

- device and vendor IDs;
- ABI and firmware version fields;
- firmware states;
- device status bits;
- error codes;
- fake MMIO register offsets;
- fake IRQ reason bits;
- fake IRQ vector IDs;
- trace event IDs.

`ioctl.h` defines the userspace/kernel ABI:

- ioctl command numbers;
- request and response structs;
- workload structs carrying userspace pointers;
- completion and trace snapshot structs.

Workload ioctl requests contain userspace pointers. The kernel driver validates
the request, copies userspace input into driver-owned fake DMA buffers, submits
a virtual-device command containing fake DMA addresses, and copies output back
to userspace after completion.

### Kernel Driver Path

After userspace enters the driver through ioctl, the workload path follows the
same ordering used by many real accelerator drivers:

```text
1. Stage command data and buffers into memory the device can read.
2. Ring a doorbell register through MMIO so firmware knows work is available.
3. Wait for completion or error notification, then read the completion result.
```

Synchronous workload flow:

```text
fwctl
  |
  | ioctl(AFL_IOCTL_VECTOR_ADD / MATRIX_MUL / MEMCOPY)
  v
kernel/driver.c
  |
  | validate request
  | copy_from_user()
  v
kernel/driver_dma.c
  |
  | allocate bounded fake DMA buffers
  | build afl_kvdev_command with dma_addr values
  v
kernel/driver_transport.c
  |
  | afl_kvdev_submit_command()
  | afl_kvdev_mmio_write32(DOORBELL)
  | afl_kvdev_wait_irq(COMPLETION)
  | afl_kvdev_pop_completion()
  v
kernel virtual accelerator
  |
  | execute workload
  | push completion
  | raise fake IRQ
  v
kernel/driver_dma.c
  |
  | copy_to_user()
  v
fwctl
```

Async vector-add flow:

```text
fwctl
  |
  | ioctl(AFL_IOCTL_VECTOR_ADD_ASYNC)
  v
driver stages buffers and submits command
  |
  | poll(/dev/afl0)
  v
fake IRQ marks device readable
  |
  | ioctl(AFL_IOCTL_GET_COMPLETION)
  v
driver pops completion and returns output
```

The initial async path supports one outstanding vector-add request.

### DMA Model

After ioctl validation, the first driver job is to put command data and workload
buffers somewhere the device side is allowed to read. Real DMA means the device
reads and writes host memory directly after the driver maps approved buffers for
the device. This project does not perform real hardware DMA. It models the
ownership boundary with driver-owned staging buffers:

```text
userspace pointer
  |
  | copy_from_user()
  v
driver-owned kernel buffer
  |
  | fake dma_addr = kernel buffer address
  v
virtual device workload handler
  |
  | fake DMA read/write
  v
driver-owned kernel buffer
  |
  | copy_to_user()
  v
userspace pointer
```

This keeps the boundary explicit:

- userspace never directly mutates virtual-device state;
- driver code owns userspace copies and staging;
- the virtual device sees only fake DMA addresses;
- workload execution uses fixed internal scratch memory.

After staging, the command descriptor contains addresses into these fake DMA
buffers. From the virtual device's point of view, it receives a command with
device-readable addresses rather than raw userspace pointers.

### MMIO Register Map

After the fake DMA buffers and command descriptor exist, the driver still needs
a control signal that tells firmware to consume the queue. That is the MMIO
role: small registers carry control, status, queue indices, and doorbells. In
real PCIe hardware, MMIO registers normally live in a BAR mapped by the driver
with `pci_iomap()` or `ioremap()`. This project does not have a real BAR.
Instead, the virtual device keeps an internal fake register table so the
control/status contract is still explicit.

```text
offset  name                    meaning
0       DEVICE_ID               fixed simulated device id
1       VENDOR_ID               fixed simulated vendor id
2       ABI_VERSION             userspace/kernel ABI version
3       FW_VERSION              simulated firmware version
4       DEVICE_STATUS           READY/RUNNING/ERROR/RECOVERY bits
5       ERROR_CODE              last firmware/device error
6       DOORBELL                driver writes COMMAND_QUEUE bit here
7       IRQ_STATUS              pending fake IRQ reason bits
8       COMMAND_QUEUE_HEAD      virtual device command consumer index
9       COMMAND_QUEUE_TAIL      driver command producer index
10      COMPLETION_QUEUE_HEAD   driver completion consumer index
11      COMPLETION_QUEUE_TAIL   virtual device completion producer index
12      COMMAND_COUNT_LO        low 32 bits of telemetry command count
13      RESET_COUNT_LO          low 32 bits of telemetry reset count
14      TRACE_HEAD              trace ring producer index
15      TRACE_COUNT             retained trace entry count
16      IRQ_VECTOR_STATUS       pending fake interrupt-vector bits
17      IRQ_VECTOR_MASK         masked fake interrupt-vector bits
18      IRQ_VECTOR_ENABLE       enabled fake interrupt-vector bits
```

`fwctl dump-status` prints a status snapshot based on these registers. The
doorbell register is the key control point: the driver writes it after queuing a
command, and the virtual firmware consumes the command queue.

### Interrupt and Completion Model

After the driver rings the doorbell, the device side needs a way to report
"work completed" or "work failed" without forcing userspace to blindly poll
status forever. Real PCIe devices typically use INTx, MSI, or MSI-X interrupts.
A production driver would allocate IRQ vectors and register handlers with
`request_irq()`. This project does not request a real hardware IRQ.

Instead, the virtual device models completion notification with:

- `irq_pending` reason bits;
- fake IRQ vector status/mask/enable bits;
- a kernel wait queue;
- `.poll` support on `/dev/afl0`.

Current fake vector layout:

```text
vector  name          source
0       ADMIN         reset/admin events
1       COMPLETION    completion queue ready
2       ERROR         firmware/device error
3       TELEMETRY     telemetry snapshot ready
```

Completion flow:

```text
virtual device pushes completion
  |
sets fake IRQ bits and vector status
  |
wake_up_interruptible()
  |
waiting ioctl path or poll() wakes
  |
driver pops completion queue entry
```

This models the software shape of interrupt-driven completion without claiming
to exercise real MSI/MSI-X.

### Firmware State Machine

Once commands can be staged, signaled, and completed, the virtual device needs
firmware-style state so failures and recovery are observable rather than just
return values from a function call. The virtual firmware models lifecycle and
command execution states:

```text
RESET
BOOTING
SELF_TEST
READY
RUNNING
ERROR
RECOVERY
```

The state machine is useful because accelerator failures are rarely just
"return an errno." A command can fail after the device has already accepted
work, updated internal state, or partially produced telemetry. The firmware path
therefore records state transitions, updates error registers, accounts for fake
cycles, and decides whether recovery is needed.

Each opcode has a simulated cycle budget. `AFL_IOCTL_TRIGGER_TIMEOUT`
intentionally exceeds its budget so the firmware records a watchdog timeout,
reports `AFL_IOCTL_ERR_TIMEOUT`, enters recovery, and returns the device to
READY.

### Trace, Telemetry, and Recovery

After the firmware path can enter RUNNING, ERROR, and RECOVERY, the next problem
is debugging: when a command fails, the driver needs more than a single errno.
Trace and telemetry are the observability layer. They answer different
questions:

- **status registers** answer "what state is the device in right now?"
- **telemetry counters** answer "what has happened over time?"
- **trace entries** answer "what sequence of firmware events led here?"

Telemetry tracks:

- boot count;
- reset count;
- command count;
- completion count;
- failed command count;
- timeout count;
- recovery count;
- last command cycles;
- current firmware state;
- last error code.

The trace ring is fixed-size and kernel-resident. It records firmware/device
events such as:

```text
BOOT
COMMAND_QUEUED
DOORBELL
COMMAND_START
COMMAND_COMPLETE
IRQ_RAISE
ERROR
RESET
WATCHDOG_TIMEOUT
RECOVERY_ENTER
RECOVERY_COMPLETE
```

When something fails, the intended debug path is:

```text
fwctl dump-status
  inspect current register/status/error view

fwctl telemetry
  inspect aggregate counters and firmware state

fwctl dump-trace
  inspect event ordering around command, IRQ, timeout, or recovery
```

`tests/kernel_verifier.c` also checks trace ordering for a vector-add command:
queued -> doorbell -> command start -> command complete.

### Virtual Accelerator Model

The previous sections describe the pieces of the driver/device interaction. The
virtual accelerator model is where those pieces live together: a kernel-resident
device model, not a userspace simulator. Its state is held in
`struct afl_kernel_vdev` and includes:

- mutex-protected device state;
- fake MMIO registers;
- fake IRQ pending bits;
- command queue;
- completion queue;
- telemetry counters;
- fake IRQ vector state;
- trace ring;
- fixed scratch memory;
- fake cycle counter.

It also contains a small workload engine used to exercise the driver path:

- `MEMCOPY`: copies bytes from one fake DMA buffer to another.
- `VECTOR_ADD`: adds two int32 vectors into an output buffer.
- `MATRIX_MUL`: multiplies row-major int32 matrices.
- `TRIGGER_FAULT`: injects firmware/device fault states.
- `TRIGGER_TIMEOUT`: exercises watchdog timeout and recovery.

These workloads are intentionally small. Their purpose is not numerical
performance; it is to force realistic driver/device behavior: input staging,
command descriptors, device-side reads and writes, completion reporting,
telemetry updates, and failure handling.

The driver and virtual device are intentionally separated inside the same kernel
module:

```text
driver layer:
  /dev/afl0, ioctl, userspace copies, fake DMA staging

virtual device layer:
  registers, queues, firmware state, opcode execution, trace, completion
```

That split keeps the project close to a future PCIe implementation: the driver
side can later replace fake MMIO/DMA/IRQ calls with BAR access, DMA API calls,
and MSI-X handlers, while preserving most of the contract and debug model.

The implemented user-facing surface includes version, status, telemetry, reset,
self-test, workload, trace, fault, timeout, async submit, and completion ioctls.
The C verifier exercises negative paths, async completion, recovery, telemetry,
and trace ordering against `/dev/afl0`.

### Repository Layout

```text
include/afl/protocol.h
  Shared userspace/kernel protocol constants: status/error enums, register
  offsets, fake IRQ vectors, trace event IDs, and device/version constants.

include/afl/ioctl.h
  Shared userspace/kernel ioctl ABI: ioctl command numbers and request/response
  structs. It includes protocol.h.

kernel/
  driver.c
    /dev/afl0 misc device, file_operations, ioctl handling.

  driver_dma.c
    Driver-owned fake DMA staging buffers and userspace copy boundaries.

  driver_transport.c
    Command submit, fake MMIO doorbell write, fake IRQ wait, completion pop.

  virtual_device.c
    Public kernel virtual-device entry points.

  virtual_device_regs.c
    Fake MMIO register access and status snapshot generation.

  virtual_device_queues.c
    Command queue, completion queue, and doorbell processing.

  virtual_device_irq.c
    Fake IRQ bits, fake IRQ vectors, wait queue, and poll mask.

  virtual_device_trace.c
    Fixed-size firmware trace ring.

  virtual_device_fw.c
    Firmware state transitions, opcode dispatch, cycle budget, watchdog timeout,
    and recovery state machine.

  virtual_device_workloads.c
    Memcopy, vector add, and matrix multiply handlers using fixed scratch memory.

tools/fwctl/fwctl.c
  Userspace control/debug CLI.

tests/kernel_verifier.c
  C verification binary for kernel ABI and virtual-device behavior.

scripts/
  Kernel module load, unload, and smoke-test helpers.

docs/
  Design notes.

notes/
  Learning notes.

Vagrantfile
  Reproducible Linux driver development VM.
```

## How to Use

### Common Commands

Inside the Vagrant VM, the common workflow is:

```bash
make build
make kernel
make load
make verifier
make smoke
make unload
```

From the host, the quickest full VM check is:

```bash
make vagrant-smoke
```

Useful targets:

- `make build`: build `fwctl` and `afl-kernel-verifier`.
- `make kernel`: build `kernel/afl_kernel.ko`.
- `make load`: build and load the kernel module.
- `make verifier`: run C verification tests against `/dev/afl0`.
- `make smoke`: run the full kernel smoke test.

### Build and Run

Build the userspace control tool and C verifier:

```sh
cmake -S . -B build
cmake --build build
```

Build and load the kernel module inside the VM:

```sh
./scripts/load-kernel-module.sh
```

Unload it:

```sh
./scripts/unload-kernel-module.sh
```

`fwctl` requires `/dev/afl0`, so the kernel module must be loaded before running
commands.

### Test

Inside the Linux VM:

```sh
cd /home/vagrant/accelerator-firmware-lab
./scripts/kernel-smoke-test.sh
```

The smoke test builds `fwctl` and `afl-kernel-verifier`, builds and loads
`afl_kernel.ko`, runs supported `fwctl` commands, runs the C verifier against
`/dev/afl0`, then unloads the module.

The C verifier checks behavior that is easy to miss in a demo-only workflow:

```text
invalid userspace pointer rejection
oversized fake DMA request rejection
async completion through poll()
async backpressure for the single outstanding request slot
empty completion queue reporting
watchdog timeout and recovery to READY
timeout/recovery telemetry counter updates
fault injection followed by reset recovery
trace event order: queued -> doorbell -> command start -> command complete
```

The verifier can also be run directly after the module is loaded:

```sh
make verifier
```

### Example Commands

```sh
./build/fwctl version
./build/fwctl selftest
./build/fwctl run-vector-add
./build/fwctl run-vector-add 5,6,7 50,60,70
./build/fwctl run-vector-add-async 2,4,6 20,40,60
./build/fwctl run-matrix-mul
./build/fwctl run-matrix-mul 2 2 3 1,2,3,4,5,6 7,8,9,10,11,12
./build/fwctl memcopy
./build/fwctl telemetry
./build/fwctl trigger-fault
./build/fwctl trigger-timeout
./build/fwctl reset
./build/fwctl dump-status
./build/fwctl dump-trace
```

### Development VM

This project includes a Vagrant VM for a reproducible ARM64 Ubuntu development
environment:

```sh
vagrant up
vagrant ssh
cd /home/vagrant/accelerator-firmware-lab
```

The VM installs C build tools, kernel headers, tracing/debug tools, Docker, Go,
and QEMU packages. The current kernel-first phase does not require QEMU source
code or a custom QEMU device model.

## Limitations

This project is intentionally phase-2 style: a kernel module with an in-kernel
virtual accelerator.

It does not yet implement:

- a real PCIe device;
- PCI probe/remove;
- BAR mapping through `pci_iomap()` or `ioremap()`;
- real MMIO through `readl()`/`writel()` against hardware;
- real DMA API usage with IOMMU/SMMU mappings;
- real hardware IRQs;
- MSI/MSI-X allocation and `request_irq()`;
- multi-queue scheduling;
- true cycle-level or RTL-accurate timing;
- firmware running on a separate embedded microcontroller target.

The fake MMIO, fake DMA, and fake IRQ layers are deliberate scaffolding. They
make the driver/device contract explicit before replacing the backend with a
real or QEMU-modeled PCIe device.

### Future Plan: QEMU PCIe Device

A future phase can move the virtual accelerator behind a QEMU PCIe device model:

```text
fwctl
  |
  | ioctl/mmap/poll
  v
Linux PCI driver
  |
  +-- pci_driver probe/remove
  +-- BAR/MMIO readl/writel
  +-- DMA API
  +-- MSI/MSI-X request_irq handlers
  v
QEMU PCIe virtual accelerator
  |
  +-- PCI config space
  +-- BAR register block
  +-- command queue processing
  +-- guest memory DMA
  +-- MSI/MSI-X interrupt generation
```

The current register map, queue model, completion model, trace events, and
firmware state machine are designed to be portable to that phase.

Suggested milestones:

1. Add a minimal QEMU PCIe device visible in `lspci`.
2. Implement BAR0 identity/status/doorbell registers.
3. Write a Linux PCI driver that probes the device and maps BAR0.
4. Add MSI-X and a real IRQ handler.
5. Add guest-memory DMA for command and completion queues.
6. Move workload execution into the QEMU device model.
