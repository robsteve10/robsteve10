// SPDX-License-Identifier: AGPL-3.0-only
#include "peer.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <future>
#include <thread>

using json = nlohmann::json;

namespace duo {

namespace {

constexpr int kPtH264High = 102;
constexpr int kPtH264Base = 106;
constexpr int kPtH265 = 104;
constexpr int kPtOpus = 111;
constexpr int kExtPlayoutDelay = 6;
const char *kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";

// Pulls Wi-Fi loss out of the phone's RTCP feedback for the video stream:
//  - receiver reports: packets that never arrived, even after retransmission
//  - generic NACKs: packets that had to be re-sent (a sign of airtime trouble)
class LossMonitor final : public rtc::MediaHandler {
public:
	LossMonitor(uint32_t ssrc, std::function<void(uint64_t lost, uint64_t nacked)> cb) : mSsrc(ssrc), mCb(std::move(cb)) {}

	void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
		for (const auto &m : messages) {
			if (m->type != rtc::Message::Control)
				continue;
			parse(reinterpret_cast<const uint8_t *>(m->data()), m->size());
		}
	}

private:
	void parse(const uint8_t *p, size_t n) {
		uint64_t lost = 0, nacked = 0;
		size_t off = 0;
		while (off + 4 <= n) {
			uint8_t count = p[off] & 0x1f;
			uint8_t pt = p[off + 1];
			size_t len = (size_t(p[off + 2]) << 8 | p[off + 3]) * 4 + 4;
			if (off + len > n || len < 8)
				break;
			const uint8_t *b = p + off;
			if (pt == 201 || pt == 200) { // RR or SR: report blocks start after sender SSRC (+ sender info for SR)
				size_t blocks = (pt == 201 ? 8 : 28);
				for (uint8_t i = 0; i < count && blocks + 24 <= len; i++, blocks += 24) {
					const uint8_t *r = b + blocks;
					uint32_t ssrc = uint32_t(r[0]) << 24 | uint32_t(r[1]) << 16 | uint32_t(r[2]) << 8 | r[3];
					if (ssrc != mSsrc)
						continue;
					int32_t cum = int32_t(uint32_t(r[5]) << 16 | uint32_t(r[6]) << 8 | r[7]);
					if (cum & 0x800000)
						cum |= int32_t(0xff000000);
					if (mHaveCum && cum > mLastCum)
						lost += uint64_t(cum - mLastCum);
					mLastCum = cum;
					mHaveCum = true;
				}
			} else if (pt == 205 && count == 1 && len >= 16) { // RTPFB generic NACK
				uint32_t media = uint32_t(b[8]) << 24 | uint32_t(b[9]) << 16 | uint32_t(b[10]) << 8 | b[11];
				if (media == mSsrc) {
					for (size_t f = 12; f + 4 <= len; f += 4) {
						uint16_t blp = uint16_t(b[f + 2] << 8 | b[f + 3]);
						nacked += 1;
						while (blp) {
							nacked += blp & 1;
							blp >>= 1;
						}
					}
				}
			}
			off += len;
		}
		if (lost || nacked)
			mCb(lost, nacked);
	}

	uint32_t mSsrc;
	std::function<void(uint64_t, uint64_t)> mCb;
	int32_t mLastCum = 0;
	bool mHaveCum = false;
};

// Token bucket that lets ordinary frames through untouched and only spreads out
// bursts bigger than `burstBytes` (keyframes). A gigabit-wired bridge can dump a
// 150 KB keyframe into the access point faster than Wi-Fi can drain it, and a
// shallow AP queue then drops the tail - which costs a retransmission round trip,
// or a whole new keyframe.
class BurstSmoother final : public rtc::MediaHandler {
public:
	BurstSmoother(double bytesPerSecond, double burstBytes)
	    : mRate(bytesPerSecond), mBurst(burstBytes), mTokens(burstBytes), mLast(Clock::now()) {
		mThread = std::thread([this] { run(); });
	}
	~BurstSmoother() override {
		{
			std::lock_guard<std::mutex> lock(mMutex);
			mStop = true;
		}
		mCv.notify_all();
		if (mThread.joinable())
			mThread.join();
	}

	void outgoing(rtc::message_vector &messages, const rtc::message_callback &send) override {
		std::lock_guard<std::mutex> lock(mMutex);
		refill();
		mSend = send;
		rtc::message_vector now;
		for (auto &m : messages) {
			if (m->type != rtc::Message::Binary) { // RTCP etc. never waits
				now.push_back(std::move(m));
				continue;
			}
			if (mQueue.empty() && mTokens > 0) {
				mTokens -= double(m->size());
				now.push_back(std::move(m));
			} else {
				mQueue.push_back(std::move(m));
			}
		}
		messages = std::move(now);
		if (!mQueue.empty())
			mCv.notify_all();
	}

private:
	void refill() {
		auto t = Clock::now();
		double dt = std::chrono::duration<double>(t - mLast).count();
		mLast = t;
		mTokens = std::min(mBurst, mTokens + dt * mRate);
	}

	void run() {
		std::unique_lock<std::mutex> lock(mMutex);
		while (!mStop) {
			if (mQueue.empty()) {
				mCv.wait(lock);
				continue;
			}
			refill();
			if (mTokens <= 0) {
				auto wait = std::chrono::duration<double>((-mTokens + 1200.0) / mRate);
				mCv.wait_for(lock, std::chrono::duration_cast<std::chrono::microseconds>(wait));
				continue;
			}
			while (!mQueue.empty() && mTokens > 0) {
				auto m = std::move(mQueue.front());
				mQueue.pop_front();
				mTokens -= double(m->size());
				auto send = mSend;
				lock.unlock();
				try {
					if (send)
						send(m);
				} catch (...) {
				}
				lock.lock();
			}
		}
	}

	double mRate, mBurst, mTokens;
	Clock::time_point mLast;
	std::mutex mMutex;
	std::condition_variable mCv;
	std::deque<rtc::message_ptr> mQueue;
	rtc::message_callback mSend;
	bool mStop = false;
	std::thread mThread;
};

uint32_t randomSsrc() {
	auto s = randomId(4);
	return uint32_t(strtoul(s.c_str(), nullptr, 16)) | 1u;
}

} // namespace

std::shared_ptr<Peer> Peer::create(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts,
                                   ClosedCallback onClosed) {
	auto p = std::shared_ptr<Peer>(new Peer(hub, net, opts, std::move(onClosed)));
	p->setup();
	return p;
}

Peer::Peer(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts, ClosedCallback onClosed)
    : mHub(hub), mNet(net), mOpts(opts), mOnClosed(std::move(onClosed)), mId(randomId(8)) {
	mCreatedMs = nowMs();
	mLastHeardMs = nowMs();
}

Peer::~Peer() { close(); }

void Peer::setup() {
	rtc::Configuration cfg;
	// One UDP port for every phone: easy to allow through a firewall or publish from Docker.
	cfg.enableIceUdpMux = true;
	cfg.portRangeBegin = mNet.udpPort;
	cfg.portRangeEnd = mNet.udpPort;
	if (mNet.bindAddress)
		cfg.bindAddress = *mNet.bindAddress;
	// We create the offer ourselves, once every track and channel is in place.
	cfg.disableAutoNegotiation = true;
	// LAN only: no STUN/TURN servers, host candidates are all a home network needs.
	mPc = std::make_shared<rtc::PeerConnection>(cfg);

	mHevc = mHub.hevc();
	const std::string cname = "duoplay";
	const std::string msid = "duoplay-" + mId;

	// --- video ---
	uint32_t vssrc = randomSsrc();
	rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
	if (mHevc) {
		video.addH265Codec(kPtH265, "level-id=93;profile-id=1;tier-flag=0;tx-mode=SRST");
	} else {
		video.addH264Codec(kPtH264High, "profile-level-id=640c1f;packetization-mode=1;level-asymmetry-allowed=1");
		video.addH264Codec(kPtH264Base, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
	}
	video.addExtMap(rtc::Description::Entry::ExtMap(kExtPlayoutDelay, kPlayoutDelayUri));
	video.addSSRC(vssrc, cname, msid, "video");
	mVideo = mPc->addTrack(video);

	mVideoCfg = std::make_shared<rtc::RtpPacketizationConfig>(vssrc, cname, mHevc ? kPtH265 : kPtH264High,
	                                                          rtc::H264RtpPacketizer::ClockRate);
	// Playout-delay extension tells the phone how much jitter buffering it may add.
	mVideoCfg->playoutDelayId = kExtPlayoutDelay;
	mVideoCfg->playoutDelayMin = 0;
	mVideoCfg->playoutDelayMax = mOpts.latencyMode == 0 ? 0 : (mOpts.latencyMode == 1 ? 4 : 15); // x10 ms

	std::shared_ptr<rtc::RtpPacketizer> vpack;
	if (mHevc)
		vpack = std::make_shared<rtc::H265RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, mVideoCfg);
	else
		vpack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, mVideoCfg);
	vpack->addToChain(std::make_shared<rtc::RtcpSrReporter>(mVideoCfg));
	// Large history: a 1080p keyframe alone can be a couple of hundred packets, and the
	// phone must be able to NACK any of them.
	vpack->addToChain(std::make_shared<rtc::RtcpNackResponder>(4096));
	std::weak_ptr<Peer> weak = weak_from_this();
	vpack->addToChain(std::make_shared<rtc::PliHandler>([weak] {
		if (auto self = weak.lock())
			self->mHub.requestKeyframe("phone asked (PLI)");
	}));
	vpack->addToChain(std::make_shared<LossMonitor>(vssrc, [weak](uint64_t lost, uint64_t nacked) {
		if (auto self = weak.lock())
			self->onRtcpLoss(lost, nacked);
	}));
	if (mOpts.smoothBursts) {
		// Let normal frames out instantly; pace only the excess at ~6x the stream rate.
		double bytesPerSec = std::max(60e6, mOpts.expectedKbps * 1000.0 * 6) / 8.0;
		double burst = std::max(48000.0, mOpts.expectedKbps * 1000.0 / 8.0 / 60.0 * 3); // ~3 average frames
		vpack->addToChain(std::make_shared<BurstSmoother>(bytesPerSec, burst));
	}
	mVideo->setMediaHandler(vpack);
	mVideo->onOpen([weak] {
		if (auto self = weak.lock()) {
			self->mVideoOpen = true;
			self->mHub.requestKeyframe("video track open");
		}
	});
	mVideo->onClosed([weak] {
		if (auto self = weak.lock())
			self->mVideoOpen = false;
	});

	// --- audio ---
	uint32_t assrc = randomSsrc();
	rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
	audio.addOpusCodec(kPtOpus, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
	audio.addSSRC(assrc, cname, msid, "audio");
	mAudio = mPc->addTrack(audio);
	mAudioCfg = std::make_shared<rtc::RtpPacketizationConfig>(assrc, cname, kPtOpus, rtc::OpusRtpPacketizer::DefaultClockRate);
	auto apack = std::make_shared<rtc::OpusRtpPacketizer>(mAudioCfg);
	apack->addToChain(std::make_shared<rtc::RtcpSrReporter>(mAudioCfg));
	mAudio->setMediaHandler(apack);
	mAudio->onOpen([weak] {
		if (auto self = weak.lock())
			self->mAudioOpen = true;
	});

	// --- data channels ---
	// Controller input: unordered and never retransmitted. Every packet carries the
	// complete pad state, so a lost one is healed by the next (sent at least every
	// 50 ms while anything is held) - a retransmission would only arrive stale.
	rtc::DataChannelInit inputInit;
	inputInit.reliability.unordered = true;
	inputInit.reliability.maxRetransmits = 0;
	mInput = mPc->createDataChannel("input", inputInit);
	mInput->onMessage([weak](rtc::message_variant data) {
		auto self = weak.lock();
		if (!self)
			return;
		if (auto *bin = std::get_if<rtc::binary>(&data))
			self->onInput(*bin);
	});

	mControl = mPc->createDataChannel("control");
	mControl->onOpen([weak] {
		if (auto self = weak.lock())
			self->mControlOpen = true;
	});
	mControl->onMessage([weak](rtc::message_variant data) {
		auto self = weak.lock();
		if (!self)
			return;
		if (auto *s = std::get_if<std::string>(&data))
			self->onControl(*s);
	});

	mPc->onStateChange([weak](rtc::PeerConnection::State state) {
		auto self = weak.lock();
		if (!self)
			return;
		self->mPcState = int(state);
		LOGI("peer", "%s: %s", self->mId.c_str(),
		     state == rtc::PeerConnection::State::Connected      ? "connected"
		     : state == rtc::PeerConnection::State::Disconnected ? "disconnected (may recover)"
		     : state == rtc::PeerConnection::State::Failed       ? "failed"
		     : state == rtc::PeerConnection::State::Closed       ? "closed"
		                                                         : "connecting");
		if (state == rtc::PeerConnection::State::Disconnected)
			self->mDisconnectedSinceMs = nowMs();
		else
			self->mDisconnectedSinceMs = 0;
		if (state == rtc::PeerConnection::State::Connected && !self->mAttached.exchange(true)) {
			self->mHub.addPeer(self);
		}
		if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
			std::thread([self] { self->close(); }).detach();
		}
	});
}

bool Peer::createOffer(std::string &sdp, std::string &err) {
	auto gathered = std::make_shared<std::promise<void>>();
	auto done = std::make_shared<std::atomic<bool>>(false);
	mPc->onGatheringStateChange([gathered, done](rtc::PeerConnection::GatheringState s) {
		if (s == rtc::PeerConnection::GatheringState::Complete && !done->exchange(true))
			gathered->set_value();
	});
	mPc->setLocalDescription(rtc::Description::Type::Offer);
	if (mPc->gatheringState() != rtc::PeerConnection::GatheringState::Complete &&
	    gathered->get_future().wait_for(std::chrono::seconds(3)) != std::future_status::ready)
		LOGW("peer", "ICE gathering slow; offering what we have");
	auto desc = mPc->localDescription();
	if (!desc) {
		err = "Could not create offer";
		return false;
	}
	sdp = std::string(*desc);

	// Advertise extra addresses (e.g. the Docker host's LAN IP) as host candidates.
	if (!mNet.announceIps.empty()) {
		std::string extra;
		int n = 0;
		for (const auto &ip : mNet.announceIps) {
			++n;
			extra += "a=candidate:duo" + std::to_string(n) + " 1 UDP " + std::to_string(2130706431 - n) + " " + ip +
			         " " + std::to_string(mNet.udpPort) + " typ host\r\n";
		}
		size_t pos = sdp.find("m=");
		while (pos != std::string::npos) {
			size_t next = sdp.find("\r\nm=", pos + 1);
			size_t insertAt = next == std::string::npos ? sdp.size() : next + 2;
			sdp.insert(insertAt, extra);
			if (next == std::string::npos)
				break;
			pos = sdp.find("m=", insertAt + extra.size());
		}
	}
	return true;
}

bool Peer::applyAnswer(const std::string &sdp, std::string &err) {
	try {
		mPc->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
	} catch (const std::exception &e) {
		err = std::string("Bad answer: ") + e.what();
		return false;
	}
	// Send with whichever of our payload types the phone picked first.
	auto remote = mPc->remoteDescription();
	if (remote) {
		for (int i = 0; i < remote->mediaCount(); i++) {
			auto entry = remote->media(i);
			auto *media = std::get_if<rtc::Description::Media *>(&entry);
			if (!media || (*media)->type() != "video")
				continue;
			for (int pt : (*media)->payloadTypes()) {
				if (pt == kPtH264High || pt == kPtH264Base || pt == kPtH265) {
					mVideoCfg->payloadType = uint8_t(pt);
					LOGI("peer", "%s: phone accepted video payload type %d (%s)", mId.c_str(), pt,
					     pt == kPtH265 ? "H.265" : pt == kPtH264High ? "H.264 high" : "H.264 baseline");
					break;
				}
			}
		}
	}
	return true;
}

void Peer::close() {
	if (mClosed.exchange(true))
		return;
	mHub.removePeer(mId);
	try {
		if (mPc)
			mPc->close();
	} catch (...) {
	}
	if (mOnClosed)
		mOnClosed(mId);
}

bool Peer::expired(int64_t now) const {
	if (mClosed)
		return true;
	if (!mAttached && now - mCreatedMs > 20000)
		return true; // never finished connecting
	int64_t since = mDisconnectedSinceMs;
	if (since && now - since > 15000)
		return true; // disconnected and not coming back
	return false;
}

std::string Peer::describe() const {
	return mId + (mPaused ? " (paused)" : "");
}

void Peer::heard() {
	mLastHeardMs = nowMs();
	if (mPaused.exchange(false)) {
		LOGI("peer", "%s: phone is back, resuming", mId.c_str());
		mNeedKeyframe = true;
		mHub.requestKeyframe("phone resumed");
	}
}

void Peer::sendVideo(const std::shared_ptr<const Bytes> &frame, uint32_t ts90k, bool keyframe) {
	if (!mVideoOpen || mClosed)
		return;
	if (mHub.hevc() != mHevc) {
		if (!mCodecWarned.exchange(true))
			sendControl(R"({"t":"renegotiate","reason":"codec changed"})");
		return;
	}
	// The phone sends a heartbeat several times a second while its page is alive. If it
	// goes quiet (app backgrounded or suspended), stop spending Wi-Fi airtime on it.
	int64_t now = nowMs();
	if (!mPaused && now - mLastHeardMs > 3000) {
		mPaused = true;
		LOGI("peer", "%s: phone went quiet, pausing its stream", mId.c_str());
	}
	if (mPaused)
		return;
	if (mNeedKeyframe) {
		if (!keyframe) {
			if (now - mLastKeyRequestMs > 400) {
				mLastKeyRequestMs = now;
				mHub.requestKeyframe("viewer waiting for keyframe");
			}
			return;
		}
		mNeedKeyframe = false;
	}
	try {
		mVideo->sendFrame(frame->data(), frame->size(), rtc::FrameInfo(ts90k));
	} catch (const std::exception &e) {
		LOGD("peer", "%s: video send failed: %s", mId.c_str(), e.what());
	}
}

void Peer::sendAudio(const uint8_t *data, size_t size, uint32_t ts48k) {
	if (!mAudioOpen || mClosed || mPaused)
		return;
	try {
		mAudio->sendFrame(reinterpret_cast<const std::byte *>(data), size, rtc::FrameInfo(ts48k));
	} catch (const std::exception &e) {
		LOGD("peer", "%s: audio send failed: %s", mId.c_str(), e.what());
	}
}

void Peer::sendControl(const std::string &msg) {
	if (!mControlOpen || mClosed)
		return;
	try {
		mControl->send(msg);
	} catch (...) {
	}
}

static int16_t rdI16(const uint8_t *p) { return int16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8); }
static uint16_t rdU16(const uint8_t *p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
static uint32_t rdU32(const uint8_t *p) {
	return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Input packet (little endian, 30 bytes):
//   0 u8  version (1)      1 u8 flags          2 u16 sequence
//   4 u32 buttons (chiaki bitmask)
//   8 u8  L2   9 u8 R2    10 i16 LX  12 i16 LY  14 i16 RX  16 i16 RY
//  18 touch 0: i8 id, u8 -, u16 x, u16 y       24 touch 1 (same)
void Peer::onInput(const rtc::binary &msg) {
	heard();
	if (msg.size() < 30)
		return;
	const auto *p = reinterpret_cast<const uint8_t *>(msg.data());
	if (p[0] != 1)
		return;
	uint16_t seq = rdU16(p + 2);
	{
		std::lock_guard<std::mutex> lock(mInputMutex);
		// Unordered channel: an older snapshot arriving late must not undo a newer one.
		if (mHaveInputSeq && int16_t(seq - mLastInputSeq) <= 0 && int16_t(seq - mLastInputSeq) > -1000)
			return;
		mLastInputSeq = seq;
		mHaveInputSeq = true;
	}
	ChiakiControllerState s;
	chiaki_controller_state_set_idle(&s);
	s.buttons = rdU32(p + 4);
	s.l2_state = p[8];
	s.r2_state = p[9];
	s.left_x = rdI16(p + 10);
	s.left_y = rdI16(p + 12);
	s.right_x = rdI16(p + 14);
	s.right_y = rdI16(p + 16);
	for (int i = 0; i < 2; i++) {
		const uint8_t *t = p + 18 + i * 6;
		s.touches[i].id = int8_t(t[0]);
		s.touches[i].x = rdU16(t + 2);
		s.touches[i].y = rdU16(t + 4);
	}
	mHub.peerInput(mId, s);
}

void Peer::onControl(const std::string &msg) {
	heard();
	json j;
	try {
		j = json::parse(msg);
	} catch (...) {
		return;
	}
	std::string t = j.value("t", "");
	if (t == "ping") {
		sendControl(json{{"t", "pong"}, {"id", j.value("id", 0)}}.dump());
	} else if (t == "keyframe") {
		mHub.requestKeyframe("phone saw a frozen picture");
	} else if (t == "settings") {
		mOpts.adaptiveBitrate = j.value("adaptive", mOpts.adaptiveBitrate);
	}
}

void Peer::onRtcpLoss(uint64_t lost, uint64_t nacked) {
	if (!mOpts.adaptiveBitrate)
		return;
	// Unrecovered loss counts fully; packets that needed a resend count half. Random
	// one-off Wi-Fi drops barely register, sustained airtime trouble backs the encoder off.
	uint64_t signal = lost + nacked / 2;
	if (signal)
		mHub.peerLoss(mId, signal);
}

} // namespace duo
