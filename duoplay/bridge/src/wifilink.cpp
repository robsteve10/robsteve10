// SPDX-License-Identifier: AGPL-3.0-only
#include "wifilink.hpp"
#include "util.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace duo {

namespace {

uint16_t rtpSeq(const rtc::message_ptr &m) {
	auto *b = reinterpret_cast<const uint8_t *>(m->data());
	return uint16_t(b[2] << 8 | b[3]);
}

// Returns a copy of an RTP packet with a one-byte-header extension element
// (id, 2 bytes: transport sequence number) added. Packets libdatachannel builds use
// the one-byte (0xBEDE) form; anything else is returned unchanged.
rtc::message_ptr withTransportSeq(const rtc::message_ptr &in, uint8_t id, uint16_t seq) {
	const auto *b = reinterpret_cast<const uint8_t *>(in->data());
	size_t n = in->size();
	if (n < 12)
		return in;
	size_t cc = b[0] & 0x0f;
	size_t hdr = 12 + 4 * cc;
	if (n < hdr)
		return in;
	bool x = (b[0] >> 4) & 1;
	size_t extStart = hdr, payload = hdr;
	std::vector<uint8_t> elems;
	if (x) {
		if (n < hdr + 4)
			return in;
		uint16_t profile = uint16_t(b[hdr] << 8 | b[hdr + 1]);
		size_t words = size_t(b[hdr + 2] << 8 | b[hdr + 3]);
		extStart = hdr + 4;
		payload = extStart + 4 * words;
		if (profile != 0xBEDE || payload > n)
			return in;
		size_t i = extStart, used = extStart;
		while (i < payload) {
			uint8_t h = b[i];
			if (h == 0) { // padding
				i++;
				continue;
			}
			uint8_t eid = h >> 4;
			size_t len = size_t(h & 0x0f) + 1;
			if (eid == 15 || i + 1 + len > payload)
				break;
			if (eid == id) // already stamped (shouldn't happen): leave it
				return in;
			i += 1 + len;
			used = i;
		}
		elems.assign(b + extStart, b + used);
	}
	elems.push_back(uint8_t(id << 4 | 1));
	elems.push_back(uint8_t(seq >> 8));
	elems.push_back(uint8_t(seq));
	while (elems.size() % 4)
		elems.push_back(0);

	rtc::binary out;
	out.reserve(hdr + 4 + elems.size() + (n - payload));
	for (size_t i = 0; i < hdr; i++)
		out.push_back(std::byte(b[i]));
	out[0] = std::byte(b[0] | 0x10); // X bit
	size_t words = elems.size() / 4;
	out.push_back(std::byte(0xBE));
	out.push_back(std::byte(0xDE));
	out.push_back(std::byte(words >> 8));
	out.push_back(std::byte(words & 0xff));
	for (uint8_t e : elems)
		out.push_back(std::byte(e));
	for (size_t i = payload; i < n; i++)
		out.push_back(std::byte(b[i]));
	auto m = rtc::make_message(std::move(out), in->type);
	m->dscp = in->dscp;
	return m;
}

} // namespace

WifiLink::WifiLink(Options opts, CongestionCallback cb)
    : mOpts(opts), mCb(std::move(cb)), mTokens(opts.burstBytes), mLastRefillUs(nowUs()),
      mHistory(std::max<size_t>(opts.history, 64)), mSendUs(65536, 0) {
	mThread = std::thread([this] { paceLoop(); });
}

WifiLink::~WifiLink() {
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mStop = true;
	}
	mCv.notify_all();
	if (mThread.joinable())
		mThread.join();
}

void WifiLink::refill() {
	int64_t now = nowUs();
	mTokens = std::min(mOpts.burstBytes, mTokens + double(now - mLastRefillUs) / 1e6 * mOpts.paceBytesPerSec);
	mLastRefillUs = now;
}

void WifiLink::transmitLocked(const rtc::message_ptr &pkt, const rtc::message_callback &send) {
	rtc::message_ptr out = pkt;
	if (mOpts.twccExtId) {
		uint16_t seq = mTwccSeq++;
		out = withTransportSeq(pkt, mOpts.twccExtId, seq);
		mSendUs[seq] = nowUs();
	}
	try {
		send(out);
	} catch (...) {
	}
}

void WifiLink::outgoing(rtc::message_vector &messages, const rtc::message_callback &send) {
	std::lock_guard<std::mutex> lock(mMutex);
	mSend = send;
	refill();
	rtc::message_vector passthrough;
	for (auto &m : messages) {
		if (m->type != rtc::Message::Binary || m->size() < 12) { // RTCP (sender reports) is never held back
			passthrough.push_back(std::move(m));
			continue;
		}
		uint16_t seq = rtpSeq(m);
		auto &slot = mHistory[seq % mHistory.size()];
		slot.seq = seq;
		slot.pkt = m;
		if (!mOpts.pace || (mQueue.empty() && mTokens > 0)) {
			mTokens -= double(m->size());
			transmitLocked(m, send);
		} else {
			mQueue.push_back(m);
		}
	}
	messages = std::move(passthrough);
	if (!mQueue.empty())
		mCv.notify_all();
}

void WifiLink::paceLoop() {
	std::unique_lock<std::mutex> lock(mMutex);
	while (!mStop) {
		if (mQueue.empty()) {
			mCv.wait(lock);
			continue;
		}
		refill();
		if (mTokens <= 0) {
			double waitS = (-mTokens + 1200.0) / mOpts.paceBytesPerSec;
			mCv.wait_for(lock, std::chrono::microseconds(int64_t(waitS * 1e6) + 1));
			continue;
		}
		while (!mQueue.empty() && mTokens > 0) {
			auto m = std::move(mQueue.front());
			mQueue.pop_front();
			mTokens -= double(m->size());
			if (mSend)
				transmitLocked(m, mSend);
		}
	}
}

void WifiLink::incoming(rtc::message_vector &messages, const rtc::message_callback &send) {
	for (const auto &m : messages) {
		if (m->type != rtc::Message::Control)
			continue;
		handleRtcp(reinterpret_cast<const uint8_t *>(m->data()), m->size(), send);
	}
}

void WifiLink::handleRtcp(const uint8_t *p, size_t n, const rtc::message_callback &send) {
	uint64_t signal = 0;
	size_t off = 0;
	std::unique_lock<std::mutex> lock(mMutex);
	while (off + 4 <= n) {
		uint8_t count = p[off] & 0x1f;
		uint8_t pt = p[off + 1];
		size_t len = (size_t(p[off + 2]) << 8 | p[off + 3]) * 4 + 4;
		if (off + len > n || len < 8)
			break;
		const uint8_t *b = p + off;
		auto be32 = [](const uint8_t *q) { return uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3]; };

		if (pt == 201 || pt == 200) { // receiver / sender report blocks about our video
			size_t first = pt == 201 ? 8 : 28;
			for (uint8_t i = 0; i < count && first + 24 * (i + 1) <= len; i++) {
				const uint8_t *r = b + first + 24 * i;
				if (be32(r) != mOpts.ssrc)
					continue;
				mLossPct = r[4] / 256.0 * 100.0;
				int32_t cum = int32_t(uint32_t(r[5]) << 16 | uint32_t(r[6]) << 8 | r[7]);
				if (cum & 0x800000)
					cum |= int32_t(0xff000000);
				if (mHaveCumLost && cum > mLastCumLost)
					signal += uint64_t(cum - mLastCumLost); // lost for good, even after re-sends
				mLastCumLost = cum;
				mHaveCumLost = true;
			}
		} else if (pt == 205 && len >= 12 && be32(b + 8) == mOpts.ssrc) {
			if (count == 1) { // generic NACK: re-send from history with fresh transport numbers
				uint64_t nacked = 0;
				for (size_t f = 12; f + 4 <= len; f += 4) {
					uint16_t pid = uint16_t(b[f] << 8 | b[f + 1]);
					uint16_t blp = uint16_t(b[f + 2] << 8 | b[f + 3]);
					for (int k = -1; k < 16; k++) {
						if (k >= 0 && !((blp >> k) & 1))
							continue;
						uint16_t seq = uint16_t(pid + (k + 1));
						nacked++;
						auto &slot = mHistory[seq % mHistory.size()];
						if (slot.pkt && slot.seq == seq) {
							transmitLocked(slot.pkt, send);
							mRetransmits++;
						}
					}
				}
				signal += nacked / 2; // needing a re-send hints at airtime trouble, not proof of congestion
			} else if (count == 15) {
				handleTwcc(b, len, signal);
			}
		}
		off += len;
	}
	lock.unlock();
	if (signal && mFeedback && mCb)
		mCb(signal);
}

void WifiLink::sample(uint16_t seq, int64_t arrivalUs, double &sumQueue, int &count) {
	int64_t sent = mSendUs[seq];
	if (!sent)
		return;
	int64_t owd = arrivalUs - sent; // includes an unknown, slowly drifting clock offset
	int64_t now = nowUs();
	// Sliding 10 s minimum = the delay of an empty queue; anything above it is queueing.
	while (!mMinWindow.empty() && mMinWindow.back().second >= owd)
		mMinWindow.pop_back();
	mMinWindow.emplace_back(now, owd);
	while (!mMinWindow.empty() && now - mMinWindow.front().first > 10'000'000)
		mMinWindow.pop_front();
	int64_t base = mMinWindow.front().second;
	sumQueue += double(owd - base) / 1000.0;
	count++;
}

void WifiLink::handleTwcc(const uint8_t *b, size_t len, uint64_t &signal) {
	if (len < 20)
		return;
	mFeedbacks++;
	uint16_t base = uint16_t(b[12] << 8 | b[13]);
	uint16_t count = uint16_t(b[14] << 8 | b[15]);
	int32_t ref = int32_t(uint32_t(b[16]) << 16 | uint32_t(b[17]) << 8 | b[18]);
	if (ref & 0x800000)
		ref |= int32_t(0xff000000);
	size_t off = 20;
	std::vector<uint8_t> status;
	status.reserve(count);
	while (status.size() < count && off + 2 <= len) {
		uint16_t chunk = uint16_t(b[off] << 8 | b[off + 1]);
		off += 2;
		if (!(chunk & 0x8000)) { // run length
			uint8_t sym = (chunk >> 13) & 3;
			for (uint16_t i = 0; i < (chunk & 0x1fff) && status.size() < count; i++)
				status.push_back(sym);
		} else if (!(chunk & 0x4000)) { // 14 one-bit symbols
			for (int i = 13; i >= 0 && status.size() < count; i--)
				status.push_back((chunk >> i) & 1);
		} else { // 7 two-bit symbols
			for (int i = 6; i >= 0 && status.size() < count; i--)
				status.push_back((chunk >> (2 * i)) & 3);
		}
	}
	int64_t t = int64_t(ref) * 64000; // µs, receiver clock
	double sumQueue = 0;
	int samples = 0, received = 0;
	for (size_t i = 0; i < status.size(); i++) {
		uint16_t seq = uint16_t(base + i);
		if (status[i] == 1) {
			if (off + 1 > len)
				break;
			t += int64_t(b[off]) * 250;
			off += 1;
		} else if (status[i] == 2) {
			if (off + 2 > len)
				break;
			t += int64_t(int16_t(b[off] << 8 | b[off + 1])) * 250;
			off += 2;
		} else {
			continue;
		}
		received++;
		sample(seq, t, sumQueue, samples);
	}
	if (!samples)
		return;
	double q = sumQueue / samples;
	mQueueMs = mQueueMs * 0.7 + q * 0.3;
	// Queueing beyond ~30 ms means the access point is buffering more than it sends:
	// report a little loss so the console's encoder steps down before packets drop.
	if (mQueueMs > 30) {
		double frac = std::clamp((mQueueMs - 30) / 100.0, 0.02, 0.05);
		signal += uint64_t(std::ceil(received * frac));
	}
}

LinkStats WifiLink::stats() {
	std::lock_guard<std::mutex> lock(mMutex);
	return {mQueueMs, mLossPct, mRetransmits, mFeedbacks};
}

} // namespace duo
