// SPDX-License-Identifier: AGPL-3.0-only
#include "setup.hpp"
#include "embedded_web.hpp"
#include "util.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace duo {

// ---------------------------------------------------------------------------------------------
// Scan results and the configuration file

static std::string unescapeIw(const std::string &s) {
	// iw prints unprintable bytes (and leading/trailing spaces, backslashes) as \xNN.
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && isxdigit(static_cast<unsigned char>(s[i + 2])) &&
		    isxdigit(static_cast<unsigned char>(s[i + 3]))) {
			out += char(std::stoi(s.substr(i + 2, 2), nullptr, 16));
			i += 3;
		} else {
			out += s[i];
		}
	}
	return out;
}

std::vector<ScannedNetwork> parseIwScan(const std::string &text) {
	std::vector<ScannedNetwork> all;
	ScannedNetwork cur;
	bool inBss = false;
	auto flush = [&] {
		if (inBss && !cur.ssid.empty())
			all.push_back(cur);
		cur = ScannedNetwork{};
	};
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.rfind("BSS ", 0) == 0) {
			flush();
			inBss = true;
			continue;
		}
		// Only the BSS's own fields, which are indented by exactly one tab.
		if (!inBss || line.size() < 2 || line[0] != '\t' || line[1] == '\t')
			continue;
		std::string t = line.substr(1);
		if (t.rfind("SSID: ", 0) == 0)
			cur.ssid = unescapeIw(t.substr(6));
		else if (t.rfind("signal: ", 0) == 0)
			cur.signal = int(std::lround(strtod(t.c_str() + 8, nullptr)));
		else if (t.rfind("freq: ", 0) == 0)
			cur.freq = int(strtod(t.c_str() + 6, nullptr));
		else if (t.rfind("capability:", 0) == 0 && t.find("Privacy") != std::string::npos)
			cur.secure = true;
		else if (t.rfind("RSN:", 0) == 0 || t.rfind("WPA:", 0) == 0)
			cur.secure = true;
	}
	flush();
	std::map<std::string, ScannedNetwork> best;
	for (const auto &n : all) {
		auto it = best.find(n.ssid);
		if (it == best.end() || n.signal > it->second.signal)
			best[n.ssid] = n;
	}
	std::vector<ScannedNetwork> out;
	for (auto &kv : best)
		out.push_back(kv.second);
	std::stable_sort(out.begin(), out.end(), [](const ScannedNetwork &a, const ScannedNetwork &b) { return a.signal > b.signal; });
	return out;
}

static bool isHex64(const std::string &s) {
	return s.size() == 64 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return isxdigit(c) != 0; });
}

std::string validateWifi(const std::string &ssid, const std::string &password, const std::string &country) {
	if (ssid.empty())
		return "Choose a network first.";
	if (ssid.size() > 32)
		return "That network name is too long (Wi-Fi names are at most 32 characters).";
	if (!password.empty() && !isHex64(password)) {
		if (password.size() < 8 || password.size() > 63)
			return "Wi-Fi passwords are 8 to 63 characters long. Check it and try again.";
		for (unsigned char c : password)
			if (c < 32 || c > 126)
				return "The password has a character Wi-Fi passwords can't contain. Check it and try again.";
	}
	if (!country.empty() && (country.size() != 2 || !isupper(static_cast<unsigned char>(country[0])) ||
	                         !isupper(static_cast<unsigned char>(country[1]))))
		return "Pick your country from the list.";
	return "";
}

std::string wpaConfigFor(const std::string &ssid, const std::string &password, const std::string &country) {
	std::string c = "# Written by DuoPlay's Wi-Fi setup. Delete this file to run the setup again.\n"
	                "ctrl_interface=/run/wpa_supplicant\n";
	if (country.size() == 2)
		c += "country=" + country + "\n";
	c += "\nnetwork={\n";
	bool plain = std::all_of(ssid.begin(), ssid.end(), [](unsigned char ch) { return ch >= 32 && ch < 127; });
	if (plain)
		c += "\t# " + ssid + "\n";
	// Hex sidesteps quoting for names with any characters in them.
	c += "\tssid=" + hexEncode(reinterpret_cast<const uint8_t *>(ssid.data()), ssid.size()) + "\n";
	c += "\tscan_ssid=1\n";
	if (password.empty()) {
		c += "\tkey_mgmt=NONE\n";
	} else if (isHex64(password)) {
		c += "\tpsk=" + password + "\n";
	} else {
		// The key itself rather than the password in quotes: wpa_supplicant's parser trips over
		// passwords with both " and # in them, and this way the password isn't on the card.
		uint8_t psk[32];
		PKCS5_PBKDF2_HMAC_SHA1(password.data(), int(password.size()), reinterpret_cast<const uint8_t *>(ssid.data()),
		                       int(ssid.size()), 4096, sizeof(psk), psk);
		c += "\tpsk=" + hexEncode(psk, sizeof(psk)) + "\n";
	}
	c += "}\n";
	return c;
}

#ifdef __linux__

// Written straight through to the SD card, so pulling the plug a second later can't lose it.
static bool saveDurably(const std::string &path, const std::string &data) {
	std::string tmp = path + ".tmp";
	int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return false;
	bool ok = write(fd, data.data(), data.size()) == ssize_t(data.size());
	ok = fsync(fd) == 0 && ok;
	close(fd);
	if (!ok || rename(tmp.c_str(), path.c_str()) != 0)
		return false;
	std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
	int dfd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	return true;
}

static std::string trimmed(const std::string &s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool wifiFromTxt(const std::string &path, const std::string &saveTo, std::string &err) {
	std::string text;
	if (!readFile(path, text)) {
		err = "can't read " + path;
		return false;
	}
	if (text.rfind("\xEF\xBB\xBF", 0) == 0) // byte order mark some editors add
		text.erase(0, 3);
	std::string ssid, password, country;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		line = trimmed(line);
		size_t sep = line.find_first_of(":=");
		if (line.empty() || line[0] == '#' || sep == std::string::npos)
			continue;
		std::string key = trimmed(line.substr(0, sep)), value = trimmed(line.substr(sep + 1));
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(tolower(c)); });
		if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
			value = value.substr(1, value.size() - 2);
		if (key == "name" || key == "ssid" || key == "network" || key == "wifi" || key == "wi-fi")
			ssid = value;
		else if (key == "password" || key == "pass" || key == "psk" || key == "key")
			password = value;
		else if (key == "country") {
			country = value;
			std::transform(country.begin(), country.end(), country.begin(), [](unsigned char c) { return char(toupper(c)); });
		}
	}
	std::string problem = validateWifi(ssid, password, country);
	if (!problem.empty()) {
		err = problem;
		return false;
	}
	if (!saveDurably(saveTo, wpaConfigFor(ssid, password, country))) {
		err = "couldn't write " + saveTo;
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// DHCP and DNS for the hotspot

namespace {

uint32_t get32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void set32(uint8_t *p, uint32_t v) {
	p[0] = uint8_t(v >> 24);
	p[1] = uint8_t(v >> 16);
	p[2] = uint8_t(v >> 8);
	p[3] = uint8_t(v);
}

int udpSocket(int port, const std::string &iface, bool broadcast, std::string &err) {
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		err = strerror(errno);
		return -1;
	}
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (broadcast)
		setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
	if (!iface.empty() && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface.c_str(), socklen_t(iface.size())) != 0) {
		err = "can't use " + iface + ": " + strerror(errno);
		close(fd);
		return -1;
	}
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(uint16_t(port));
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		err = "port " + std::to_string(port) + ": " + strerror(errno);
		close(fd);
		return -1;
	}
	return fd;
}

class HotspotServices {
public:
	HotspotServices(const SetupPortalOptions &o, std::atomic<int64_t> &activity) : mOpts(o), mActivity(activity) {
		in_addr a{};
		inet_pton(AF_INET, o.ip.c_str(), &a);
		mIp = ntohl(a.s_addr);
	}
	~HotspotServices() { stop(); }

	bool start(std::string &err) {
		mDhcp = udpSocket(mOpts.dhcpPort, mOpts.iface, true, err);
		if (mDhcp < 0) {
			err = "DHCP " + err;
			return false;
		}
		mDns = udpSocket(mOpts.dnsPort, mOpts.iface, false, err);
		if (mDns < 0) {
			err = "DNS " + err;
			return false;
		}
		mRunning = true;
		mThread = std::thread([this] { run(); });
		return true;
	}

	void stop() {
		mRunning = false;
		if (mThread.joinable())
			mThread.join();
		for (int *fd : {&mDhcp, &mDns})
			if (*fd >= 0) {
				close(*fd);
				*fd = -1;
			}
	}

private:
	void run() {
		uint8_t buf[1500];
		while (mRunning) {
			pollfd p[2] = {{mDhcp, POLLIN, 0}, {mDns, POLLIN, 0}};
			if (poll(p, 2, 300) <= 0)
				continue;
			for (int k = 0; k < 2; k++) {
				if (!(p[k].revents & POLLIN))
					continue;
				sockaddr_in from{};
				socklen_t fl = sizeof(from);
				ssize_t n = recvfrom(p[k].fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fl);
				if (n <= 0)
					continue;
				if (k == 0)
					dhcp(buf, size_t(n));
				else
					dns(buf, size_t(n), from);
			}
		}
	}

	// --- DHCP (RFC 2131), just enough for phones joining the setup hotspot ---
	uint32_t leaseFor(const std::array<uint8_t, 6> &mac, uint32_t wanted) {
		auto it = mLeases.find(mac);
		if (it != mLeases.end())
			return it->second;
		const uint32_t net = mIp & 0xFFFFFF00u;
		auto taken = [&](uint32_t ip) {
			for (const auto &kv : mLeases)
				if (kv.second == ip)
					return true;
			return ip == mIp;
		};
		uint32_t ip = 0;
		if ((wanted & 0xFFFFFF00u) == net && (wanted & 0xFF) >= 10 && (wanted & 0xFF) <= 250 && !taken(wanted))
			ip = wanted;
		for (uint32_t h = 10; !ip && h <= 250; h++)
			if (!taken(net | h))
				ip = net | h;
		if (!ip) { // a setup hotspot only ever sees a phone or two; start over rather than refuse
			mLeases.clear();
			ip = net | 10;
		}
		mLeases[mac] = ip;
		return ip;
	}

	void dhcp(const uint8_t *q, size_t n) {
		if (n < 240 || q[0] != 1 || q[1] != 1 || q[2] != 6 || get32(q + 236) != 0x63825363)
			return;
		int type = 0;
		uint32_t requested = 0, server = 0;
		for (size_t i = 240; i < n;) {
			uint8_t code = q[i++];
			if (code == 0)
				continue;
			if (code == 255 || i >= n)
				break;
			uint8_t len = q[i++];
			if (i + len > n)
				break;
			if (code == 53 && len >= 1)
				type = q[i];
			else if (code == 50 && len == 4)
				requested = get32(q + i);
			else if (code == 54 && len == 4)
				server = get32(q + i);
			i += len;
		}
		std::array<uint8_t, 6> mac;
		memcpy(mac.data(), q + 28, 6);
		const uint32_t ciaddr = get32(q + 12);
		mActivity = nowMs();
		switch (type) {
		case 1: // DISCOVER -> OFFER
			reply(q, 2, leaseFor(mac, requested));
			break;
		case 3: { // REQUEST -> ACK / NAK
			if (server && server != mIp)
				return; // the phone picked another server
			uint32_t want = requested ? requested : ciaddr;
			uint32_t ip = leaseFor(mac, want);
			if (want && want != ip)
				reply(q, 6, 0);
			else
				reply(q, 5, ip);
			LOGI("setup", "Phone joined the setup hotspot (%u.%u.%u.%u)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
			break;
		}
		case 4: // DECLINE: someone else has that address
		case 7: // RELEASE
			mLeases.erase(mac);
			break;
		case 8: // INFORM
			reply(q, 5, 0);
			break;
		default:
			break;
		}
	}

	void reply(const uint8_t *q, uint8_t type, uint32_t yiaddr) {
		std::vector<uint8_t> r(240, 0);
		r[0] = 2;
		r[1] = 1;
		r[2] = 6;
		memcpy(&r[4], q + 4, 4);   // transaction id
		memcpy(&r[10], q + 10, 2); // flags
		if (type == 5 && !yiaddr)
			memcpy(&r[12], q + 12, 4); // INFORM: ciaddr
		set32(&r[16], yiaddr);
		set32(&r[20], type == 6 ? 0 : mIp);
		memcpy(&r[24], q + 24, 4);  // giaddr
		memcpy(&r[28], q + 28, 16); // chaddr
		set32(&r[236], 0x63825363);
		auto opt = [&](uint8_t code, std::initializer_list<uint8_t> v) {
			r.push_back(code);
			r.push_back(uint8_t(v.size()));
			r.insert(r.end(), v.begin(), v.end());
		};
		auto opt32 = [&](uint8_t code, uint32_t v) { opt(code, {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); };
		opt(53, {type});
		opt32(54, mIp);
		if (type != 6) {
			if (yiaddr)
				opt32(51, 600);
			opt32(1, 0xFFFFFF00u);
			opt32(3, mIp); // a router: without one iOS never checks for a sign-in page
			opt32(6, mIp); // and DNS, which answers everything with the Pi
			opt32(28, (mIp & 0xFFFFFF00u) | 0xFF);
		}
		r.push_back(255);
		if (r.size() < 300)
			r.resize(300, 0);
		sockaddr_in to{};
		to.sin_family = AF_INET;
		to.sin_port = htons(uint16_t(mOpts.dhcpPort + 1));
		uint32_t ciaddr = get32(q + 12);
		to.sin_addr.s_addr = htonl(type != 6 && ciaddr ? ciaddr : INADDR_BROADCAST);
		sendto(mDhcp, r.data(), r.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to));
	}

	// --- DNS: every name is the Pi, which is how phones find the setup page by themselves ---
	void dns(const uint8_t *q, size_t n, const sockaddr_in &from) {
		if (n < 12 || (q[2] & 0x80) || (q[2] & 0x78) || ((q[4] << 8) | q[5]) < 1)
			return; // not a standard query
		size_t p = 12;
		while (p < n && q[p] != 0) {
			if ((q[p] & 0xC0) || p + 1 + q[p] >= n)
				return;
			p += 1 + q[p];
		}
		p++;
		if (p + 4 > n)
			return;
		uint16_t qtype = uint16_t(q[p] << 8 | q[p + 1]);
		size_t qend = p + 4;
		bool answerA = qtype == 1 || qtype == 255;
		std::vector<uint8_t> r(q, q + qend);
		r[2] = uint8_t(0x84 | (q[2] & 0x01)); // response, authoritative, keep "recursion desired"
		r[3] = 0x80;                           // recursion available, no error
		r[4] = 0, r[5] = 1;
		r[6] = 0, r[7] = answerA ? 1 : 0;
		r[8] = r[9] = r[10] = r[11] = 0;
		if (answerA) {
			// Short-lived, so nothing sticks once the phone is back on its own Wi-Fi.
			const uint8_t rr[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 5, 0, 4};
			r.insert(r.end(), rr, rr + sizeof(rr));
			uint8_t ip[4];
			set32(ip, mIp);
			r.insert(r.end(), ip, ip + 4);
		}
		sendto(mDns, r.data(), r.size(), 0, reinterpret_cast<const sockaddr *>(&from), sizeof(from));
	}

	const SetupPortalOptions &mOpts;
	std::atomic<int64_t> &mActivity;
	uint32_t mIp = 0;
	int mDhcp = -1, mDns = -1;
	std::atomic<bool> mRunning{false};
	std::thread mThread;
	std::map<std::array<uint8_t, 6>, uint32_t> mLeases;
};

const EmbeddedFile *embedded(const std::string &path) {
	for (size_t i = 0; i < kEmbeddedWebCount; i++)
		if (path == kEmbeddedWeb[i].path)
			return &kEmbeddedWeb[i];
	return nullptr;
}

} // namespace

int runSetupPortal(const SetupPortalOptions &o, const std::atomic<bool> &quit) {
	std::atomic<int64_t> activity{nowMs()};
	std::vector<ScannedNetwork> networks;
	if (!o.scanFile.empty()) {
		std::string scan;
		if (readFile(o.scanFile, scan))
			networks = parseIwScan(scan);
	}
	LOGI("setup", "Wi-Fi setup is open on %s (%s); %zu networks nearby", o.iface.empty() ? "all interfaces" : o.iface.c_str(),
	     o.ip.c_str(), networks.size());

	HotspotServices services(o, activity);
	std::string err;
	if (!services.start(err)) {
		LOGE("setup", "Can't start the setup hotspot's services: %s", err.c_str());
		return 1;
	}

	const EmbeddedFile *page = embedded("setup.html");
	if (!page) {
		LOGE("setup", "The setup page isn't built in");
		return 1;
	}

	std::mutex m;
	bool saved = false;
	int64_t savedAt = 0;
	std::string savedName;

	httplib::Server srv;
	srv.new_task_queue = [] { return new httplib::ThreadPool(4); };
	srv.set_pre_routing_handler([&](const httplib::Request &, httplib::Response &res) {
		activity = nowMs();
		res.set_header("Cache-Control", "no-store");
		return httplib::Server::HandlerResponse::Unhandled;
	});
	srv.Get("/setup/networks", [&](const httplib::Request &, httplib::Response &res) {
		json list = json::array();
		for (const auto &n : networks)
			list.push_back({{"ssid", n.ssid},
			                {"id", hexEncode(reinterpret_cast<const uint8_t *>(n.ssid.data()), n.ssid.size())},
			                {"signal", n.signal},
			                {"secure", n.secure},
			                {"freq", n.freq}});
		json j = {{"networks", list}, {"message", o.message}, {"country", o.country}};
		res.set_content(j.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
	});
	srv.Post("/setup/join", [&](const httplib::Request &req, httplib::Response &res) {
		json j = json::parse(req.body, nullptr, false);
		auto fail = [&](int status, const std::string &msg) {
			res.status = status;
			res.set_content(json({{"error", msg}}).dump(), "application/json");
		};
		if (!j.is_object())
			return fail(400, "Something went wrong sending that. Try again.");
		std::string ssid;
		if (j.contains("id") && j["id"].is_string() && !j["id"].get<std::string>().empty()) {
			auto raw = hexDecode(j["id"].get<std::string>());
			if (!raw)
				return fail(400, "Choose a network first.");
			ssid.assign(raw->begin(), raw->end());
		} else {
			ssid = j.value("ssid", "");
		}
		std::string password = j.value("password", "");
		std::string country = j.value("country", "");
		std::string problem = validateWifi(ssid, password, country);
		if (!problem.empty())
			return fail(400, problem);
		if (o.saveTo.empty() || !saveDurably(o.saveTo, wpaConfigFor(ssid, password, country))) {
			LOGE("setup", "Couldn't save the Wi-Fi settings to %s", o.saveTo.c_str());
			return fail(500, "The Pi couldn't save that to its SD card. Check the card isn't locked and try again.");
		}
		{
			std::lock_guard<std::mutex> lk(m);
			saved = true;
			savedAt = nowMs();
			savedName = ssid;
		}
		LOGI("setup", "Wi-Fi network chosen; switching over");
		res.set_content(json({{"ok", true}}).dump(), "application/json");
	});
	// Everything else is the setup page itself, whatever the phone asked for: that's what makes
	// iOS's "is there a sign-in page?" check (captive.apple.com) open it in a sheet.
	srv.Get(R"(/.*)", [&](const httplib::Request &req, httplib::Response &res) {
		std::string path = req.path.size() > 1 ? req.path.substr(1) : "";
		const EmbeddedFile *f = nullptr;
		if (path.rfind("icons/", 0) == 0)
			f = embedded(path);
		if (!f)
			f = page;
		res.set_content(reinterpret_cast<const char *>(f->data), f->size, f->mime);
	});

	if (!srv.bind_to_port("0.0.0.0", o.httpPort)) {
		LOGE("setup", "Can't open the setup page on port %d", o.httpPort);
		return 1;
	}
	std::thread http([&] { srv.listen_after_bind(); });

	int rc = 130;
	while (!quit) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		std::lock_guard<std::mutex> lk(m);
		if (saved && nowMs() - savedAt > 2500) { // let the "joining" page reach the phone first
			rc = 0;
			break;
		}
		if (!saved && o.idleTimeoutSec > 0 && nowMs() - activity > int64_t(o.idleTimeoutSec) * 1000) {
			LOGI("setup", "Nobody used the setup page for %d s", o.idleTimeoutSec);
			rc = 3;
			break;
		}
	}
	srv.stop();
	http.join();
	services.stop();
	return rc;
}

#else

bool wifiFromTxt(const std::string &, const std::string &, std::string &err) {
	err = "only for the Raspberry Pi kit (Linux)";
	return false;
}

int runSetupPortal(const SetupPortalOptions &, const std::atomic<bool> &) {
	LOGE("setup", "The Wi-Fi setup hotspot is only for the Raspberry Pi kit (Linux)");
	return 1;
}

#endif

} // namespace duo
