// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace duo {

// Multicast DNS (Bonjour) for one or more names: answers "<name>.local" with this machine's
// address on whichever network the question came from, so the phone can simply open
// http://duoplay.local:8080 instead of hunting for the bridge's IP address. Coexists with
// avahi (Raspberry Pi OS), which keeps answering for the machine's own host name.
// Linux only; elsewhere start() does nothing.
class MdnsResponder {
public:
	explicit MdnsResponder(std::vector<std::string> names); // without ".local"
	~MdnsResponder();
	void start();
	void stop();

	// Builds the reply to one mDNS packet, if it asks about one of `names` (exposed for tests).
	// `ip` is the IPv4 address (host byte order) to answer with. Returns false when the
	// packet is not for us. `unicastTo` is set for questions that want a direct reply.
	static bool answer(const std::vector<std::string> &names, const uint8_t *pkt, size_t len, uint32_t ip,
	                   bool legacyUnicast, std::vector<uint8_t> &out, bool &unicastRequested);

private:
	void run();

	std::vector<std::string> mNames;
	std::atomic<bool> mRunning{false};
	std::thread mThread;
};

} // namespace duo
