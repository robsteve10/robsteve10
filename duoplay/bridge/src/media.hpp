// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chiaki/controller.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duo {

using Bytes = std::vector<std::byte>;

// Where audio/video comes from: the PS5 (through chiaki) or the built-in test pattern.
class StreamSource {
public:
	virtual ~StreamSource() = default;
	virtual void requestKeyframe() = 0;
	virtual void setController(const ChiakiControllerState &state) = 0;
	// Packets the phone lost over Wi-Fi, so the console's encoder can back off.
	virtual void reportDownstreamLoss(uint64_t lost) = 0;
};

// One connected browser.
class PeerSink {
public:
	virtual ~PeerSink() = default;
	virtual const std::string &id() const = 0;
	virtual void sendVideo(const std::shared_ptr<const Bytes> &frame, uint32_t ts90k, bool keyframe) = 0;
	virtual void sendAudio(const uint8_t *data, size_t size, uint32_t ts48k) = 0;
	virtual void sendControl(const std::string &json) = 0;
	virtual bool wantsAdaptiveBitrate() const = 0;
};

struct NalInfo {
	bool keyframe = false;   // IDR / IRAP picture
	bool hasParams = false;  // SPS (H.264) or VPS/SPS (H.265) present
	bool hasPicture = false; // any VCL NAL present
};
NalInfo scanAnnexB(const uint8_t *data, size_t size, bool hevc);

// Fans the console's stream out to every connected browser without re-encoding,
// and funnels their controller input and network feedback back to the source.
class MediaHub {
public:
	MediaHub();
	~MediaHub();

	// --- source side (any thread) ---
	void setSource(StreamSource *source, bool hevc, bool hdr = false);
	void clearSource(StreamSource *source);
	void videoSample(const uint8_t *data, size_t size);
	void audioHeader(uint32_t sampleRate, uint32_t frameSize, uint32_t channels);
	void audioFrame(const uint8_t *data, size_t size);
	void broadcastControl(const std::string &json);
	bool hevc() const { return mHevc; }
	bool hdr() const { return mHdr; }

	// --- peer side ---
	void addPeer(const std::shared_ptr<PeerSink> &peer);
	void removePeer(const std::string &id);
	size_t peerCount();
	void requestKeyframe(const char *why);
	void peerInput(const std::string &peerId, const ChiakiControllerState &state);
	void peerLoss(const std::string &peerId, uint64_t lost);

	struct Stats {
		double videoMbps = 0;
		double fps = 0;
		uint64_t keyframes = 0;
		uint64_t keyframeRequests = 0;
		uint64_t downstreamLostReported = 0;
		int width = 0, height = 0;
	};
	Stats stats();

private:
	void maintenance();
	void pushInputLocked();

	std::mutex mSourceMutex;
	StreamSource *mSource = nullptr;
	std::atomic<bool> mHevc{false};
	std::atomic<bool> mHdr{false};

	std::mutex mPeersMutex;
	std::vector<std::shared_ptr<PeerSink>> mPeers;

	// video state (touched only from the source's video thread, plus stats)
	Bytes mParamSets;
	int64_t mVideoStartUs = 0;
	uint32_t mVideoTsBase = 0;
	std::atomic<int64_t> mLastKeyframeRequestMs{0};

	// audio state
	std::atomic<uint32_t> mAudioFrameSize{480};
	uint32_t mAudioTs = 0;

	// input
	struct PeerInput {
		ChiakiControllerState state;
		int64_t lastMs = 0;
		bool neutral = true;
	};
	std::mutex mInputMutex;
	std::map<std::string, PeerInput> mInputs;

	// stats
	std::mutex mStatsMutex;
	uint64_t mBytesWindow = 0, mFramesWindow = 0;
	int64_t mWindowStartMs = 0;
	Stats mStats;

	std::atomic<bool> mRunning{true};
	std::mutex mWakeMutex;
	std::condition_variable mWakeCv;
	std::thread mThread;
};

} // namespace duo
