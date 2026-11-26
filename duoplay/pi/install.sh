#!/bin/sh
# Installs the DuoPlay bridge on a Surface RT or Raspberry Pi running Raspberry Pi OS (or another
# ARM Linux with systemd), as a service that starts with it. On a tablet such as the Surface RT
# it also makes it a server: it never sleeps, and its screen goes off. In a terminal on the
# device, or from an SSH app on the iPhone:
#
#   curl -fsSL https://raw.githubusercontent.com/robsteve10/robsteve10/claude/festive-carson-vg2aw4/duoplay/pi/install.sh | sudo sh
#
# (A Pi with nothing on its card: use the SD-card kit instead, see README.)
set -eu

BASE=${DUOPLAY_BASE:-https://raw.githubusercontent.com/robsteve10/robsteve10/claude/festive-carson-vg2aw4/duoplay/pi}

case "$(uname -m)" in
armv6l | armv7l | armv8l | aarch64) ;;
*)
	echo "This is for ARM devices such as a Raspberry Pi or Surface RT. This computer is $(uname -m)." >&2
	exit 1
	;;
esac
if [ "$(id -u)" != 0 ]; then
	echo "Run it with sudo:  curl -fsSL $BASE/install.sh | sudo sh" >&2
	exit 1
fi
if [ "$(uname -m)" = aarch64 ] && [ "$(getconf PAGESIZE 2>/dev/null || echo 4096)" != 4096 ]; then
	# A Pi 5's default kernel uses 16K pages, which can't run 32-bit programs.
	echo "This Pi's kernel can't run 32-bit programs. Add the line  kernel=kernel8.img  to" >&2
	echo "/boot/firmware/config.txt, reboot, and run this again." >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fetch() {
	if command -v curl >/dev/null 2>&1; then curl -fsSL -o "$2" "$1"; else wget -q -O "$2" "$1"; fi
}

echo "Downloading DuoPlay..."
fetch "$BASE/duoplay-bridge-armv6" "$tmp/duoplay-bridge"
fetch "$BASE/duoplay-bridge-armv6.sha256" "$tmp/sum"
if [ "$(cut -d' ' -f1 "$tmp/sum")" != "$(sha256sum "$tmp/duoplay-bridge" | cut -d' ' -f1)" ]; then
	echo "The download came through damaged. Run the command again." >&2
	exit 1
fi
chmod 755 "$tmp/duoplay-bridge"
"$tmp/duoplay-bridge" --install
