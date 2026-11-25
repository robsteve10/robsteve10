// SPDX-License-Identifier: AGPL-3.0-only
#include "peer.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>
#include <future>
#include <thread>

using json = nlohmann::json;

namespace duo {

namespace {

constexpr int kPtH264High = 102;
constexpr int kPtH264Base = 106;
constexpr int kPtOpus = 111;
constexpr int kPtRed = 63;
constexpr int kExtTransportCc = 3;
constexpr int kExtPlayoutDelay = 6;
const char *kTransportCcUri = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
const char *kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";

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
	// One UDP port for every phone: easy to allow through a firewall.
	cfg.enableIceUdpMux = true;
	cfg.portRangeBegin = mNet.udpPort;
	cfg.portRangeEnd = mNet.udpPort;
	if (mNet.bindAddress)
		cfg.bindAddress = *mNet.bindAddress;
	cfg.disableAutoNegotiation = true; // we create the offer once every track and channel exists
	// LAN only: no STUN/TURN, host candidates are all a home network needs.
	mPc = std::make_shared<rtc::PeerConnection>(cfg);

	mHevc = mHub.hevc();
	mMse = mHevc || mOpts.mediaPlayer; // Safari plays HEVC (and HDR) reliably only in its media player
	const std::string cname = "duoplay";
	std::weak_ptr<Peer> weak = weak_from_this();

	if (!mMse) {
		// ---- video over RTP (H.264) ----
		uint32_t vssrc = randomSsrc();
		rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
		video.addH264Codec(kPtH264High, "profile-level-id=640c1f;packetization-mode=1;level-asymmetry-allowed=1");
		video.addH264Codec(kPtH264Base, "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1");
		for (int pt : {kPtH264High, kPtH264Base})
			if (auto *map = video.rtpMap(pt))
				map->addFeedback("transport-cc");
		video.addExtMap(rtc::Description::Entry::ExtMap(kExtTransportCc, kTransportCcUri));
		video.addExtMap(rtc::Description::Entry::ExtMap(kExtPlayoutDelay, kPlayoutDelayUri));
		// Separate stream ids for picture and sound: the phone then doesn't hold the
		// picture back to lip-sync it with the audio jitter buffer.
		video.addSSRC(vssrc, cname, "duoplay-video-" + mId, "video");
		mVideo = mPc->addTrack(video);

		mVideoCfg = std::make_shared<rtc::RtpPacketizationConfig>(vssrc, cname, kPtH264High, rtc::H264RtpPacketizer::ClockRate);
		mVideoCfg->playoutDelayId = kExtPlayoutDelay;
		mVideoCfg->playoutDelayMin = 0;
		mVideoCfg->playoutDelayMax = mOpts.latencyMode == 0 ? 0 : (mOpts.latencyMode == 1 ? 4 : 15); // x10 ms

		auto pack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, mVideoCfg);
		pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(mVideoCfg));
		pack->addToChain(std::make_shared<rtc::PliHandler>([weak] {
			if (auto self = weak.lock())
				self->mHub.requestKeyframe("phone asked (PLI)");
		}));
		WifiLink::Options lo;
		lo.ssrc = vssrc;
		lo.twccExtId = kExtTransportCc;
		lo.pace = mOpts.smoothBursts;
		// Normal frames leave at once; only bursts above ~3 average frames are spread out,
		// at 6x the stream's rate.
		lo.paceBytesPerSec = std::max(60e6, mOpts.expectedKbps * 1000.0 * 6) / 8.0;
		lo.burstBytes = std::max(48000.0, mOpts.expectedKbps * 1000.0 / 8.0 / std::max(1, mOpts.fps) * 3);
		lo.history = 4096;
		mLink = std::make_shared<WifiLink>(lo, [weak](uint64_t signal) {
			if (auto self = weak.lock())
				if (self->mOpts.adaptiveBitrate)
					self->mHub.peerLoss(self->mId, signal);
		});
		mLink->setCongestionFeedback(mOpts.adaptiveBitrate);
		pack->addToChain(mLink);
		mVideo->setMediaHandler(pack);
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
	} else {
		// ---- video as MP4 fragments for Safari's media player (HEVC / HDR) ----
		Fmp4Muxer::Color color;
		if (mHub.hdr())
			color = {9, 16, 9, false}; // BT.2020 primaries, PQ (SMPTE ST 2084), BT.2020 non-constant, limited range
		mMuxer = std::make_unique<Fmp4Muxer>(mHevc, mOpts.fps, color);
		mMedia = mPc->createDataChannel("media"); // reliable, ordered
		mMedia->onOpen([weak] {
			if (auto self = weak.lock()) {
				self->mMediaOpen = true;
				self->mNeedKeyframe = true;
				self->mHub.requestKeyframe("media channel open");
			}
		});
	}

	// ---- audio: Opus over RTP, with redundancy and retransmission ----
	uint32_t assrc = randomSsrc();
	rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
	audio.addAudioCodec(kPtRed, "red/48000/2", std::to_string(kPtOpus) + "/" + std::to_string(kPtOpus));
	audio.addOpusCodec(kPtOpus, "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1");
	for (int pt : {kPtRed, kPtOpus})
		if (auto *map = audio.rtpMap(pt))
			map->addFeedback("nack");
	audio.addSSRC(assrc, cname, "duoplay-audio-" + mId, "audio");
	mAudio = mPc->addTrack(audio);
	mAudioCfg = std::make_shared<rtc::RtpPacketizationConfig>(assrc, cname, kPtOpus, rtc::OpusRtpPacketizer::DefaultClockRate);
	auto apack = std::make_shared<rtc::OpusRtpPacketizer>(mAudioCfg);
	apack->addToChain(std::make_shared<rtc::RtcpSrReporter>(mAudioCfg));
	apack->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
	mAudio->setMediaHandler(apack);
	mAudio->onOpen([weak] {
		if (auto self = weak.lock())
			self->mAudioOpen = true;
	});

	// ---- data channels ----
	// Controller input: unordered and never retransmitted. Every packet carries the
	// complete pad state, so a lost one is healed by the next.
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
		if (state == rtc::PeerConnection::State::Connected && !self->mAttached.exchange(true))
			self->mHub.addPeer(self);
		if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
			std::thread([self] { self->close(); }).detach();
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
	auto remote = mPc->remoteDescription();
	if (!remote)
		return true;
	for (int i = 0; i < remote->mediaCount(); i++) {
		auto entry = remote->media(i);
		auto *media = std::get_if<rtc::Description::Media *>(&entry);
		if (!media)
			continue;
		auto pts = (*media)->payloadTypes();
		if ((*media)->type() == "video" && mVideoCfg) {
			for (int pt : pts) {
				if (pt == kPtH264High || pt == kPtH264Base) {
					mVideoCfg->payloadType = uint8_t(pt);
					LOGI("peer", "%s: phone accepted H.264 (%s)", mId.c_str(), pt == kPtH264High ? "high" : "baseline");
					break;
				}
			}
		} else if ((*media)->type() == "audio") {
			bool red = std::find(pts.begin(), pts.end(), kPtRed) != pts.end();
			mAudioRed = red;
			mAudioCfg->payloadType = uint8_t(red ? kPtRed : kPtOpus);
			LOGI("peer", "%s: audio %s", mId.c_str(), red ? "with redundancy (RED)" : "plain Opus");
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
	return since && now - since > 15000;
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
	if (mClosed)
		return;
	if (mHub.hevc() != mHevc) {
		if (!mCodecWarned.exchange(true))
			sendControl(R"({"t":"renegotiate","reason":"picture format changed"})");
		return;
	}
	// The page sends a heartbeat several times a second while it's alive. If it goes
	// quiet (app backgrounded or suspended), stop spending Wi-Fi airtime on it.
	int64_t now = nowMs();
	if (!mPaused && now - mLastHeardMs > 3000) {
		mPaused = true;
		LOGI("peer", "%s: phone went quiet, pausing its stream", mId.c_str());
	}
	if (mPaused)
		return;
	if (mMse) {
		sendVideoMse(*frame, keyframe);
		return;
	}
	if (!mVideoOpen)
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

// Media-channel message: [type][flags: bit0 = last chunk][message id, u32 LE][payload chunk].
bool Peer::sendMedia(uint8_t type, const std::vector<uint8_t> &payload) {
	size_t limit = 60000;
	try {
		limit = std::min<size_t>(limit, std::max<size_t>(mMedia->maxMessageSize(), 4096));
	} catch (...) {
	}
	size_t chunk = limit - 6;
	uint32_t id = ++mMediaMsgId;
	size_t off = 0;
	do {
		size_t n = std::min(chunk, payload.size() - off);
		rtc::binary msg(6 + n);
		msg[0] = std::byte(type);
		msg[1] = std::byte(off + n >= payload.size() ? 1 : 0);
		for (int i = 0; i < 4; i++)
			msg[2 + i] = std::byte((id >> (8 * i)) & 0xff);
		if (n)
			memcpy(msg.data() + 6, payload.data() + off, n);
		try {
			if (!mMedia->send(std::move(msg)))
				return false;
		} catch (...) {
			return false;
		}
		off += n;
	} while (off < payload.size());
	return true;
}

void Peer::sendVideoMse(const Bytes &frame, bool keyframe) {
	if (!mMediaOpen || !mMedia)
		return;
	std::lock_guard<std::mutex> lock(mMediaMutex);
	int64_t now = nowMs();
	const auto *data = reinterpret_cast<const uint8_t *>(frame.data());
	if (mNeedKeyframe && !keyframe) {
		if (now - mLastKeyRequestMs > 400) {
			mLastKeyRequestMs = now;
			mHub.requestKeyframe("media player waiting for keyframe");
		}
		return;
	}
	if (keyframe) {
		bool changed = mMuxer->configure(data, frame.size());
		if (mMuxer->ready() && (changed || !mInitSent)) {
			json header = {{"mime", mMuxer->mime()}, {"width", mMuxer->width()}, {"height", mMuxer->height()}, {"hdr", mHub.hdr()}};
			std::string h = header.dump();
			auto init = mMuxer->initSegment();
			std::vector<uint8_t> payload;
			payload.push_back(uint8_t(h.size() & 0xff));
			payload.push_back(uint8_t(h.size() >> 8));
			payload.insert(payload.end(), h.begin(), h.end());
			payload.insert(payload.end(), init.begin(), init.end());
			if (sendMedia(1, payload)) {
				mInitSent = true;
				LOGI("peer", "%s: media player stream %s (%dx%d)", mId.c_str(), mMuxer->mime().c_str(), mMuxer->width(), mMuxer->height());
			}
		}
	}
	if (!mInitSent)
		return;
	mNeedKeyframe = false;

	// The reliable channel never drops anything, so Wi-Fi trouble shows up as a growing
	// send backlog, which on screen is latency. Back the console off early; if the backlog
	// still gets out of hand, skip to the next keyframe rather than play catch-up.
	size_t backlog = 0;
	try {
		backlog = mMedia->bufferedAmount();
	} catch (...) {
	}
	if (backlog > 1500000) {
		mNeedKeyframe = true;
		mHub.requestKeyframe("media backlog");
		if (mOpts.adaptiveBitrate)
			mHub.peerLoss(mId, 40);
		return;
	}
	if (backlog > 300000 && now - mLastCongestionMs > 200 && mOpts.adaptiveBitrate) {
		mLastCongestionMs = now;
		mHub.peerLoss(mId, 15);
	}
	sendMedia(2, mMuxer->fragment(data, frame.size(), keyframe));
}

void Peer::sendAudio(const uint8_t *data, size_t size, uint32_t ts48k) {
	if (!mAudioOpen || mClosed || mPaused)
		return;
	try {
		if (mAudioRed) {
			// RFC 2198: the previous frame rides along, so one lost packet costs no sound.
			std::vector<std::byte> p;
			p.reserve(size + mPrevAudio.size() + 5);
			uint32_t offset = ts48k - mPrevAudioTs;
			if (!mPrevAudio.empty() && mPrevAudio.size() < 1024 && offset > 0 && offset < 16384) {
				p.push_back(std::byte(0x80 | kPtOpus));
				p.push_back(std::byte(offset >> 6));
				p.push_back(std::byte(((offset & 0x3f) << 2) | ((mPrevAudio.size() >> 8) & 0x3)));
				p.push_back(std::byte(mPrevAudio.size() & 0xff));
				p.push_back(std::byte(kPtOpus));
				for (uint8_t b : mPrevAudio)
					p.push_back(std::byte(b));
			} else {
				p.push_back(std::byte(kPtOpus));
			}
			for (size_t i = 0; i < size; i++)
				p.push_back(std::byte(data[i]));
			mPrevAudio.assign(data, data + size);
			mPrevAudioTs = ts48k;
			mAudio->sendFrame(p.data(), p.size(), rtc::FrameInfo(ts48k));
		} else {
			mAudio->sendFrame(reinterpret_cast<const std::byte *>(data), size, rtc::FrameInfo(ts48k));
		}
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
		sendControl(json{{"t", "pong"}, {"id", j.value("id", 0.0)}}.dump());
		json link = {{"t", "link"}};
		if (mLink) {
			auto s = mLink->stats();
			link["queue_ms"] = s.queueMs;
			link["loss_pct"] = s.lossPct;
			link["rtx"] = s.retransmits;
			link["twcc"] = s.feedbacks;
		} else if (mMedia) {
			size_t backlog = 0;
			try {
				backlog = mMedia->bufferedAmount();
			} catch (...) {
			}
			link["queue_ms"] = double(backlog) * 8.0 / std::max(1000, mOpts.expectedKbps);
			link["loss_pct"] = 0;
		}
		sendControl(link.dump());
	} else if (t == "keyframe") {
		if (mMse)
			mNeedKeyframe = true;
		mHub.requestKeyframe("phone saw a frozen picture");
	} else if (t == "settings") {
		mOpts.adaptiveBitrate = j.value("adaptive", mOpts.adaptiveBitrate);
		if (mLink)
			mLink->setCongestionFeedback(mOpts.adaptiveBitrate);
	}
}

} // namespace duo
