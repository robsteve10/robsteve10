#!/usr/bin/env bash
# Builds the DuoPlay bridge into ./dist (Linux, Raspberry Pi OS, macOS, WSL2).
set -euo pipefail
cd "$(dirname "$0")/.."

missing=()
for tool in cmake git c++ protoc python3; do
	command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
python3 -c 'import google.protobuf' 2>/dev/null || missing+=("python3 protobuf module")
if ((${#missing[@]})); then
	echo "Missing: ${missing[*]}"
	echo
	echo "Install the build tools first:"
	echo "  Debian / Ubuntu / Raspberry Pi OS / WSL2:"
	echo "    sudo apt install build-essential cmake git pkg-config libssl-dev protobuf-compiler python3-protobuf"
	echo "  Fedora:  sudo dnf install gcc-c++ cmake git openssl-devel protobuf-compiler python3-protobuf"
	echo "  Arch:    sudo pacman -S base-devel cmake git openssl protobuf python-protobuf"
	echo "  macOS:   brew install cmake openssl@3 protobuf && python3 -m pip install --user protobuf"
	exit 1
fi

extra=()
if [[ "$(uname)" == "Darwin" ]] && command -v brew >/dev/null; then
	extra+=("-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)")
fi

jobs=$( (nproc || sysctl -n hw.ncpu || echo 4) 2>/dev/null | head -1)
cmake -S bridge -B build -DCMAKE_BUILD_TYPE=Release -DPython3_EXECUTABLE="$(command -v python3)" "${extra[@]}"
cmake --build build -j"$jobs" --target duoplay-bridge

mkdir -p dist
cp build/duoplay-bridge dist/
rm -rf dist/web
cp -R web dist/web
echo
echo "Built: dist/duoplay-bridge"
echo "Run:   ./dist/duoplay-bridge"
