#!/bin/sh
# Provision the icon-sdl test VM (runs as root inside it; vm.sh create
# pipes it in). $1 is a snapshot.debian.org timestamp: every package comes
# from the Debian archive exactly as it was then, so a re-provisioned VM
# gets the same versions. The versions installed are written to
# /var/local/packages.lock (copied to sdl/vm/packages.lock).
set -eu
SNAPSHOT=$1
export DEBIAN_FRONTEND=noninteractive

# Pin apt to the snapshot; snapshots are old by design, so don't reject
# their Release files as expired.
rm -f /etc/apt/sources.list
cat > /etc/apt/sources.list.d/debian.sources <<EOF
Types: deb
URIs: http://snapshot.debian.org/archive/debian/$SNAPSHOT/
Suites: trixie trixie-updates
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg

Types: deb
URIs: http://snapshot.debian.org/archive/debian-security/$SNAPSHOT/
Suites: trixie-security
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
EOF
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/99snapshot

# Nothing should change the VM behind our back.
systemctl disable --now apt-daily.timer apt-daily-upgrade.timer 2>/dev/null || true
apt-get -y -q purge unattended-upgrades 2>/dev/null || true

apt-get -q update
apt-get -y -q install --no-install-recommends \
   build-essential git rsync ca-certificates cmake pkg-config python3 \
   xvfb xauth x11-utils x11-apps \
   xfonts-base xfonts-75dpi xfonts-100dpi xfonts-scalable \
   libx11-dev libxpm-dev libxt-dev \
   libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev \
   libxfixes-dev libxtst-dev libxkbcommon-dev \
   libwayland-dev wayland-protocols libdecor-0-dev \
   libfontconfig-dev fonts-dejavu-core

mkdir -p /var/local
dpkg-query -W -f '${Package} ${Version} ${Architecture}\n' | sort > /var/local/packages.lock
echo "provisioned: $(wc -l < /var/local/packages.lock) packages"
