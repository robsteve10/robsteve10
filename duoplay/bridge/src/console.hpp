// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "store.hpp"

#include <chiaki/discovery.h>
#include <chiaki/log.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duo {

// One ChiakiLog for the whole process, forwarding into our logger.
ChiakiLog *chiakiLog();

struct DiscoveredHost {
	std::string id;      // lower-case MAC hex (matches ConsoleRecord::id)
	std::string addr;
	std::string name;
	std::string type;    // "PS5" / "PS4"
	std::string state;   // "ready" / "standby" / "unknown"
	std::string systemVersion;
	std::string runningApp;
	bool ps5 = true;
	int64_t lastSeenMs = 0;
};

// Finds consoles on the LAN, wakes them and pairs ("registers") with them.
class ConsoleManager {
public:
	explicit ConsoleManager(Store &store);
	~ConsoleManager();

	void start();
	void stop();

	std::vector<DiscoveredHost> discovered();
	// Add an address to probe directly, for networks that block broadcast.
	void addProbeHost(const std::string &host);
	// Probe more often for a little while (the setup screen is open).
	void boost();
	// Fewer probes while a stream is running; they are tiny, but every bit of airtime counts.
	void setStreaming(bool streaming) { mStreaming = streaming; }

	bool wake(const ConsoleRecord &console, std::string &err);

	// Blocking. Pairs with the console at `host` using the 8 digit PIN shown under
	// Settings > System > Remote Play > Pair Device.
	bool registerConsole(const std::string &host, bool ps5, uint32_t pin, ConsoleRecord &out, std::string &err);

private:
	void run();
	void ping();
	static void onHost(ChiakiDiscoveryHost *host, void *user);

	Store &mStore;
	ChiakiDiscovery mDiscovery{};
	ChiakiDiscoveryThread mDiscoveryThread{};
	bool mDiscoveryOk = false;
	std::thread mThread;
	std::atomic<bool> mRunning{false};
	std::atomic<bool> mStreaming{false};
	std::atomic<int64_t> mBoostUntil{0};
	std::mutex mWaitMutex;
	std::condition_variable mWaitCv;

	std::mutex mMutex;
	std::map<std::string, DiscoveredHost> mHosts; // by id
	std::vector<std::string> mProbeHosts;
};

} // namespace duo
