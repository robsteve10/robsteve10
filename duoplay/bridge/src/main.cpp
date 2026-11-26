// SPDX-License-Identifier: AGPL-3.0-only
//
// DuoPlay bridge: runs next to your PS5 on the home network, speaks the Remote Play
// protocol to the console (via chiaki-ng's library) and re-serves the stream to a
// browser over WebRTC without re-encoding it.

#include "api.hpp"
#include "device.hpp"
#include "embedded_web.hpp"
#include "console.hpp"
#include "mdns.hpp"
#include "media.hpp"
#include "setup.hpp"
#include "store.hpp"
#include "streamer.hpp"
#include "tls.hpp"
#include "util.hpp"

#include <chiaki/common.h>
#include <httplib.h>
#include <rtc/rtc.hpp>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace duo;

namespace {

std::atomic<bool> g_quit{false};

void onSignal(int) { g_quit = true; }

void usage() {
	fprintf(stderr,
	        "DuoPlay bridge %s - PS5 Remote Play for the browser\n\n"
	        "Usage: duoplay-bridge [options]\n"
	        "  --http-port N        web app over HTTP (default 8080)\n"
	        "  --https-port N       web app over HTTPS, needed for controllers/wake lock (default 8443, 0 = off)\n"
	        "  --udp-port N         the single UDP port all streaming uses (default 9600)\n"
	        "  --bind IP            only use this local address for streaming\n"
	        "  --announce-ip IP     also offer this address to phones (e.g. the host's LAN IP when in Docker)\n"
	        "  --data-dir DIR       where pairing data and certificates live (default %s)\n"
	        "  --web-dir DIR        serve the web app from this folder instead of the built-in copy\n"
	        "  --mdns-name NAME     answer NAME.local on the network (default duoplay; \"\" turns it off)\n"
	        "  --install            set DuoPlay up to start at boot (Linux with systemd, run with sudo);\n"
	        "                       on a tablet such as a Surface RT it also keeps it awake, screen off\n"
	        "  --screen on|off      switch a built-in screen on or off now (sudo)\n"
	        "  --keep-screen-off    keep built-in screens off while running (a tablet used as a server)\n"
	        "  --uninstall          remove that again\n"
	        "  --test-pattern FILE  offer a looping H.264 test stream as a fake console\n"
	        "  --verbose            debug logging\n\n"
	        "Raspberry Pi kit, first-run Wi-Fi setup (run by the kit while its setup hotspot is up):\n"
	        "  --setup-portal       serve the setup page, DHCP and DNS on the hotspot instead of streaming\n"
	        "  --setup-iface IF     hotspot interface (wlan0)    --setup-ip IP     its address (192.168.4.1)\n"
	        "  --setup-scan FILE    `iw dev IF scan` output      --setup-save FILE where to write the network\n"
	        "  --setup-message TXT  note shown on the page       --setup-country CC  preselected country\n"
	        "  --setup-idle SEC     give up (exit 3) after SEC without visitors\n"
	        "  --wifi-txt FILE      turn a hand-written wifi.txt into the --setup-save file, then exit\n"
	        "  --log-file FILE      the file this bridge's output goes to, shown in the app (Settings)\n",
	        DUOPLAY_VERSION, defaultDataDir().c_str());
}

bool isDir(const std::string &p) {
#ifdef _WIN32
	DWORD a = GetFileAttributesA(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
	struct stat st;
	return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

std::string findWebDir() {
	std::string exe = executableDir();
	for (const std::string &cand : {joinPath(exe, "web"), joinPath(exe, "../web"), joinPath(exe, "../../web"),
	                                joinPath(exe, "../share/duoplay/web"), std::string("web"), std::string("../web")}) {
		if (isDir(cand)) {
			std::string index;
			if (readFile(joinPath(cand, "index.html"), index))
				return cand;
		}
	}
	return "web";
}

} // namespace

int main(int argc, char **argv) {
	int httpPort = 8080, httpsPort = 8443;
	NetworkConfig net;
	net.udpPort = 9600;
	std::string dataDir = defaultDataDir();
	std::string webDir;
	std::string testPattern;
	std::string mdnsName = "duoplay";
	bool setupPortal = false;
	SetupPortalOptions setup;
	std::string wifiTxt, logFile;
	bool screenOff = false;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc) {
				fprintf(stderr, "%s needs a value\n", a.c_str());
				exit(2);
			}
			return argv[++i];
		};
		if (a == "--http-port")
			httpPort = std::stoi(next());
		else if (a == "--https-port")
			httpsPort = std::stoi(next());
		else if (a == "--udp-port")
			net.udpPort = uint16_t(std::stoi(next()));
		else if (a == "--bind")
			net.bindAddress = next();
		else if (a == "--announce-ip")
			net.announceIps.push_back(next());
		else if (a == "--data-dir")
			dataDir = next();
		else if (a == "--web-dir")
			webDir = next();
		else if (a == "--test-pattern")
			testPattern = next();
		else if (a == "--mdns-name")
			mdnsName = next();
		else if (a == "--screen") {
			std::string v = next();
			if (v != "on" && v != "off") {
				fprintf(stderr, "--screen wants on or off\n");
				return 2;
			}
			int n = setScreen(v == "on");
			printf(n ? "Screen %s.\n" : "No built-in screen found (or not allowed: run it with sudo).\n", v.c_str());
			return n ? 0 : 1;
		} else if (a == "--keep-screen-off")
			screenOff = true;
		else if (a == "--wifi-txt")
			wifiTxt = next();
		else if (a == "--log-file")
			logFile = next();
		else if (a == "--setup-portal")
			setupPortal = true;
		else if (a == "--setup-iface")
			setup.iface = next();
		else if (a == "--setup-ip")
			setup.ip = next();
		else if (a == "--setup-scan")
			setup.scanFile = next();
		else if (a == "--setup-save")
			setup.saveTo = next();
		else if (a == "--setup-message")
			setup.message = next();
		else if (a == "--setup-country")
			setup.country = next();
		else if (a == "--setup-idle")
			setup.idleTimeoutSec = std::stoi(next());
		else if (a == "--setup-ports") { // HTTP,DNS,DHCP - for testing without root
			std::string v = next();
			if (sscanf(v.c_str(), "%d,%d,%d", &setup.httpPort, &setup.dnsPort, &setup.dhcpPort) != 3) {
				fprintf(stderr, "--setup-ports wants HTTP,DNS,DHCP\n");
				return 2;
			}
		}
		else if (a == "--install")
			return installService(dataDir == defaultDataDir() ? "/var/lib/duoplay" : dataDir);
		else if (a == "--uninstall")
			return uninstallService();
		else if (a == "--verbose" || a == "-v")
			setLogLevel(LogLevel::Debug);
		else if (a == "--help" || a == "-h") {
			usage();
			return 0;
		} else {
			fprintf(stderr, "Unknown option %s\n\n", a.c_str());
			usage();
			return 2;
		}
	}
	// The web app is compiled in; --web-dir serves a folder instead (handy while developing it).
	bool embedded = webDir.empty() && kEmbeddedWebCount > 0;
	if (webDir.empty() && !embedded)
		webDir = findWebDir();

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);
#ifndef _WIN32
	std::signal(SIGPIPE, SIG_IGN);
#endif
	if (!wifiTxt.empty()) {
		std::string err;
		if (setup.saveTo.empty() || !wifiFromTxt(wifiTxt, setup.saveTo, err)) {
			LOGE("setup", "wifi.txt: %s", setup.saveTo.empty() ? "needs --setup-save" : err.c_str());
			return 1;
		}
		return 0;
	}
	if (setupPortal)
		return runSetupPortal(setup, g_quit);

	if (chiaki_lib_init() != CHIAKI_ERR_SUCCESS) {
		LOGE("main", "Could not initialise the Remote Play library");
		return 1;
	}
	rtc::InitLogger(logLevel() == LogLevel::Debug ? rtc::LogLevel::Info : rtc::LogLevel::Warning,
	                [](rtc::LogLevel level, std::string msg) {
		                if (level <= rtc::LogLevel::Error)
			                LOGE("webrtc", "%s", msg.c_str());
		                else if (level <= rtc::LogLevel::Warning)
			                LOGW("webrtc", "%s", msg.c_str());
		                else
			                LOGD("webrtc", "%s", msg.c_str());
	                });

	DeviceProfile device = detectDevice();
	LOGI("main", "Running on %s (%d core%s%s%s)", device.model.c_str(), device.cores, device.cores == 1 ? "" : "s",
	     device.wireless ? ", Wi-Fi " : "", device.wireless ? device.netIface.c_str() : "");
	if (device.maxKbps)
		LOGI("main", "This board streams best at %dp, up to %d kbps; the app defaults to that", device.recommendedHeight, device.maxKbps);
	tuneForStreaming(device);

	Store store(dataDir);
	ConsoleManager consoles(store);
	MediaHub hub;
	SessionManager sessions(store, consoles, hub, testPattern);
	consoles.start();

	std::vector<std::string> ips;
	for (const auto &a : localIPv4Addresses())
		ips.push_back(a.ip);
	for (const auto &ip : net.announceIps)
		ips.push_back(ip);

	ApiContext ctx{store, consoles, sessions, hub, net, webDir, {}, false, httpPort, httpsPort, device, logFile};
	if (httpsPort > 0) {
		std::string err;
		if (ensureTls(dataDir, ips, hostName(), ctx.tls, err))
			ctx.tlsEnabled = true;
		else
			LOGE("tls", "HTTPS disabled: %s", err.c_str());
	}

	httplib::Server http;
	std::unique_ptr<httplib::SSLServer> https;
	if (ctx.tlsEnabled) {
		https = std::make_unique<httplib::SSLServer>(ctx.tls.certPem.c_str(), ctx.tls.keyPem.c_str());
		if (!https->is_valid()) {
			LOGE("tls", "Could not load the HTTPS certificate; HTTPS disabled");
			https.reset();
			ctx.tlsEnabled = false;
		}
	}
	Api api(ctx);
	api.install(http);
	if (https)
		api.install(*https);

	for (auto *s : {&http, static_cast<httplib::Server *>(https.get())}) {
		if (!s)
			continue;
		s->new_task_queue = [] { return new httplib::ThreadPool(16); };
		s->set_keep_alive_max_count(1000);
		s->set_read_timeout(40);
		s->set_write_timeout(40);
	}

	if (!http.bind_to_port("0.0.0.0", httpPort)) {
		LOGE("main", "Port %d is taken; pick another with --http-port", httpPort);
		return 1;
	}
	if (https && !https->bind_to_port("0.0.0.0", httpsPort)) {
		LOGE("main", "Port %d is taken; pick another with --https-port (or 0 to disable HTTPS)", httpsPort);
		https.reset();
	}
	std::thread httpThread([&] { http.listen_after_bind(); });
	MdnsResponder mdns({mdnsName});
	mdns.start();
	std::atomic<bool> screenKeeperRunning{screenOff};
	std::thread screenKeeper;
	if (screenOff)
		screenKeeper = std::thread([&] { keepScreenOff(screenKeeperRunning); });
	std::thread httpsThread;
	if (https)
		httpsThread = std::thread([&] { https->listen_after_bind(); });

	fprintf(stderr, "\n  DuoPlay bridge %s is running.\n\n  Open on your iPhone (same Wi-Fi):\n", DUOPLAY_VERSION);
#ifdef __linux__
	if (!mdnsName.empty())
		fprintf(stderr, "    http://%s.local:%d\n", mdnsName.c_str(), httpPort);
#endif
	auto addrs = localIPv4Addresses();
	for (const auto &a : addrs) {
		if (https)
			fprintf(stderr, "    https://%s:%d   (recommended, after installing the certificate)\n", a.ip.c_str(), httpsPort);
		fprintf(stderr, "    http://%s:%d\n", a.ip.c_str(), httpPort);
	}
	if (addrs.empty())
		fprintf(stderr, "    (no network address found)\n");
	fprintf(stderr, "\n  Streaming uses UDP port %u. Web app: %s\n  Data: %s\n\n", net.udpPort, embedded ? "built in" : webDir.c_str(),
	        dataDir.c_str());

	while (!g_quit)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	LOGI("main", "Shutting down");
	screenKeeperRunning = false;
	if (screenKeeper.joinable())
		screenKeeper.join();
	mdns.stop();
	http.stop();
	if (https)
		https->stop();
	if (httpThread.joinable())
		httpThread.join();
	if (httpsThread.joinable())
		httpsThread.join();
	api.closeAllPeers();
	sessions.stop();
	consoles.stop();
	return 0;
}
