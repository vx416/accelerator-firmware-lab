BUILD_DIR ?= build
CMAKE ?= cmake
MAKE ?= make

.PHONY: help configure build kernel load unload smoke verifier clean vagrant-up vagrant-rsync vagrant-smoke

help:
	@echo "accelerator-firmware-lab targets:"
	@echo "  make build          Build userspace tools and verification binaries"
	@echo "  make kernel         Build the out-of-tree kernel module"
	@echo "  make load           Build and load afl_kernel.ko, creating /dev/afl0"
	@echo "  make unload         Unload afl_kernel.ko"
	@echo "  make smoke          Run full kernel smoke test"
	@echo "  make verifier       Run C verification tests against /dev/afl0"
	@echo "  make clean          Clean CMake and kernel build outputs"
	@echo "  make vagrant-up     Start the Vagrant VM"
	@echo "  make vagrant-rsync  Sync this workspace into the Vagrant VM"
	@echo "  make vagrant-smoke  Sync and run smoke test inside the Vagrant VM"

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	$(CMAKE) --build $(BUILD_DIR)

kernel:
	$(MAKE) -C kernel

load:
	./scripts/load-kernel-module.sh

unload:
	./scripts/unload-kernel-module.sh

smoke:
	./scripts/kernel-smoke-test.sh

verifier: build
	./$(BUILD_DIR)/afl-kernel-verifier

clean:
	$(CMAKE) --build $(BUILD_DIR) --target clean
	$(MAKE) -C kernel clean

vagrant-up:
	vagrant up

vagrant-rsync:
	vagrant rsync

vagrant-smoke: vagrant-rsync
	vagrant ssh -c 'cd /home/vagrant/accelerator-firmware-lab && make smoke'
