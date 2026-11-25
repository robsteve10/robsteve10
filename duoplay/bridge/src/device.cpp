// SPDX-License-Identifier: AGPL-3.0-only
#include "device.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace duo {

static std::string readTrimmed(const std::string &path) {
	std::string s;
	if (!readFile(path, s))
		return "";
	while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == ' '))
		s.pop_back();
	return s;
}

static bool exists(const std::string &p) {
#ifdef _WIN32
	return false;
#else
	struct stat st;
	return stat(p.c_str(), &st) == 0;
#endif
}

DeviceProfile detectDevice() {
	DeviceProfile d;
	d.cores = int(std::max(1u, std::thread::hardware_concurrency()));
	d.model = readTrimmed("/proc/device-tree/model");
	if (d.model.empty())
		d.model = hostName();

	// The interface carrying the default route: first entry in /proc/net/route with destination 0.
	std::ifstream routes("/proc/net/route");
	std::string line;
	std::getline(routes, line); // header
	while (std::getline(routes, line)) {
		std::istringstream ss(line);
		std::string iface, dest;
		ss >> iface >> dest;
		if (dest == "00000000") {
			d.netIface = iface;
			break;
		}
	}
	if (!d.netIface.empty())
		d.wireless = exists("/sys/class/net/" + d.netIface + "/wireless");

	const std::string &m = d.model;
	bool pi = m.find("Raspberry Pi") != std::string::npos;
	if (pi && m.find("Zero 2") != std::string::npos) {
		// Four cores are plenty, but the radio is a single 2.4 GHz stream that carries the
		// picture twice (console -> Pi, Pi -> phone).
		d.maxKbps = 8000;
		d.recommendedHeight = 720;
	} else if (pi && (m.find("Zero") != std::string::npos || m.find("Model A Rev") != std::string::npos ||
	                  m.find("Model B Rev") != std::string::npos || m.find("Model B Plus") != std::string::npos ||
	                  m.find("Model A Plus") != std::string::npos || m.find("Compute Module Rev") != std::string::npos)) {
		// Pi Zero / Zero W / Pi 1: one ARMv6 core, and on the Zero W one 2.4 GHz radio.
		d.lowPower = true;
		d.maxKbps = 6000;
		d.recommendedHeight = 720;
	}
	return d;
}

void tuneForStreaming(const DeviceProfile &dev) {
#ifndef _WIN32
	if (geteuid() != 0)
		return;
	int tuned = 0;
	for (int cpu = 0; cpu < 64; cpu++) {
		std::string p = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/scaling_governor";
		if (!exists(p))
			break;
		std::ofstream f(p);
		f << "performance";
		tuned += f.good() ? 1 : 0;
	}
	if (tuned)
		LOGI("device", "CPU governor set to performance on %d core(s)", tuned);
	if (dev.wireless && !dev.netIface.empty()) {
		for (const char *iw : {"/usr/sbin/iw", "/sbin/iw", "/usr/bin/iw"}) {
			if (!exists(iw))
				continue;
			std::string cmd = std::string(iw) + " dev " + dev.netIface + " set power_save off >/dev/null 2>&1";
			if (system(cmd.c_str()) == 0)
				LOGI("device", "Wi-Fi power saving off on %s", dev.netIface.c_str());
			break;
		}
	}
#endif
}

#ifndef _WIN32
static bool writeText(const std::string &path, const std::string &text) {
	std::ofstream f(path, std::ios::trunc);
	f << text;
	return f.good();
}
#endif

int installService(const std::string &dataDir) {
#ifdef _WIN32
	fprintf(stderr, "--install is for Linux (Raspberry Pi OS and similar).\n");
	return 1;
#else
	if (geteuid() != 0) {
		fprintf(stderr, "Installing needs administrator rights. Run it again with sudo in front.\n");
		return 1;
	}
	if (!exists("/run/systemd/system")) {
		fprintf(stderr, "This system doesn't use systemd; start duoplay-bridge some other way.\n");
		return 1;
	}
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (n <= 0) {
		fprintf(stderr, "Can't find this program's own file.\n");
		return 1;
	}
	self[n] = 0;
	const std::string target = "/usr/local/bin/duoplay-bridge";
	if (target != self) {
		std::string cmd = "install -m 0755 '" + std::string(self) + "' " + target + ".new && mv -f " + target + ".new " + target;
		if (system(cmd.c_str()) != 0) {
			fprintf(stderr, "Couldn't copy the program to %s.\n", target.c_str());
			return 1;
		}
	}
	makeDirs(dataDir);
	std::string unit =
	    "[Unit]\n"
	    "Description=DuoPlay bridge (PS5 Remote Play for iPhone Duo)\n"
	    "After=network-online.target\n"
	    "Wants=network-online.target\n\n"
	    "[Service]\n"
	    "ExecStart=" + target + " --data-dir " + dataDir + "\n"
	    "Restart=always\n"
	    "RestartSec=2\n"
	    "Nice=-10\n"
	    "IOSchedulingClass=realtime\n\n"
	    "[Install]\n"
	    "WantedBy=multi-user.target\n";
	if (!writeText("/etc/systemd/system/duoplay.service", unit)) {
		fprintf(stderr, "Couldn't write the service file.\n");
		return 1;
	}
	// NetworkManager (Raspberry Pi OS Bookworm and later) turns Wi-Fi power saving back on
	// at every reconnect unless told not to.
	if (exists("/etc/NetworkManager/conf.d"))
		writeText("/etc/NetworkManager/conf.d/duoplay-wifi-powersave.conf", "[connection]\nwifi.powersave = 2\n");
	if (system("systemctl daemon-reload && systemctl enable duoplay.service >/dev/null 2>&1 && systemctl restart duoplay.service") != 0) {
		fprintf(stderr, "Installed, but systemd couldn't start it. Check: systemctl status duoplay\n");
		return 1;
	}
	std::string host = hostName();
	printf("\n  DuoPlay is installed and starts automatically whenever the Pi powers on.\n\n"
	       "  On your iPhone, open:  http://duoplay.local:8080\n");
	if (host != "duoplay")
		printf("            or:         http://%s.local:8080\n", host.c_str());
	for (const auto &a : localIPv4Addresses())
		printf("            or:         http://%s:8080\n", a.ip.c_str());
	printf("\n  Logs: journalctl -u duoplay -f      Remove: sudo duoplay-bridge --uninstall\n\n");
	return 0;
#endif
}

int uninstallService() {
#ifdef _WIN32
	return 1;
#else
	if (geteuid() != 0) {
		fprintf(stderr, "Run it again with sudo in front.\n");
		return 1;
	}
	int rc = system("systemctl disable --now duoplay.service >/dev/null 2>&1; "
	                "rm -f /etc/systemd/system/duoplay.service /etc/NetworkManager/conf.d/duoplay-wifi-powersave.conf; "
	                "systemctl daemon-reload");
	printf("DuoPlay's service is removed. The program stays at /usr/local/bin/duoplay-bridge and your pairing data in "
	       "/var/lib/duoplay; delete them if you like.\n");
	return rc == 0 ? 0 : 1;
#endif
}

} // namespace duo
