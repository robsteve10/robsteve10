// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>

namespace duo {

// What the bridge is running on, and what that means for streaming.
struct DeviceProfile {
	std::string model;          // e.g. "Raspberry Pi Zero W Rev 1.1", or the host name
	int cores = 1;
	bool lowPower = false;      // single-core ARMv6 boards (Pi Zero W, Pi 1)
	int maxKbps = 0;            // bitrate the board and its radio carry comfortably (0 = no cap)
	int recommendedHeight = 0;  // 0 = no recommendation
	std::string netIface;       // interface with the default route
	bool wireless = false;      // ...and whether it is Wi-Fi
};

DeviceProfile detectDevice();

// Best effort, needs root: CPU governor to "performance" (no clock ramp-up delay on each
// frame) and Wi-Fi power saving off (a sleeping radio adds latency spikes).
void tuneForStreaming(const DeviceProfile &dev);

// Installs this binary as a service that starts at boot (Linux with systemd). Returns an exit code.
int installService(const std::string &dataDir);
int uninstallService();

} // namespace duo
