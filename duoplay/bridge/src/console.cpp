// SPDX-License-Identifier: AGPL-3.0-only
#include "console.hpp"
#include "util.hpp"

#include <chiaki/regist.h>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#endif

namespace duo {

static void chiakiLogCb(ChiakiLogLevel level, const char *msg, void *) {
	switch (level) {
	case CHIAKI_LOG_ERROR: LOGE("ps5", "%s", msg); break;
	case CHIAKI_LOG_WARNING: LOGW("ps5", "%s", msg); break;
	case CHIAKI_LOG_INFO: LOGI("ps5", "%s", msg); break;
	default: LOGD("ps5", "%s", msg); break;
	}
}

ChiakiLog *chiakiLog() {
	static ChiakiLog log = [] {
		ChiakiLog l;
		uint32_t mask = CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO;
		if (logLevel() == LogLevel::Debug)
			mask |= CHIAKI_LOG_DEBUG;
		chiaki_log_init(&l, mask, chiakiLogCb, nullptr);
		return l;
	}();
	return &log;
}

static std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(tolower(c)); });
	return s;
}

ConsoleManager::ConsoleManager(Store &store) : mStore(store) {}

ConsoleManager::~ConsoleManager() { stop(); }

void ConsoleManager::start() {
	if (mRunning.exchange(true))
		return;
	ChiakiErrorCode err = chiaki_discovery_init(&mDiscovery, chiakiLog(), AF_INET);
	if (err != CHIAKI_ERR_SUCCESS) {
		LOGE("discovery", "Could not open discovery socket: %s (console search disabled; you can still enter an IP)",
		     chiaki_error_string(err));
	} else {
		err = chiaki_discovery_thread_start(&mDiscoveryThread, &mDiscovery, &ConsoleManager::onHost, this);
		if (err != CHIAKI_ERR_SUCCESS) {
			LOGE("discovery", "Could not start discovery thread: %s", chiaki_error_string(err));
			chiaki_discovery_fini(&mDiscovery);
		} else {
			mDiscoveryOk = true;
		}
	}
	mThread = std::thread([this] { run(); });
}

void ConsoleManager::stop() {
	if (!mRunning.exchange(false))
		return;
	mWaitCv.notify_all();
	if (mThread.joinable())
		mThread.join();
	if (mDiscoveryOk) {
		chiaki_discovery_thread_stop(&mDiscoveryThread);
		chiaki_discovery_fini(&mDiscovery);
		mDiscoveryOk = false;
	}
}

void ConsoleManager::boost() {
	mBoostUntil = nowMs() + 30000;
	mWaitCv.notify_all();
}

void ConsoleManager::addProbeHost(const std::string &host) {
	{
		std::lock_guard<std::mutex> lock(mMutex);
		if (std::find(mProbeHosts.begin(), mProbeHosts.end(), host) == mProbeHosts.end())
			mProbeHosts.push_back(host);
	}
	boost();
}

void ConsoleManager::run() {
	while (mRunning) {
		if (mDiscoveryOk)
			ping();
		int64_t interval = mStreaming ? 5000 : (nowMs() < mBoostUntil ? 1000 : 3000);
		std::unique_lock<std::mutex> lock(mWaitMutex);
		mWaitCv.wait_for(lock, std::chrono::milliseconds(interval));
	}
}

static bool resolveIPv4(const std::string &host, sockaddr_in &out) {
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *res = nullptr;
	if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
		return false;
	memcpy(&out, res->ai_addr, sizeof(sockaddr_in));
	freeaddrinfo(res);
	return true;
}

void ConsoleManager::ping() {
	std::vector<sockaddr_in> targets;
	sockaddr_in bc{};
	bc.sin_family = AF_INET;
	bc.sin_addr.s_addr = 0xffffffff;
	targets.push_back(bc);
	// Subnet-directed broadcasts too: some OSes only route 255.255.255.255 out of one interface.
	for (const auto &a : localIPv4Addresses()) {
		sockaddr_in s{};
		if (!a.broadcast.empty() && inet_pton(AF_INET, a.broadcast.c_str(), &s.sin_addr) == 1) {
			s.sin_family = AF_INET;
			targets.push_back(s);
		}
	}
	std::vector<std::string> hosts;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		hosts = mProbeHosts;
	}
	for (const auto &c : mStore.consoles())
		if (!c.host.empty())
			hosts.push_back(c.host);
	for (const auto &h : hosts) {
		sockaddr_in s{};
		if (resolveIPv4(h, s))
			targets.push_back(s);
	}

	ChiakiDiscoveryPacket packet{};
	packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
	for (auto &t : targets) {
		packet.protocol_version = (char *)CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5;
		t.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS5);
		chiaki_discovery_send(&mDiscovery, &packet, (sockaddr *)&t, sizeof(t));
		packet.protocol_version = (char *)CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
		t.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS4);
		chiaki_discovery_send(&mDiscovery, &packet, (sockaddr *)&t, sizeof(t));
	}
}

void ConsoleManager::onHost(ChiakiDiscoveryHost *host, void *user) {
	auto *self = static_cast<ConsoleManager *>(user);
	if (!host->host_addr)
		return;
	DiscoveredHost h;
	h.addr = host->host_addr;
	h.id = lower(host->host_id ? host->host_id : h.addr);
	h.name = host->host_name ? host->host_name : "";
	h.type = host->host_type ? host->host_type : "";
	h.ps5 = chiaki_discovery_host_is_ps5(host);
	h.systemVersion = host->system_version ? host->system_version : "";
	h.runningApp = host->running_app_name ? host->running_app_name : "";
	switch (host->state) {
	case CHIAKI_DISCOVERY_HOST_STATE_READY: h.state = "ready"; break;
	case CHIAKI_DISCOVERY_HOST_STATE_STANDBY: h.state = "standby"; break;
	default: h.state = "unknown"; break;
	}
	h.lastSeenMs = nowMs();
	bool isNew;
	{
		std::lock_guard<std::mutex> lock(self->mMutex);
		auto it = self->mHosts.find(h.id);
		isNew = it == self->mHosts.end() || it->second.state != h.state || it->second.addr != h.addr;
		self->mHosts[h.id] = h;
	}
	if (isNew)
		LOGI("discovery", "%s %s at %s is %s", h.type.c_str(), h.name.c_str(), h.addr.c_str(), h.state.c_str());
	// Keep the paired console's address fresh in case DHCP handed it a new one.
	self->mStore.updateHost(h.id, h.addr);
}

std::vector<DiscoveredHost> ConsoleManager::discovered() {
	std::vector<DiscoveredHost> out;
	int64_t now = nowMs();
	std::lock_guard<std::mutex> lock(mMutex);
	for (auto it = mHosts.begin(); it != mHosts.end();) {
		if (now - it->second.lastSeenMs > 20000) {
			it = mHosts.erase(it);
			continue;
		}
		DiscoveredHost h = it->second;
		// While streaming we probe rarely, so only call a console gone after a long silence.
		if (now - h.lastSeenMs > (mStreaming ? 16000 : 8000))
			h.state = "unknown";
		out.push_back(h);
		++it;
	}
	return out;
}

bool ConsoleManager::wake(const ConsoleRecord &console, std::string &err) {
	auto key = hexDecode(console.registKey);
	if (!key || key->size() != CHIAKI_SESSION_AUTH_SIZE) {
		err = "Stored registration key is invalid; pair the console again.";
		return false;
	}
	std::string keyStr(reinterpret_cast<const char *>(key->data()), strnlen(reinterpret_cast<const char *>(key->data()), key->size()));
	uint64_t credential = strtoull(keyStr.c_str(), nullptr, 16);
	ChiakiErrorCode e = chiaki_discovery_wakeup(chiakiLog(), mDiscoveryOk ? &mDiscovery : nullptr,
	                                            console.host.c_str(), credential, console.ps5);
	if (e != CHIAKI_ERR_SUCCESS) {
		err = std::string("Wake-up failed: ") + chiaki_error_string(e);
		return false;
	}
	LOGI("console", "Sent wake-up to %s (%s)", console.nickname.c_str(), console.host.c_str());
	boost();
	return true;
}

namespace {
struct RegistWait {
	std::mutex m;
	std::condition_variable cv;
	bool done = false;
	bool ok = false;
	ChiakiRegisteredHost host{};
};
} // namespace

static void registCb(ChiakiRegistEvent *event, void *user) {
	auto *w = static_cast<RegistWait *>(user);
	std::lock_guard<std::mutex> lock(w->m);
	w->done = true;
	w->ok = event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS;
	if (w->ok && event->registered_host)
		w->host = *event->registered_host;
	w->cv.notify_all();
}

bool ConsoleManager::registerConsole(const std::string &host, bool ps5, uint32_t pin, ConsoleRecord &out,
                                     std::string &err) {
	std::string accountB64 = mStore.psnAccountId();
	auto account = base64Decode(accountB64);
	if (!account || account->size() != CHIAKI_PSN_ACCOUNT_ID_SIZE) {
		err = "Set your PSN account first (step 1).";
		return false;
	}
	ChiakiRegistInfo info{};
	info.target = ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;
	info.host = host.c_str();
	info.broadcast = false;
	info.psn_online_id = nullptr;
	memcpy(info.psn_account_id, account->data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
	info.pin = pin;
	info.console_pin = 0;
	info.holepunch_info = nullptr;
	info.rudp = nullptr;

	RegistWait wait;
	ChiakiRegist regist{};
	ChiakiErrorCode e = chiaki_regist_start(&regist, chiakiLog(), &info, registCb, &wait);
	if (e != CHIAKI_ERR_SUCCESS) {
		err = std::string("Could not start pairing: ") + chiaki_error_string(e);
		return false;
	}
	{
		std::unique_lock<std::mutex> lock(wait.m);
		if (!wait.cv.wait_for(lock, std::chrono::seconds(30), [&] { return wait.done; })) {
			lock.unlock();
			chiaki_regist_stop(&regist);
			lock.lock();
			wait.cv.wait_for(lock, std::chrono::seconds(5), [&] { return wait.done; });
		}
	}
	chiaki_regist_fini(&regist);
	if (!wait.ok) {
		err = "The console refused pairing. Check the PIN (it changes every few minutes) and that the PSN account "
		      "is the one signed in on the console.";
		return false;
	}

	const ChiakiRegisteredHost &r = wait.host;
	out.id = hexEncode(r.server_mac, sizeof(r.server_mac));
	out.nickname = std::string(r.server_nickname, strnlen(r.server_nickname, sizeof(r.server_nickname)));
	out.host = host;
	out.ps5 = chiaki_target_is_ps5(r.target);
	out.target = int(r.target);
	out.registKey = hexEncode(reinterpret_cast<const uint8_t *>(r.rp_regist_key), sizeof(r.rp_regist_key));
	out.rpKey = hexEncode(r.rp_key, sizeof(r.rp_key));
	out.rpKeyType = r.rp_key_type;
	mStore.upsertConsole(out);
	LOGI("console", "Paired with %s (%s)", out.nickname.c_str(), host.c_str());
	return true;
}

} // namespace duo
