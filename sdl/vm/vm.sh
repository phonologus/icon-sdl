#!/bin/sh
# A headless Debian 13 (arm64) VM under QEMU + macOS Hypervisor.framework,
# for building the X11 reference icont and testing the Linux SDL backend.
#
#   sdl/vm/vm.sh create     download the pinned image, make the VM, provision it
#   sdl/vm/vm.sh start      boot it (in the background) and wait for SSH
#   sdl/vm/vm.sh stop       shut it down cleanly
#   sdl/vm/vm.sh status     running or not
#   sdl/vm/vm.sh ssh [CMD]  log in, or run CMD
#   sdl/vm/vm.sh destroy    stop it and delete its disk (keeps the base image)
#   sdl/vm/vm.sh provision  install (or update) its packages again
#
# The packages are those a user installs (sdl/linux-packages.txt) plus
# the test VM's own tools (TESTPKGS: Xvfb, X fonts and headers for the
# X11 reference build, Python for the test scripts). With
# ICONSDL_VM_MINIMAL=1, none: only the pinned package sources, for
# following the README's Linux instructions from nothing. Use
# ICONSDL_VM_STATE and ICONSDL_VM_PORT to keep such a VM apart.
#
# Everything the VM needs is pinned: the Debian cloud image by SHA-512,
# and its packages through snapshot.debian.org at a fixed date (see
# provision.sh). Only `create` uses the network. The state (images, disk,
# SSH key) lives in sdl/vm/state, which git ignores, or in
# $ICONSDL_VM_STATE.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
S=${ICONSDL_VM_STATE:-$here/state}
PORT=${ICONSDL_VM_PORT:-2222}

IMG_BUILD=20261116-2664
IMG=debian-13-genericcloud-arm64-$IMG_BUILD.qcow2
IMG_URL=https://cloud.debian.org/images/cloud/trixie/$IMG_BUILD/$IMG
IMG_SHA512=ecbb14df864d2f8842539733c10d7251c58294f86b85a487b23b2c959a9b75f003df6b289deb20e1aee03ef65de2b1429818689e1fbc4b727d579c61b247fee9
SNAPSHOT=20261116T000000Z

TESTPKGS="git rsync ca-certificates pkg-config python3 xvfb xauth x11-utils
   x11-apps xfonts-base xfonts-75dpi xfonts-100dpi xfonts-scalable
   libxpm-dev libxt-dev fonts-dejavu-core weston libdecor-0-plugin-1-cairo"

FW=$(cd "$(dirname "$(command -v qemu-system-aarch64)")/../share/qemu" && pwd)

die() { echo "vm.sh: $*" >&2; exit 1; }

ssh_vm() {
   ssh -i "$S/id_ed25519" -p "$PORT" \
      -o UserKnownHostsFile="$S/known_hosts" -o StrictHostKeyChecking=accept-new \
      -o LogLevel=ERROR -o ConnectTimeout=5 icon@127.0.0.1 "$@"
}

running() {
   [ -f "$S/vm.pid" ] && kill -0 "$(cat "$S/vm.pid")" 2>/dev/null
}

wait_ssh() {
   i=0
   until ssh_vm true 2>/dev/null; do
      i=$((i + 1))
      [ $i -lt 90 ] || die "no SSH after 3 minutes; see $S/console.log"
      running || die "VM exited; see $S/console.log"
      sleep 2
   done
}

start() {
   running && { echo "already running"; return; }
   [ -f "$S/disk.qcow2" ] || die "no VM; run: $0 create"
   qemu-system-aarch64 \
      -machine virt -accel hvf -cpu host -smp 4 -m 4096 \
      -drive if=pflash,format=raw,readonly=on,file="$FW/edk2-aarch64-code.fd" \
      -drive if=pflash,format=raw,file="$S/efivars.fd" \
      -drive if=virtio,format=qcow2,file="$S/disk.qcow2" \
      -drive if=virtio,format=raw,readonly=on,file="$S/seed.iso" \
      -netdev user,id=n0,hostfwd=tcp:127.0.0.1:"$PORT"-:22 \
      -device virtio-net-pci,netdev=n0 \
      -display none -serial file:"$S/console.log" \
      -daemonize -pidfile "$S/vm.pid"
   wait_ssh
   echo "VM up: $0 ssh"
}

stop() {
   running || { echo "not running"; return; }
   ssh_vm sudo systemctl poweroff 2>/dev/null || true
   i=0
   while running; do
      i=$((i + 1))
      if [ $i -gt 60 ]; then kill "$(cat "$S/vm.pid")"; break; fi
      sleep 1
   done
   rm -f "$S/vm.pid"
   echo "stopped"
}

create() {
   [ -f "$S/disk.qcow2" ] && die "VM exists; '$0 destroy' first"
   mkdir -p "$S"

   # The pinned base image, verified.
   if [ ! -f "$S/$IMG" ]; then
      echo "downloading $IMG_URL"
      curl -fL --progress-bar -o "$S/$IMG.part" "$IMG_URL"
      mv "$S/$IMG.part" "$S/$IMG"
   fi
   echo "$IMG_SHA512  $S/$IMG" | shasum -a 512 -c --status \
      || die "checksum mismatch for $S/$IMG"

   # A key for this VM only.
   [ -f "$S/id_ed25519" ] || ssh-keygen -q -t ed25519 -N '' -C icon-sdl-vm -f "$S/id_ed25519"
   rm -f "$S/known_hosts"

   # cloud-init seed: the user and its key.
   rm -rf "$S/seed" "$S/seed.iso"
   mkdir "$S/seed"
   sed "s|@KEY@|$(cat "$S/id_ed25519.pub")|" "$here/user-data.in" > "$S/seed/user-data"
   printf 'instance-id: icon-sdl-vm\nlocal-hostname: icon-sdl-vm\n' > "$S/seed/meta-data"
   hdiutil makehybrid -quiet -o "$S/seed.iso" "$S/seed" -iso -joliet \
      -default-volume-name cidata

   # A copy-on-write disk over the pristine image, and UEFI variables.
   qemu-img create -q -f qcow2 -b "$IMG" -F qcow2 "$S/disk.qcow2" 20G
   cp "$FW/edk2-arm-vars.fd" "$S/efivars.fd"

   start
   provision
}

provision() {
   pkgs=$(grep -v '^#' "$here/../linux-packages.txt" | tr '\n' ' ')
   if [ "${ICONSDL_VM_MINIMAL:-}" = 1 ]; then
      pkgs=""
      lock="$S/packages.lock"
   else
      pkgs="$pkgs $(echo $TESTPKGS)"
      lock="$here/packages.lock"
   fi
   echo "provisioning (packages from snapshot.debian.org at $SNAPSHOT): $pkgs"
   ssh_vm "sudo sh -s $SNAPSHOT $pkgs" < "$here/provision.sh"
   ssh_vm cat /var/local/packages.lock > "$lock"
   echo "VM ready; installed package versions recorded in $lock"
}

destroy() {
   stop
   rm -f "$S/disk.qcow2" "$S/efivars.fd" "$S/seed.iso" "$S/known_hosts" "$S/console.log"
   rm -rf "$S/seed"
   echo "destroyed (base image kept in $S)"
}

case "${1:-}" in
create) create ;;
start) start ;;
stop) stop ;;
status) if running; then echo running; else echo stopped; fi ;;
ssh) shift; ssh_vm "$@" ;;
provision) provision ;;
destroy) destroy ;;
*) sed -n '2,22p' "$0" >&2; exit 2 ;;
esac
