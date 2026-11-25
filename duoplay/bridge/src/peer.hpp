// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "fmp4.hpp"
#include "media.hpp"
#include "wifilink.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace duo {

struct NetworkConfig {
	uint16_t udpPort = 9600;              // single UDP port for all WebRTC traffic (ICE UDP mux)
	std::optional<std::string> bindAddress;
	std::vector<std::string> announceIps; // extra host candidates, e.g. a Docker host's LAN IP
};

struct PeerOptions {
	std::string clientId;
	bool adaptiveBitrate = true; // feed Wi-Fi congestion back into the console's encoder
	bool smoothBursts = true;    // pace keyframe bursts so the access point's queue doesn't overflow
	int latencyMode = 1;         // 0 = no buffering, 1 = balanced, 2 = smooth
	int expectedKbps = 15000;
	int fps = 60;
	bool mediaPlayer = false;    // send H.264 through Safari's media player too (fallback / testing)
};

// One phone. Picture travels either as RTP (H.264, lowest latency) or, for HEVC/HDR, as
// MP4 fragments on a reliable data channel for Safari's media player ("mse").
class Peer final : public PeerSink, public std::enable_shared_from_this<Peer> {
public:
	using ClosedCallback = std::function<void(const std::string &id)>;

	static std::shared_ptr<Peer> create(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts,
	                                    ClosedCallback onClosed);
	~Peer() override;

	bool createOffer(std::string &sdp, std::string &err);
	bool applyAnswer(const std::string &sdp, std::string &err);
	void close();

	const std::string &id() const override { return mId; }
	const std::string &clientId() const { return mOpts.clientId; }
	const char *transport() const { return mMse ? "mse" : "rtp"; }
	void sendVideo(const std::shared_ptr<const Bytes> &frame, uint32_t ts90k, bool keyframe) override;
	void sendAudio(const uint8_t *data, size_t size, uint32_t ts48k) override;
	void sendControl(const std::string &json) override;
	bool wantsAdaptiveBitrate() const override { return mOpts.adaptiveBitrate; }

	bool expired(int64_t nowMs) const;

private:
	Peer(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts, ClosedCallback onClosed);
	void setup();
	void onInput(const rtc::binary &msg);
	void onControl(const std::string &msg);
	void heard();
	bool sendMedia(uint8_t type, const std::vector<uint8_t> &payload);
	void sendVideoMse(const Bytes &frame, bool keyframe);

	MediaHub &mHub;
	NetworkConfig mNet;
	PeerOptions mOpts;
	ClosedCallback mOnClosed;
	std::string mId;
	bool mHevc = false;
	bool mMse = false;

	std::shared_ptr<rtc::PeerConnection> mPc;
	std::shared_ptr<rtc::Track> mVideo, mAudio;
	std::shared_ptr<rtc::DataChannel> mInput, mControl, mMedia;
	std::shared_ptr<rtc::RtpPacketizationConfig> mVideoCfg, mAudioCfg;
	std::shared_ptr<WifiLink> mLink;

	std::atomic<bool> mVideoOpen{false}, mAudioOpen{false}, mControlOpen{false}, mMediaOpen{false};
	std::atomic<bool> mNeedKeyframe{true};
	std::atomic<int64_t> mLastKeyRequestMs{0};
	std::atomic<bool> mClosed{false};
	std::atomic<bool> mAttached{false};
	std::atomic<int64_t> mCreatedMs{0};
	std::atomic<int64_t> mLastHeardMs{0};
	std::atomic<bool> mPaused{false};
	std::atomic<bool> mCodecWarned{false};
	std::atomic<int64_t> mDisconnectedSinceMs{0};

	// audio redundancy (RFC 2198): each packet also carries the previous Opus frame
	std::atomic<bool> mAudioRed{false};
	std::vector<uint8_t> mPrevAudio;
	uint32_t mPrevAudioTs = 0;

	// HDR / media-player path
	std::mutex mMediaMutex;
	std::unique_ptr<Fmp4Muxer> mMuxer;
	bool mInitSent = false;
	uint32_t mMediaMsgId = 0;
	int64_t mLastCongestionMs = 0;

	uint16_t mLastInputSeq = 0;
	bool mHaveInputSeq = false;
	std::mutex mInputMutex;
};

} // namespace duo
