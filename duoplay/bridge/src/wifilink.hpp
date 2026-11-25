// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <rtc/rtc.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace duo {

struct LinkStats {
	double queueMs = 0;       // how much the Wi-Fi path is buffering right now (from TWCC feedback)
	double lossPct = 0;       // unrecovered loss the phone reported
	uint64_t retransmits = 0; // packets re-sent on request
	uint64_t feedbacks = 0;   // transport feedback reports received
};

// Last handler on the video track; everything that leaves for the phone goes through here.
//
//  - Pacing: frames go out at once; only the part of a burst above `burstBytes`
//    (a big keyframe) is spread at `paceBytesPerSec`, so a shallow access-point queue
//    doesn't drop the tail.
//  - Retransmission: the last `history` packets are kept and re-sent on NACK.
//  - Transport-wide congestion control: every transmission (re-sends included) carries
//    a fresh transport sequence number; the phone reports when each one arrived. From
//    that we track queueing delay on the Wi-Fi path and, when it builds up, tell the
//    console to lower its bitrate before packets start to drop.
class WifiLink final : public rtc::MediaHandler {
public:
	struct Options {
		uint32_t ssrc = 0;
		uint8_t twccExtId = 0; // 0 = don't stamp transport sequence numbers
		bool pace = true;
		double paceBytesPerSec = 8e6;
		double burstBytes = 64000;
		size_t history = 4096;
	};
	// Called with a loss-equivalent figure for the console's congestion report.
	using CongestionCallback = std::function<void(uint64_t lossSignal)>;

	WifiLink(Options opts, CongestionCallback cb);
	~WifiLink() override;

	void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override;
	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;
	void setCongestionFeedback(bool on) { mFeedback = on; }
	LinkStats stats();

private:
	void transmitLocked(const rtc::message_ptr &pkt, const rtc::message_callback &send);
	void paceLoop();
	void refill();
	void handleRtcp(const uint8_t *p, size_t n, const rtc::message_callback &send);
	void handleTwcc(const uint8_t *b, size_t len, uint64_t &signal);
	void sample(uint16_t seq, int64_t arrivalUs, double &sumQueue, int &count);

	Options mOpts;
	CongestionCallback mCb;
	std::atomic<bool> mFeedback{true};

	std::mutex mMutex;
	std::condition_variable mCv;
	std::deque<rtc::message_ptr> mQueue;
	rtc::message_callback mSend;
	double mTokens;
	int64_t mLastRefillUs;
	bool mStop = false;
	std::thread mThread;

	struct Stored {
		uint16_t seq = 0;
		rtc::message_ptr pkt;
	};
	std::vector<Stored> mHistory;

	uint16_t mTwccSeq = 1;
	std::vector<int64_t> mSendUs; // by transport sequence number

	// queueing-delay estimate
	std::deque<std::pair<int64_t, int64_t>> mMinWindow; // (time sampled, one-way delay), increasing delay
	double mQueueMs = 0;
	int32_t mLastCumLost = 0;
	bool mHaveCumLost = false;
	double mLossPct = 0;
	uint64_t mRetransmits = 0;
	uint64_t mFeedbacks = 0;
};

} // namespace duo
