// SPDX-License-Identifier: AGPL-3.0-only
#include "media.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstring>

namespace duo {

// Walks Annex-B start codes and classifies the NAL units in an access unit.
NalInfo scanAnnexB(const uint8_t *d, size_t n, bool hevc) {
	NalInfo info;
	size_t i = 0;
	while (i + 3 < n) {
		// find 00 00 01
		if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t h = i + 3;
		if (h >= n)
			break;
		if (hevc) {
			int t = (d[h] >> 1) & 0x3f;
			if (t <= 31)
				info.hasPicture = true;
			if (t >= 16 && t <= 21)
				info.keyframe = true;
			if (t == 32 || t == 33)
				info.hasParams = true;
		} else {
			int t = d[h] & 0x1f;
			if (t >= 1 && t <= 5)
				info.hasPicture = true;
			if (t == 5)
				info.keyframe = true;
			if (t == 7)
				info.hasParams = true;
		}
		i = h + 1;
	}
	return info;
}

// Copies just the parameter-set NAL units (with their start codes) out of an access unit.
static Bytes extractParamSets(const uint8_t *d, size_t n, bool hevc) {
	Bytes out;
	std::vector<size_t> starts; // offsets of start codes (00 00 01, or 00 00 00 01)
	for (size_t i = 0; i + 3 <= n; i++) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			starts.push_back(i > 0 && d[i - 1] == 0 ? i - 1 : i);
			i += 2;
		}
	}
	for (size_t k = 0; k < starts.size(); k++) {
		size_t begin = starts[k];
		size_t end = k + 1 < starts.size() ? starts[k + 1] : n;
		size_t h = begin + (d[begin + 2] == 1 ? 3 : 4);
		if (h >= end)
			continue;
		int t = hevc ? (d[h] >> 1) & 0x3f : d[h] & 0x1f;
		bool param = hevc ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
		if (param)
			out.insert(out.end(), reinterpret_cast<const std::byte *>(d + begin), reinterpret_cast<const std::byte *>(d + end));
	}
	return out;
}

MediaHub::MediaHub() { mThread = std::thread([this] { maintenance(); }); }

MediaHub::~MediaHub() {
	mRunning = false;
	mWakeCv.notify_all();
	if (mThread.joinable())
		mThread.join();
}

void MediaHub::setSource(StreamSource *source, bool hevc, bool hdr) {
	std::lock_guard<std::mutex> lock(mSourceMutex);
	mSource = source;
	mHevc = hevc;
	mHdr = hdr;
	mParamSets.clear();
	mVideoStartUs = 0;
	mAudioTs = 0;
	// Random-ish base so successive sessions never reuse timestamps on a live track.
	mVideoTsBase = uint32_t(nowUs() * 90 / 1000);
}

void MediaHub::clearSource(StreamSource *source) {
	std::lock_guard<std::mutex> lock(mSourceMutex);
	if (mSource == source)
		mSource = nullptr;
}

void MediaHub::videoSample(const uint8_t *data, size_t size) {
	if (!size)
		return;
	NalInfo info = scanAnnexB(data, size, mHevc);
	if (!info.hasPicture) {
		// chiaki hands over the stream header (SPS/PPS, VPS too for HEVC) on its own
		// when a stream (or adaptive profile) starts. Keep it: every keyframe sent to
		// a browser must carry it, or a phone that joined late can't decode anything.
		if (info.hasParams) {
			mParamSets = extractParamSets(data, size, mHevc);
			LOGD("media", "Cached %zu bytes of stream parameter sets", mParamSets.size());
		}
		return;
	}

	auto frame = std::make_shared<Bytes>();
	if (info.keyframe && !info.hasParams && !mParamSets.empty()) {
		frame->reserve(mParamSets.size() + size);
		frame->insert(frame->end(), mParamSets.begin(), mParamSets.end());
	} else if (info.keyframe && info.hasParams) {
		// Refresh the cache from the in-band parameter sets.
		Bytes params = extractParamSets(data, size, mHevc);
		if (!params.empty())
			mParamSets = std::move(params);
	}
	frame->insert(frame->end(), reinterpret_cast<const std::byte *>(data), reinterpret_cast<const std::byte *>(data) + size);

	// RTP timestamps follow arrival time at the bridge: browsers render as soon as a
	// frame is complete, and their jitter estimate then reflects real network jitter.
	int64_t now = nowUs();
	if (!mVideoStartUs)
		mVideoStartUs = now;
	uint32_t ts = mVideoTsBase + uint32_t((now - mVideoStartUs) * 90 / 1000);

	std::vector<std::shared_ptr<PeerSink>> peers;
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		peers = mPeers;
	}
	std::shared_ptr<const Bytes> shared = frame;
	for (auto &p : peers)
		p->sendVideo(shared, ts, info.keyframe);

	std::lock_guard<std::mutex> lock(mStatsMutex);
	mBytesWindow += frame->size();
	mFramesWindow++;
	if (info.keyframe)
		mStats.keyframes++;
}

void MediaHub::audioHeader(uint32_t sampleRate, uint32_t frameSize, uint32_t channels) {
	LOGI("media", "Audio: %u Hz, %u channel(s), %u samples per frame", sampleRate, channels, frameSize);
	if (frameSize)
		mAudioFrameSize = sampleRate == 48000 ? frameSize : frameSize * 48000 / std::max(1u, sampleRate);
}

void MediaHub::audioFrame(const uint8_t *data, size_t size) {
	uint32_t ts = mAudioTs;
	mAudioTs += mAudioFrameSize;
	std::vector<std::shared_ptr<PeerSink>> peers;
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		peers = mPeers;
	}
	for (auto &p : peers)
		p->sendAudio(data, size, ts);
}

void MediaHub::broadcastControl(const std::string &json) {
	std::vector<std::shared_ptr<PeerSink>> peers;
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		peers = mPeers;
	}
	for (auto &p : peers)
		p->sendControl(json);
}

void MediaHub::addPeer(const std::shared_ptr<PeerSink> &peer) {
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		mPeers.push_back(peer);
	}
	requestKeyframe("new viewer");
}

void MediaHub::removePeer(const std::string &id) {
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		mPeers.erase(std::remove_if(mPeers.begin(), mPeers.end(), [&](auto &p) { return p->id() == id; }), mPeers.end());
	}
	std::lock_guard<std::mutex> lock(mInputMutex);
	if (mInputs.erase(id))
		pushInputLocked(); // never leave a button held by a phone that went away
}

size_t MediaHub::peerCount() {
	std::lock_guard<std::mutex> lock(mPeersMutex);
	return mPeers.size();
}

void MediaHub::requestKeyframe(const char *why) {
	// The console takes a moment to answer; asking again before then only wastes bits.
	int64_t now = nowMs();
	int64_t last = mLastKeyframeRequestMs.load();
	if (now - last < 250)
		return;
	mLastKeyframeRequestMs = now;
	{
		std::lock_guard<std::mutex> lock(mStatsMutex);
		mStats.keyframeRequests++;
	}
	LOGD("media", "Requesting keyframe (%s)", why);
	std::lock_guard<std::mutex> lock(mSourceMutex);
	if (mSource)
		mSource->requestKeyframe();
}

static bool isNeutral(const ChiakiControllerState &s) {
	return s.buttons == 0 && s.l2_state == 0 && s.r2_state == 0 && s.left_x == 0 && s.left_y == 0 &&
	       s.right_x == 0 && s.right_y == 0 && s.touches[0].id < 0 && s.touches[1].id < 0;
}

void MediaHub::peerInput(const std::string &peerId, const ChiakiControllerState &state) {
	std::lock_guard<std::mutex> lock(mInputMutex);
	auto &in = mInputs[peerId];
	in.state = state;
	in.lastMs = nowMs();
	in.neutral = isNeutral(state);
	pushInputLocked();
}

void MediaHub::pushInputLocked() {
	ChiakiControllerState merged;
	chiaki_controller_state_set_idle(&merged);
	bool first = true;
	for (auto &kv : mInputs) {
		if (first) {
			merged = kv.second.state;
			first = false;
		} else {
			ChiakiControllerState out;
			chiaki_controller_state_or(&out, &merged, &kv.second.state);
			merged = out;
		}
	}
	std::lock_guard<std::mutex> lock(mSourceMutex);
	if (mSource)
		mSource->setController(merged);
}

void MediaHub::peerLoss(const std::string &peerId, uint64_t lost) {
	if (!lost)
		return;
	{
		std::lock_guard<std::mutex> lock(mStatsMutex);
		mStats.downstreamLostReported += lost;
	}
	std::lock_guard<std::mutex> lock(mSourceMutex);
	if (mSource)
		mSource->reportDownstreamLoss(lost);
}

MediaHub::Stats MediaHub::stats() {
	std::lock_guard<std::mutex> lock(mStatsMutex);
	return mStats;
}

void MediaHub::maintenance() {
	mWindowStartMs = nowMs();
	while (mRunning) {
		{
			std::unique_lock<std::mutex> lock(mWakeMutex);
			mWakeCv.wait_for(lock, std::chrono::milliseconds(250));
		}
		int64_t now = nowMs();

		// A phone whose input went silent while holding something (Wi-Fi drop, app
		// suspended mid-fold) must not leave the character running into a wall.
		{
			std::lock_guard<std::mutex> lock(mInputMutex);
			bool changed = false;
			for (auto &kv : mInputs) {
				if (!kv.second.neutral && now - kv.second.lastMs > 1200) {
					chiaki_controller_state_set_idle(&kv.second.state);
					kv.second.neutral = true;
					changed = true;
					LOGW("input", "Input from %s went silent; released all buttons", kv.first.c_str());
				}
			}
			if (changed)
				pushInputLocked();
		}

		std::lock_guard<std::mutex> lock(mStatsMutex);
		int64_t elapsed = now - mWindowStartMs;
		if (elapsed >= 1000) {
			mStats.videoMbps = double(mBytesWindow) * 8.0 / 1000.0 / double(elapsed);
			mStats.fps = double(mFramesWindow) * 1000.0 / double(elapsed);
			mBytesWindow = 0;
			mFramesWindow = 0;
			mWindowStartMs = now;
		}
	}
}

} // namespace duo
