// SPDX-License-Identifier: AGPL-3.0-only
//
// DuoPlay bridge: runs next to your PS5 on the home network, speaks the Remote Play
// protocol to the console (via chiaki-ng's library) and re-serves the stream to a
// browser over WebRTC without re-encoding it.

#include "api.hpp"
#include "console.hpp"
#include "media.hpp"
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
	        "  --web-dir DIR        where the web app is (default: next to the program)\n"
	        "  --test-pattern FILE  offer a looping H.264 test stream as a fake console\n"
	        "  --verbose            debug logging\n",
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
	if (webDir.empty())
		webDir = findWebDir();

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);
#ifndef _WIN32
	std::signal(SIGPIPE, SIG_IGN);
#endif

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

	ApiContext ctx{store, consoles, sessions, hub, net, webDir, {}, false, httpPort, httpsPort};
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
	std::thread httpsThread;
	if (https)
		httpsThread = std::thread([&] { https->listen_after_bind(); });

	fprintf(stderr, "\n  DuoPlay bridge %s is running.\n\n  Open on your iPhone (same Wi-Fi):\n", DUOPLAY_VERSION);
	auto addrs = localIPv4Addresses();
	for (const auto &a : addrs) {
		if (https)
			fprintf(stderr, "    https://%s:%d   (recommended, after installing the certificate)\n", a.ip.c_str(), httpsPort);
		fprintf(stderr, "    http://%s:%d\n", a.ip.c_str(), httpPort);
	}
	if (addrs.empty())
		fprintf(stderr, "    (no network address found)\n");
	fprintf(stderr, "\n  Streaming uses UDP port %u. Web app: %s\n  Data: %s\n\n", net.udpPort, webDir.c_str(),
	        dataDir.c_str());

	while (!g_quit)
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	LOGI("main", "Shutting down");
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
