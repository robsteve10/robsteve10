// SPDX-License-Identifier: AGPL-3.0-only
//
// Test pattern source: plays a looping H.264 file through exactly the same path a
// console stream takes (media hub -> WebRTC -> phone), and records the controller
// input it receives. Lets you check the phone side - layouts, Wi-Fi behaviour,
// touch controls - without a console, and is what the automated tests use.

#include "streamer.hpp"
#include "util.hpp"

#include <atomic>
#include <thread>

using json = nlohmann::json;

namespace duo {

namespace {

struct AccessUnit {
	size_t begin, end;
	bool key;
};

std::vector<AccessUnit> splitAccessUnits(const std::string &d, bool hevc) {
	std::vector<size_t> starts;
	const auto *p = reinterpret_cast<const uint8_t *>(d.data());
	size_t n = d.size();
	for (size_t i = 0; i + 3 <= n; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			starts.push_back(i > 0 && p[i - 1] == 0 ? i - 1 : i);
			i += 2;
		}
	}
	std::vector<AccessUnit> aus;
	size_t auStart = 0;
	bool auHasPicture = false, auKey = false;
	for (size_t k = 0; k < starts.size(); k++) {
		size_t s = starts[k];
		size_t h = s + (p[s + 2] == 1 ? 3 : 4);
		if (h >= n)
			break;
		int t;
		bool vcl, firstSlice, boundary, key;
		if (hevc) {
			t = (p[h] >> 1) & 0x3f;
			vcl = t <= 31;
			firstSlice = vcl && h + 2 < n && (p[h + 2] & 0x80); // first_slice_segment_in_pic_flag
			boundary = auHasPicture && (t == 35 || t == 32 || t == 33 || t == 34 || t == 39 || firstSlice);
			key = t >= 16 && t <= 21;
		} else {
			t = p[h] & 0x1f;
			vcl = t >= 1 && t <= 5;
			firstSlice = vcl && h + 1 < n && (p[h + 1] & 0x80); // first_mb_in_slice == 0
			boundary = auHasPicture && (t == 9 || t == 7 || t == 8 || t == 6 || firstSlice);
			key = t == 5;
		}
		if (boundary) {
			aus.push_back({auStart, s, auKey});
			auStart = s;
			auHasPicture = false;
			auKey = false;
		}
		if (vcl)
			auHasPicture = true;
		if (key)
			auKey = true;
	}
	if (auHasPicture)
		aus.push_back({auStart, n, auKey});
	return aus;
}

} // namespace

class FakeSource final : public SourceBase {
public:
	FakeSource(SessionManager &mgr, MediaHub &hub, std::string path, int fps, bool hdr)
	    : mMgr(mgr), mHub(hub), mFps(fps > 0 ? fps : 60) {
		// "standard.h264|hdr.hevc": the second file stands in for the console's HDR stream.
		size_t bar = path.find('|');
		std::string sdr = path.substr(0, bar);
		std::string hdrPath = bar == std::string::npos ? "" : path.substr(bar + 1);
		mHevc = hdr && !hdrPath.empty();
		mPath = mHevc ? hdrPath : sdr;
		chiaki_controller_state_set_idle(&mLastInput);
	}
	~FakeSource() override { end(); }

	bool begin(std::string &err) override {
		if (!readFile(mPath, mData) || mData.empty()) {
			err = "Could not read test pattern file " + mPath;
			return false;
		}
		mAus = splitAccessUnits(mData, mHevc);
		if (mAus.empty() || !mAus.front().key) {
			err = "Test pattern must be an Annex-B stream starting with a keyframe";
			return false;
		}
		LOGI("test", "Test pattern: %zu %s frames from %s at %d fps", mAus.size(), mHevc ? "HEVC" : "H.264", mPath.c_str(), mFps);
		mHub.setSource(this, mHevc, mHevc);
		mHub.audioHeader(48000, 960, 2);
		mRunning = true;
		mVideo = std::thread([this] { videoLoop(); });
		mAudio = std::thread([this] { audioLoop(); });
		return true;
	}

	void end() override {
		mRunning = false;
		mHub.clearSource(this);
		if (mVideo.joinable())
			mVideo.join();
		if (mAudio.joinable())
			mAudio.join();
	}

	void requestKeyframe() override { mKeyRequested = true; }

	void setController(const ChiakiControllerState &state) override {
		std::lock_guard<std::mutex> lock(mInputMutex);
		mLastInput = state;
		mInputUpdates++;
	}

	void reportDownstreamLoss(uint64_t lost) override { mLost += lost; }

	bool setLoginPin(const std::string &) override { return true; }
	bool goHome() override { return true; }

	json stats() override {
		std::lock_guard<std::mutex> lock(mInputMutex);
		return {{"test_pattern", true},
		        {"width", 0},
		        {"height", 0},
		        {"input",
		         {{"updates", mInputUpdates},
		          {"buttons", mLastInput.buttons},
		          {"l2", mLastInput.l2_state},
		          {"r2", mLastInput.r2_state},
		          {"lx", mLastInput.left_x},
		          {"ly", mLastInput.left_y},
		          {"rx", mLastInput.right_x},
		          {"ry", mLastInput.right_y},
		          {"touch0", mLastInput.touches[0].id >= 0
		                         ? json{{"id", mLastInput.touches[0].id}, {"x", mLastInput.touches[0].x}, {"y", mLastInput.touches[0].y}}
		                         : json(nullptr)}}},
		        {"wifi_lost_reported", mLost.load()}};
	}

private:
	void videoLoop() {
		auto frameDur = std::chrono::microseconds(1000000 / mFps);
		auto next = Clock::now();
		size_t i = 0;
		while (mRunning) {
			if (mKeyRequested.exchange(false)) {
				// jump to the next keyframe, like the console answering an IDR request
				size_t j = i;
				do {
					j = (j + 1) % mAus.size();
				} while (!mAus[j].key && j != i);
				i = j;
			}
			const auto &au = mAus[i];
			mHub.videoSample(reinterpret_cast<const uint8_t *>(mData.data()) + au.begin, au.end - au.begin);
			i = (i + 1) % mAus.size();
			next += frameDur;
			std::this_thread::sleep_until(next);
			if (Clock::now() - next > std::chrono::milliseconds(200))
				next = Clock::now();
		}
	}

	void audioLoop() {
		// 20 ms Opus silence frames keep the audio track (and its RTCP) alive.
		static const uint8_t silence[] = {0xf8, 0xff, 0xfe};
		auto next = Clock::now();
		while (mRunning) {
			mHub.audioFrame(silence, sizeof(silence));
			next += std::chrono::milliseconds(20);
			std::this_thread::sleep_until(next);
			if (Clock::now() - next > std::chrono::milliseconds(200))
				next = Clock::now();
		}
	}

	SessionManager &mMgr;
	MediaHub &mHub;
	std::string mPath;
	int mFps;
	bool mHevc = false;
	std::string mData;
	std::vector<AccessUnit> mAus;
	std::atomic<bool> mRunning{false};
	std::atomic<bool> mKeyRequested{false};
	std::atomic<uint64_t> mLost{0};
	std::thread mVideo, mAudio;
	std::mutex mInputMutex;
	ChiakiControllerState mLastInput;
	uint64_t mInputUpdates = 0;
};

std::unique_ptr<SourceBase> makeFakeSource(SessionManager &mgr, MediaHub &hub, const std::string &path, int fps, bool hdr) {
	return std::make_unique<FakeSource>(mgr, hub, path, fps, hdr);
}

} // namespace duo
