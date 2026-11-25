// SPDX-License-Identifier: AGPL-3.0-only
#include "mdns.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef __linux__
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace duo {

namespace {

constexpr uint16_t kTypeA = 1, kTypeNsec = 47, kTypeAny = 255, kTypeAAAA = 28;
constexpr uint16_t kClassIn = 1, kCacheFlush = 0x8000, kUnicastResponse = 0x8000;

uint16_t get16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
void put16(std::vector<uint8_t> &v, uint16_t x) {
	v.push_back(uint8_t(x >> 8));
	v.push_back(uint8_t(x));
}
void put32(std::vector<uint8_t> &v, uint32_t x) {
	put16(v, uint16_t(x >> 16));
	put16(v, uint16_t(x));
}

// Reads a (possibly compressed) name, lower-cased with dots. Advances `off` past it.
bool readName(const uint8_t *p, size_t n, size_t &off, std::string &out) {
	out.clear();
	size_t pos = off;
	bool jumped = false;
	for (int hops = 0; hops < 32;) {
		if (pos >= n)
			return false;
		uint8_t l = p[pos];
		if ((l & 0xC0) == 0xC0) {
			if (pos + 1 >= n)
				return false;
			if (!jumped)
				off = pos + 2;
			jumped = true;
			pos = size_t(l & 0x3F) << 8 | p[pos + 1];
			hops++;
			continue;
		}
		if (l & 0xC0)
			return false;
		if (l == 0) {
			if (!jumped)
				off = pos + 1;
			return true;
		}
		if (pos + 1 + l > n || out.size() + l > 255)
			return false;
		if (!out.empty())
			out += '.';
		for (size_t k = 0; k < l; k++)
			out += char(tolower(p[pos + 1 + k]));
		pos += 1 + l;
	}
	return false;
}

void putName(std::vector<uint8_t> &v, const std::string &fqdn) {
	size_t start = 0;
	while (start < fqdn.size()) {
		size_t dot = fqdn.find('.', start);
		if (dot == std::string::npos)
			dot = fqdn.size();
		v.push_back(uint8_t(dot - start));
		v.insert(v.end(), fqdn.begin() + long(start), fqdn.begin() + long(dot));
		start = dot + 1;
	}
	v.push_back(0);
}

} // namespace

bool MdnsResponder::answer(const std::vector<std::string> &names, const uint8_t *p, size_t n, uint32_t ip,
                           bool legacyUnicast, std::vector<uint8_t> &out, bool &unicastRequested) {
	unicastRequested = false;
	if (n < 12 || (get16(p + 2) & 0x8000)) // too short, or a response rather than a question
		return false;
	uint16_t qd = get16(p + 4);
	size_t off = 12;
	std::string matched;
	bool wantA = false, wantOther = false;
	size_t qStart = 0, qEnd = 0;
	for (uint16_t i = 0; i < qd && i < 16; i++) {
		size_t begin = off;
		std::string name;
		if (!readName(p, n, off, name) || off + 4 > n)
			return false;
		uint16_t type = get16(p + off), cls = get16(p + off + 2);
		off += 4;
		if ((cls & 0x7FFF) != kClassIn && (cls & 0x7FFF) != kTypeAny)
			continue;
		for (const auto &want : names) {
			if (name != want + ".local")
				continue;
			matched = name;
			if (type == kTypeA || type == kTypeAny)
				wantA = true;
			else if (type == kTypeAAAA)
				wantOther = true; // say quickly that there's no IPv6 address instead of letting it time out
			if (cls & kUnicastResponse)
				unicastRequested = true;
			if (!qStart) {
				qStart = begin;
				qEnd = off;
			}
		}
	}
	if (matched.empty() || (!wantA && !wantOther))
		return false;

	// Multicast replies carry no question and use the cache-flush bit; replies to "legacy"
	// one-shot resolvers (source port other than 5353) echo the question and the query id.
	const uint32_t ttl = legacyUnicast ? 10 : 120;
	const uint16_t cls = legacyUnicast ? kClassIn : uint16_t(kClassIn | kCacheFlush);
	out.clear();
	put16(out, legacyUnicast ? get16(p) : 0);
	put16(out, 0x8400); // response, authoritative
	put16(out, legacyUnicast ? 1 : 0);
	put16(out, 1);      // one answer
	put16(out, 0);
	put16(out, 1);      // one additional record
	size_t nameAt;
	if (legacyUnicast) {
		// Re-encode the question uncompressed (the original may point into itself).
		nameAt = out.size();
		putName(out, matched);
		size_t typeAt = qEnd - 4;
		out.insert(out.end(), p + typeAt, p + typeAt + 2);
		put16(out, kClassIn);
	}
	auto ptr = [&](size_t at) { put16(out, uint16_t(0xC000 | at)); };
	auto aRecord = [&](bool first) {
		if (first && !legacyUnicast) {
			nameAt = out.size();
			putName(out, matched);
		} else {
			ptr(nameAt);
		}
		put16(out, kTypeA);
		put16(out, cls);
		put32(out, ttl);
		put16(out, 4);
		put32(out, ip);
	};
	auto nsec = [&](bool first) {
		if (first && !legacyUnicast) {
			nameAt = out.size();
			putName(out, matched);
		} else {
			ptr(nameAt);
		}
		put16(out, kTypeNsec);
		put16(out, cls);
		put32(out, ttl);
		put16(out, 5);
		ptr(nameAt);      // next domain name: ourselves
		out.push_back(0); // window 0
		out.push_back(1); // bitmap length
		out.push_back(0x40); // only type 1 (A) exists
	};
	if (wantA) {
		aRecord(true);
		nsec(false);
	} else {
		nsec(true);
		aRecord(false);
	}
	(void)qStart;
	return true;
}

MdnsResponder::MdnsResponder(std::vector<std::string> names) {
	for (auto &n : names) {
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(tolower(c)); });
		if (!n.empty())
			mNames.push_back(n);
	}
}

MdnsResponder::~MdnsResponder() { stop(); }

void MdnsResponder::start() {
#ifdef __linux__
	if (mRunning || mNames.empty())
		return;
	mRunning = true;
	mThread = std::thread([this] { run(); });
#endif
}

void MdnsResponder::stop() {
	mRunning = false;
	if (mThread.joinable())
		mThread.join();
}

#ifdef __linux__
namespace {

struct Iface {
	unsigned index;
	uint32_t ip; // host order
};

std::vector<Iface> currentInterfaces() {
	std::vector<Iface> out;
	for (const auto &a : localIPv4Addresses()) {
		in_addr in{};
		if (inet_pton(AF_INET, a.ip.c_str(), &in) != 1)
			continue;
		unsigned idx = if_nametoindex(a.iface.c_str());
		if (idx)
			out.push_back({idx, ntohl(in.s_addr)});
	}
	std::sort(out.begin(), out.end(), [](const Iface &a, const Iface &b) {
		return a.index != b.index ? a.index < b.index : a.ip < b.ip;
	});
	return out;
}

int openSocket(const std::vector<Iface> &ifs) {
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
	setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one));
	int ttl = 255;
	setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
	sockaddr_in sa{};
	sa.sin_family = AF_INET;
	sa.sin_port = htons(5353);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(fd, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) != 0) {
		LOGW("mdns", "Can't listen for .local names (port 5353: %s)", strerror(errno));
		close(fd);
		return -1;
	}
	int joined = 0;
	for (const auto &i : ifs) {
		ip_mreqn m{};
		inet_pton(AF_INET, "224.0.0.251", &m.imr_multiaddr);
		m.imr_address.s_addr = htonl(i.ip);
		m.imr_ifindex = int(i.index);
		if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0 || errno == EADDRINUSE)
			joined++;
	}
	if (!joined) {
		close(fd);
		return -1;
	}
	return fd;
}

void sendTo(int fd, const std::vector<uint8_t> &pkt, const sockaddr_in &to, const Iface *via) {
	if (via) {
		ip_mreqn m{};
		m.imr_address.s_addr = htonl(via->ip);
		m.imr_ifindex = int(via->index);
		setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &m, sizeof(m));
	}
	sendto(fd, pkt.data(), pkt.size(), 0, reinterpret_cast<const sockaddr *>(&to), sizeof(to));
}

} // namespace

void MdnsResponder::run() {
	int fd = -1;
	std::vector<Iface> ifs;
	int64_t nextCheck = 0;
	sockaddr_in group{};
	group.sin_family = AF_INET;
	group.sin_port = htons(5353);
	inet_pton(AF_INET, "224.0.0.251", &group.sin_addr);
	bool logged = false;

	while (mRunning) {
		if (nowMs() >= nextCheck) {
			// Addresses come and go (Wi-Fi joins, DHCP renewals): re-join the group when they change.
			nextCheck = nowMs() + 4000;
			auto now = currentInterfaces();
			bool changed = now.size() != ifs.size() ||
			               !std::equal(now.begin(), now.end(), ifs.begin(), [](const Iface &a, const Iface &b) {
				               return a.index == b.index && a.ip == b.ip;
			               });
			if (changed || fd < 0) {
				if (fd >= 0)
					close(fd);
				ifs = now;
				fd = ifs.empty() ? -1 : openSocket(ifs);
				if (fd >= 0) {
					if (!logged)
						LOGI("mdns", "Answering for %s.local", mNames.front().c_str());
					logged = true;
					// Announce, so a phone that asked a moment ago doesn't wait for its retry.
					for (const auto &i : ifs) {
						std::vector<uint8_t> q, pkt;
						put16(q, 0), put16(q, 0), put16(q, 1), put16(q, 0), put16(q, 0), put16(q, 0);
						putName(q, mNames.front() + ".local");
						put16(q, kTypeA), put16(q, kClassIn);
						bool uni;
						if (answer(mNames, q.data(), q.size(), i.ip, false, pkt, uni))
							sendTo(fd, pkt, group, &i);
					}
				}
			}
		}
		if (fd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		pollfd pfd{fd, POLLIN, 0};
		if (poll(&pfd, 1, 500) <= 0)
			continue;
		uint8_t buf[2048];
		uint8_t ctrl[256];
		sockaddr_in from{};
		iovec iov{buf, sizeof(buf)};
		msghdr msg{};
		msg.msg_name = &from;
		msg.msg_namelen = sizeof(from);
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = ctrl;
		msg.msg_controllen = sizeof(ctrl);
		ssize_t n = recvmsg(fd, &msg, 0);
		if (n <= 0)
			continue;
		unsigned ifindex = 0;
		for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
			if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
				ifindex = unsigned(reinterpret_cast<in_pktinfo *>(CMSG_DATA(c))->ipi_ifindex);
		// Answer with the address on the network the question came from.
		const Iface *via = nullptr;
		uint32_t src = ntohl(from.sin_addr.s_addr);
		for (const auto &i : ifs)
			if (i.index == ifindex) {
				if (!via)
					via = &i;
				// several addresses on one interface: prefer the one in the asker's subnet (/24 guess)
				if ((i.ip & 0xFFFFFF00u) == (src & 0xFFFFFF00u))
					via = &i;
			}
		if (!via)
			continue;
		bool legacy = ntohs(from.sin_port) != 5353;
		std::vector<uint8_t> reply;
		bool unicast = false;
		if (!answer(mNames, buf, size_t(n), via->ip, legacy, reply, unicast))
			continue;
		if (legacy || unicast)
			sendTo(fd, reply, from, nullptr);
		else
			sendTo(fd, reply, group, via);
	}
	if (fd >= 0)
		close(fd);
}
#else
void MdnsResponder::run() {}
#endif

} // namespace duo
