#!/usr/bin/env python3
import os
import sys
import subprocess
from time import time

shared_dir = sys.argv[1]

# Starting VirtioFSD
os.system("unlink /tmp/virtiofsd.sock")
os.system("unlink /tmp/virtiofsd.sock.pid")
virtiofsd_args = ["virtiofsd", "--allow-direct-io", "--cache", "never", "--socket-path", "/tmp/virtiofsd.sock", "--shared-dir", shared_dir]
virtiofsd_proc = subprocess.Popen(virtiofsd_args)

start = time()
while not os.path.exists("/tmp/virtiofsd.sock"):
    if time() - start > 5:
        raise Exception("VirtioFSD failure!")

# Starting Linux VM
qemu_args = ["sudo", "qemu-system-x86_64", "-enable-kvm", "-machine", "memory-backend=mem0"]
qemu_args += ["-m", "8G", "-object", "memory-backend-memfd,id=mem0,share=on,size=8G"]
qemu_args += ["-smp", "4"]
qemu_args += ["-chardev", "socket,id=char0,path=/tmp/virtiofsd.sock"]
qemu_args += ["-device", "vhost-user-fs-pci,chardev=char0,tag=virtiofs0"]
qemu_args += ["-drive", "file=./ubuntu_server/ubuntu24.04.qcow2,format=qcow2"]

subprocess.run(qemu_args)
