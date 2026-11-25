// SPDX-License-Identifier: AGPL-3.0-only
#include "util.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <direct.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace duo {

static std::atomic<int> g_level{int(LogLevel::Info)};
static std::mutex g_log_mutex;

void setLogLevel(LogLevel level) { g_level = int(level); }
LogLevel logLevel() { return LogLevel(g_level.load()); }

void logf(LogLevel level, const char *tag, const char *fmt, ...) {
	if (int(level) < g_level.load())
		return;
	char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	auto now = std::chrono::system_clock::now();
	std::time_t t = std::chrono::system_clock::to_time_t(now);
	int ms = int(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	static const char *names[] = {"debug", "info ", "warn ", "error"};
	std::lock_guard<std::mutex> lock(g_log_mutex);
	fprintf(stderr, "%02d:%02d:%02d.%03d %s [%s] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
	        names[int(level)], tag, msg);
	fflush(stderr);
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t *data, size_t len) {
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3) {
		uint32_t n = uint32_t(data[i]) << 16;
		if (i + 1 < len)
			n |= uint32_t(data[i + 1]) << 8;
		if (i + 2 < len)
			n |= data[i + 2];
		out += kB64[(n >> 18) & 63];
		out += kB64[(n >> 12) & 63];
		out += i + 1 < len ? kB64[(n >> 6) & 63] : '=';
		out += i + 2 < len ? kB64[n & 63] : '=';
	}
	return out;
}

std::optional<std::vector<uint8_t>> base64Decode(const std::string &in) {
	std::vector<uint8_t> out;
	uint32_t buf = 0;
	int bits = 0;
	for (char c : in) {
		if (c == '=' || c == ' ' || c == '\n' || c == '\r')
			continue;
		const char *p = strchr(kB64, c);
		if (c == '-')
			p = kB64 + 62; // tolerate URL-safe alphabet
		else if (c == '_')
			p = kB64 + 63;
		if (!p || !*p)
			return std::nullopt;
		buf = (buf << 6) | uint32_t(p - kB64);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back(uint8_t((buf >> bits) & 0xff));
		}
	}
	return out;
}

std::string hexEncode(const uint8_t *data, size_t len) {
	static const char digits[] = "0123456789abcdef";
	std::string out;
	out.reserve(len * 2);
	for (size_t i = 0; i < len; i++) {
		out += digits[data[i] >> 4];
		out += digits[data[i] & 15];
	}
	return out;
}

std::optional<std::vector<uint8_t>> hexDecode(const std::string &in) {
	if (in.size() % 2)
		return std::nullopt;
	std::vector<uint8_t> out;
	out.reserve(in.size() / 2);
	auto val = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (size_t i = 0; i < in.size(); i += 2) {
		int hi = val(in[i]), lo = val(in[i + 1]);
		if (hi < 0 || lo < 0)
			return std::nullopt;
		out.push_back(uint8_t(hi << 4 | lo));
	}
	return out;
}

std::string randomId(size_t bytes) {
	std::vector<uint8_t> b(bytes);
	RAND_bytes(b.data(), int(b.size()));
	return hexEncode(b.data(), b.size());
}

bool readFile(const std::string &path, std::string &out) {
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return false;
	std::ostringstream ss;
	ss << f.rdbuf();
	out = ss.str();
	return true;
}

bool writeFileAtomic(const std::string &path, const std::string &data) {
	std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f)
			return false;
		f.write(data.data(), std::streamsize(data.size()));
		if (!f)
			return false;
	}
#ifdef _WIN32
	return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
	return rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

bool makeDirs(const std::string &path) {
	if (path.empty())
		return false;
	std::string cur;
	for (size_t i = 0; i < path.size(); i++) {
		cur += path[i];
		if ((path[i] == '/' || path[i] == '\\' || i + 1 == path.size()) && cur.size() > 1) {
#ifdef _WIN32
			_mkdir(cur.c_str());
#else
			mkdir(cur.c_str(), 0700);
#endif
		}
	}
	return true;
}

std::string joinPath(const std::string &a, const std::string &b) {
	if (a.empty())
		return b;
	char last = a.back();
	if (last == '/' || last == '\\')
		return a + b;
	return a + "/" + b;
}

std::string defaultDataDir() {
#ifdef _WIN32
	const char *appdata = getenv("APPDATA");
	return joinPath(appdata ? appdata : ".", "DuoPlay");
#else
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		return joinPath(xdg, "duoplay");
	const char *home = getenv("HOME");
	return joinPath(home ? home : ".", ".config/duoplay");
#endif
}

std::string executableDir() {
	std::string path;
#ifdef _WIN32
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	path.assign(buf, n);
#elif defined(__APPLE__)
	char buf[4096];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
		path = buf;
#else
	char buf[4096];
	ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
	if (n > 0)
		path.assign(buf, size_t(n));
#endif
	size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? "." : path.substr(0, slash);
}

static int addressRank(const std::string &ip) {
	// Prefer typical home-LAN ranges so the URL we print is the one the phone can reach.
	if (ip.rfind("192.168.", 0) == 0)
		return 0;
	if (ip.rfind("10.", 0) == 0)
		return 1;
	if (ip.rfind("172.", 0) == 0) {
		int second = atoi(ip.c_str() + 4);
		if (second >= 16 && second <= 31)
			return 2; // also where Docker bridges live, hence lower
	}
	if (ip.rfind("169.254.", 0) == 0)
		return 9;
	return 5;
}

std::vector<LocalAddress> localIPv4Addresses() {
	std::vector<LocalAddress> out;
#ifdef _WIN32
	ULONG size = 15000;
	std::vector<uint8_t> buf(size);
	auto *addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
	                         nullptr, addrs, &size) == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buf.data());
		if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
		                         nullptr, addrs, &size) != NO_ERROR)
			return out;
	}
	for (auto *a = addrs; a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
			uint32_t mask = u->OnLinkPrefixLength ? htonl(~((1u << (32 - u->OnLinkPrefixLength)) - 1)) : 0;
			in_addr bc;
			bc.s_addr = sin->sin_addr.s_addr | ~mask;
			char bcs[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &bc, bcs, sizeof(bcs));
			char name[256];
			WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
			out.push_back({name, ip, bcs});
		}
	}
#else
	ifaddrs *ifs = nullptr;
	if (getifaddrs(&ifs) != 0)
		return out;
	for (ifaddrs *i = ifs; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
			continue;
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_addr)->sin_addr, ip, sizeof(ip));
		std::string bc;
		if ((i->ifa_flags & IFF_BROADCAST) && i->ifa_broadaddr) {
			char b[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(i->ifa_broadaddr)->sin_addr, b, sizeof(b));
			bc = b;
		}
		out.push_back({i->ifa_name, ip, bc});
	}
	freeifaddrs(ifs);
#endif
	std::stable_sort(out.begin(), out.end(), [](const LocalAddress &a, const LocalAddress &b) {
		auto docker = [](const LocalAddress &x) {
			return x.iface.rfind("docker", 0) == 0 || x.iface.rfind("br-", 0) == 0 || x.iface.rfind("veth", 0) == 0;
		};
		if (docker(a) != docker(b))
			return !docker(a);
		return addressRank(a.ip) < addressRank(b.ip);
	});
	return out;
}

std::string hostName() {
	char buf[256] = {0};
	if (gethostname(buf, sizeof(buf) - 1) != 0)
		return "duoplay";
	return buf;
}

} // namespace duo
