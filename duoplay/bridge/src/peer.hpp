// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "media.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace duo {

struct NetworkConfig {
	uint16_t udpPort = 9303 + 200;        // single UDP port for all WebRTC traffic (ICE UDP mux)
	std::optional<std::string> bindAddress;
	std::vector<std::string> announceIps; // extra host candidates, e.g. the Docker host's LAN IP
};

struct PeerOptions {
	std::string clientId;
	bool adaptiveBitrate = true; // feed Wi-Fi loss back into the console's encoder
	bool smoothBursts = true;    // pace keyframe bursts so the access point's queue doesn't overflow
	int latencyMode = 1;         // 0 = lowest (no jitter buffer), 1 = balanced, 2 = smooth
	int expectedKbps = 15000;
};

class Peer final : public PeerSink, public std::enable_shared_from_this<Peer> {
public:
	using ClosedCallback = std::function<void(const std::string &id)>;

	static std::shared_ptr<Peer> create(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts,
	                                    ClosedCallback onClosed);
	~Peer() override;

	// Server-offer signalling: we offer (so we control codecs and header extensions),
	// the phone answers. Both block only for local ICE gathering, which on a LAN is instant.
	bool createOffer(std::string &sdp, std::string &err);
	bool applyAnswer(const std::string &sdp, std::string &err);
	void close();

	const std::string &id() const override { return mId; }
	const std::string &clientId() const { return mOpts.clientId; }
	void sendVideo(const std::shared_ptr<const Bytes> &frame, uint32_t ts90k, bool keyframe) override;
	void sendAudio(const uint8_t *data, size_t size, uint32_t ts48k) override;
	void sendControl(const std::string &json) override;
	bool wantsAdaptiveBitrate() const override { return mOpts.adaptiveBitrate; }

	bool expired(int64_t nowMs) const;
	std::string describe() const;

private:
	Peer(MediaHub &hub, const NetworkConfig &net, const PeerOptions &opts, ClosedCallback onClosed);
	void setup();
	void onInput(const rtc::binary &msg);
	void onControl(const std::string &msg);
	void heard();
	void onRtcpLoss(uint64_t lost, uint64_t nacked);

	MediaHub &mHub;
	NetworkConfig mNet;
	PeerOptions mOpts;
	ClosedCallback mOnClosed;
	std::string mId;
	bool mHevc = false;

	std::shared_ptr<rtc::PeerConnection> mPc;
	std::shared_ptr<rtc::Track> mVideo, mAudio;
	std::shared_ptr<rtc::DataChannel> mInput, mControl;
	std::shared_ptr<rtc::RtpPacketizationConfig> mVideoCfg, mAudioCfg;

	std::atomic<bool> mVideoOpen{false}, mAudioOpen{false}, mControlOpen{false};
	std::atomic<bool> mNeedKeyframe{true};
	std::atomic<int64_t> mLastKeyRequestMs{0};
	std::atomic<bool> mClosed{false};
	std::atomic<bool> mAttached{false};
	std::atomic<int64_t> mCreatedMs{0};
	std::atomic<int64_t> mLastHeardMs{0};
	std::atomic<bool> mPaused{false};
	std::atomic<bool> mCodecWarned{false};
	std::atomic<int> mPcState{0};
	std::atomic<int64_t> mDisconnectedSinceMs{0};

	uint16_t mLastInputSeq = 0;
	bool mHaveInputSeq = false;
	std::mutex mInputMutex;
};

} // namespace duo
