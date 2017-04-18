#!/bin/sh
# Runs a number of KVM-based VM.
# This script should be run inside tmux.

VM_IMAGE="$HOME/jessie-x64-nfs-2017030114_tcpscaler-20170418.qcow2"
MEM=1024

[ -z "$2" ] && { echo "usage: $0 <ID of first VM> <number of VM>"; echo "Launches a number of VM."; echo "This script must be run inside tmux."; exit 1; }

first_vm="$1"
nb_vm="$2"

for i in $(seq "$first_vm" $((first_vm + nb_vm - 1)))
do
  name="jessie-${i}"
  image="/tmp/${name}.qcow2"
  mac="00:16:3E:84:01:${i}"
  cp "$VM_IMAGE" "$image"
  tmux new-window -n "$name"
  tmux send-keys -t "$name" kvm Space -m Space "$MEM" Space -drive Space file="$image",if=virtio,media=disk Space -net Space nic,model=virtio,macaddr="$mac" Space -net Space tap,ifname=tap${i},script=no Space -nographic Enter exit Enter
done
