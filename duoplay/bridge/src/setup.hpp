// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace duo {

// First-run Wi-Fi setup for the Raspberry Pi kit.
//
// With no Wi-Fi it can join, the Pi turns its radio into a "DuoPlay Setup" hotspot and runs
// `duoplay-bridge --setup-portal`. Joining that network from the iPhone brings up the setup
// page on its own (the captive-portal sheet hotel Wi-Fi uses): this serves the page plus the
// two small services a hotspot needs, DHCP to give the phone an address and DNS that answers
// every name with the Pi, which is what makes iOS show the sheet.
struct SetupPortalOptions {
	std::string iface;      // the hotspot's interface, e.g. wlan0 ("" = all)
	std::string ip = "192.168.4.1";
	std::string scanFile;   // `iw dev <iface> scan` output captured before the hotspot came up
	std::string saveTo;     // wpa_supplicant configuration written when the user picks a network
	std::string message;    // shown above the form, e.g. why setup is showing again
	std::string country;    // Wi-Fi country to preselect
	int httpPort = 80;
	int dnsPort = 53;
	int dhcpPort = 67;      // clients are answered on dhcpPort + 1
	int idleTimeoutSec = 0; // stop (exit code 3) after this long with nobody using it; 0 = never
};

// Runs until a network is saved (returns 0), the idle timeout passes (3), `quit` is set (130),
// or something fails to start (1).
int runSetupPortal(const SetupPortalOptions &opts, const std::atomic<bool> &quit);

struct ScannedNetwork {
	std::string ssid; // raw bytes as broadcast
	int signal = -100; // dBm
	bool secure = false;
	int freq = 0;     // MHz
};
// Networks from `iw dev wlan0 scan` output: one entry per name, strongest first.
std::vector<ScannedNetwork> parseIwScan(const std::string &text);

// The wpa_supplicant configuration for one network. Empty password = open network.
std::string wpaConfigFor(const std::string &ssid, const std::string &password, const std::string &country);

// Reads a hand-written wifi.txt on the card ("name: ...", "password: ...", "country: ..." lines)
// and saves it as `saveTo`, like the setup page would.
bool wifiFromTxt(const std::string &path, const std::string &saveTo, std::string &err);

// Checks what the setup page sent; returns an error message for the user, or "" if fine.
std::string validateWifi(const std::string &ssid, const std::string &password, const std::string &country);

} // namespace duo
