Vagrant.configure("2") do |config|
  config.vm.define "accelerator-firmware-lab" do |vm|
    # ARM64 Ubuntu guest image. The lab is intended to run kernel-module,
    # driver-interface, and later QEMU experiments inside this VM.
    vm.vm.box = "perk/ubuntu-25.04-arm64"
    vm.vm.box_version = "20250424"
    vm.vm.box_architecture = "arm64"

    # This is the Vagrant host-side provider. It tells Vagrant to boot this VM
    # with QEMU on the host machine. It is separate from the QEMU packages that
    # are installed inside the guest below.
    vm.vm.provider "qemu" do |qemu|
      qemu.ssh_port = "50023"
      qemu.memory = "4096"
      qemu.cpus = 2
    end

    # Keep the project source on the host and sync it into the guest. rsync keeps
    # kernel build artifacts and CMake outputs out of the shared source tree.
    vm.vm.synced_folder ".", "/home/vagrant/accelerator-firmware-lab",
      type: "rsync",
      rsync__args: ["--verbose", "--archive", "--delete", "-z", "--exclude", "artifacts/"],
      rsync__exclude: ["build/", "artifacts/", ".vagrant/", ".DS_Store"],
      rsync__auto: true

    vm.vm.provision "shell", inline: <<-SHELL
      set -eux

      # Base package index refresh before installing the lab toolchain.
      sudo apt-get update

      # Core development stack:
      # - C/C++ build and debug tools for the userspace simulator and tests.
      # - Linux headers/tools for building the out-of-tree afl_kernel.ko module.
      # - qemu-system/qemu-utils inside the guest for future nested QEMU or disk
      #   image experiments. These packages are not QEMU source code and are not
      #   required for the current phase-2 misc/char driver backend.
      sudo apt-get install -y \
        ca-certificates apt-transport-https software-properties-common \
        build-essential git curl wget vim htop unzip plocate gnupg lsb-release \
        pkg-config cmake ninja-build meson clang llvm lldb gdb valgrind \
        python3 python3-pip python3-venv \
        qemu-system qemu-utils \
        zlib1g-dev libelf-dev libzstd-dev libseccomp-dev \
        linux-headers-$(uname -r) linux-tools-$(uname -r) linux-tools-common \
        bsdutils strace ltrace systemtap-sdt-dev protobuf-compiler

      sudo -u vagrant mkdir -p /home/vagrant/workspace

      # Convenience environment variable for shell sessions inside the VM.
      grep -qxF 'export AFL_WORKSPACE=$HOME/accelerator-firmware-lab' /home/vagrant/.bashrc || echo 'export AFL_WORKSPACE=$HOME/accelerator-firmware-lab' >> /home/vagrant/.bashrc

      cd /tmp

      # Install Go manually so the VM has a predictable version independent of
      # the Ubuntu package repository.
      GO_VERSION="1.24.2"
      if [ ! -x /usr/local/go/bin/go ]; then
        wget -q https://go.dev/dl/go${GO_VERSION}.linux-arm64.tar.gz
        sudo rm -rf /usr/local/go
        sudo tar -C /usr/local -xzf go${GO_VERSION}.linux-arm64.tar.gz
        rm go${GO_VERSION}.linux-arm64.tar.gz
      fi
      grep -qxF 'export PATH=$PATH:/usr/local/go/bin' /home/vagrant/.bashrc || echo 'export PATH=$PATH:/usr/local/go/bin' >> /home/vagrant/.bashrc
      grep -qxF 'export GOPATH=$HOME/go' /home/vagrant/.bashrc || echo 'export GOPATH=$HOME/go' >> /home/vagrant/.bashrc
      grep -qxF 'export PATH=$PATH:$GOPATH/bin' /home/vagrant/.bashrc || echo 'export PATH=$PATH:$GOPATH/bin' >> /home/vagrant/.bashrc

      # Docker is useful for auxiliary tooling and experiments. The user is
      # added to the docker group; a new login session is required before that
      # group membership is visible.
      sudo mkdir -p /etc/apt/keyrings
      if [ ! -f /etc/apt/keyrings/docker.gpg ]; then
        curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
      fi
      echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
      sudo apt-get update
      sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
      sudo usermod -aG docker vagrant
    SHELL
  end
end
