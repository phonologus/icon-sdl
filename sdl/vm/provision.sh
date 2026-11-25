#!/bin/sh
# Provision the icon-sdl test VM (runs as root inside it; vm.sh pipes it
# in). $1 is a snapshot.debian.org timestamp: every package comes from
# the Debian archive exactly as it was then, so a re-provisioned VM gets
# the same versions. The remaining arguments are the packages to install
# (vm.sh takes them from sdl/linux-packages.txt, plus its test tools).
# The versions installed are written to /var/local/packages.lock.
set -eu
SNAPSHOT=$1
shift
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
apt-get -y -q install --no-install-recommends "$@"

mkdir -p /var/local
dpkg-query -W -f '${Package} ${Version} ${Architecture}\n' | sort > /var/local/packages.lock
echo "provisioned: $(wc -l < /var/local/packages.lock) packages"
