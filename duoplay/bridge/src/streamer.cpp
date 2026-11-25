// SPDX-License-Identifier: AGPL-3.0-only
#include "streamer.hpp"
#include "util.hpp"

#include <chiaki/session.h>

#include <cstring>

using json = nlohmann::json;

namespace duo {

// ---------------------------------------------------------------------------
// The real thing: a chiaki session with the console.
// ---------------------------------------------------------------------------
class ChiakiSource final : public SourceBase {
public:
	ChiakiSource(SessionManager &mgr, MediaHub &hub, ConsoleRecord console, StreamRequest req, std::string psn)
	    : mMgr(mgr), mHub(hub), mConsole(std::move(console)), mReq(req), mPsn(std::move(psn)) {}
	~ChiakiSource() override { end(); }

	bool begin(std::string &err) override {
		auto registKey = hexDecode(mConsole.registKey);
		auto rpKey = hexDecode(mConsole.rpKey);
		if (!registKey || registKey->size() != CHIAKI_SESSION_AUTH_SIZE || !rpKey || rpKey->size() != 0x10) {
			err = "Stored pairing data is invalid; pair the console again.";
			return false;
		}
		mHost = mConsole.host;

		ChiakiConnectInfo ci;
		memset(&ci, 0, sizeof(ci));
		ci.ps5 = mConsole.ps5;
		ci.host = mHost.c_str();
		memcpy(ci.regist_key, registKey->data(), sizeof(ci.regist_key));
		memcpy(ci.morning, rpKey->data(), sizeof(ci.morning));

		ChiakiVideoResolutionPreset preset = CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
		if (mReq.height <= 360)
			preset = CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
		else if (mReq.height <= 540)
			preset = CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
		else if (mReq.height <= 720)
			preset = CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
		if (!mConsole.ps5 && preset == CHIAKI_VIDEO_RESOLUTION_PRESET_1080p)
			preset = CHIAKI_VIDEO_RESOLUTION_PRESET_720p; // base PS4 tops out lower; let it downgrade
		chiaki_connect_video_profile_preset(&ci.video_profile, preset,
		                                    mReq.fps <= 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
		if (mReq.bitrateKbps > 0)
			ci.video_profile.bitrate = unsigned(mReq.bitrateKbps);
		// H.264 for the lowest-latency WebRTC path; HDR needs the PS5's 10-bit HEVC.
		ci.video_profile.codec = (mReq.hdr && mConsole.ps5) ? CHIAKI_CODEC_H265_HDR : CHIAKI_CODEC_H264;
		ci.video_profile_auto_downgrade = true;
		ci.enable_keyboard = false;
		// Present a DualShock-style pad: the console then sends plain rumble, which a
		// browser can play on a connected controller. DualSense mode would send haptic
		// audio instead, which nothing in a browser can render.
		ci.enable_dualsense = false;
		ci.audio_video_disabled = CHIAKI_NONE_DISABLED;
		ci.auto_regist = false;
		ci.holepunch_session = nullptr;
		ci.rudp_sock = nullptr;
		if (auto acc = base64Decode(mPsn); acc && acc->size() == CHIAKI_PSN_ACCOUNT_ID_SIZE)
			memcpy(ci.psn_account_id, acc->data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
		ci.packet_loss_max = 0.05;
		// A browser decoder can't conceal a broken reference the way chiaki's own
		// player can, so ask for a clean keyframe whenever FEC can't repair a frame.
		ci.enable_idr_on_fec_failure = true;

		ChiakiErrorCode e = chiaki_session_init(&mSession, &ci, chiakiLog());
		if (e != CHIAKI_ERR_SUCCESS) {
			err = std::string("Could not set up the session: ") + chiaki_error_string(e);
			return false;
		}
		mInited = true;

		chiaki_session_set_event_cb(&mSession, &ChiakiSource::eventCb, this);
		chiaki_session_set_video_sample_cb(&mSession, &ChiakiSource::videoCb, this);
		ChiakiAudioSink sink{};
		sink.user = this;
		sink.header_cb = &ChiakiSource::audioHeaderCb;
		sink.frame_cb = &ChiakiSource::audioFrameCb;
		chiaki_session_set_audio_sink(&mSession, &sink);
		ChiakiControllerState idle;
		chiaki_controller_state_set_idle(&idle);
		chiaki_session_set_controller_state(&mSession, &idle);

		mHub.setSource(this, ci.video_profile.codec != CHIAKI_CODEC_H264, ci.video_profile.codec == CHIAKI_CODEC_H265_HDR);
		e = chiaki_session_start(&mSession);
		if (e != CHIAKI_ERR_SUCCESS) {
			mHub.clearSource(this);
			err = std::string("Could not start the session: ") + chiaki_error_string(e);
			return false;
		}
		mStarted = true;
		LOGI("session", "Connecting to %s at %s: %ux%u@%u, %u kbps, %s", mConsole.nickname.c_str(), mHost.c_str(),
		     ci.video_profile.width, ci.video_profile.height, ci.video_profile.max_fps, ci.video_profile.bitrate,
		     chiaki_codec_name(ci.video_profile.codec));
		return true;
	}

	void end() override {
		std::lock_guard<std::mutex> lock(mEndMutex);
		mConnected = false;
		mHub.clearSource(this);
		if (mStarted) {
			chiaki_session_stop(&mSession);
			chiaki_session_join(&mSession);
			mStarted = false;
		}
		if (mInited) {
			chiaki_session_fini(&mSession);
			mInited = false;
		}
	}

	void requestKeyframe() override {
		if (mConnected)
			chiaki_session_request_idr(&mSession);
	}

	void setController(const ChiakiControllerState &state) override {
		if (!mInited)
			return;
		ChiakiControllerState s = state;
		chiaki_session_set_controller_state(&mSession, &s);
	}

	void reportDownstreamLoss(uint64_t lost) override {
		// Folded into the loss figures chiaki reports to the console every 200 ms, so the
		// console's encoder reacts to the Wi-Fi hop too, not just console -> bridge.
		if (mConnected)
			chiaki_packet_stats_push_generation(&mSession.stream_connection.packet_stats, 0, lost);
	}

	bool setLoginPin(const std::string &pin) override {
		if (!mInited)
			return false;
		return chiaki_session_set_login_pin(&mSession, reinterpret_cast<const uint8_t *>(pin.data()), pin.size()) ==
		       CHIAKI_ERR_SUCCESS;
	}
	bool goHome() override { return mConnected && chiaki_session_go_home(&mSession) == CHIAKI_ERR_SUCCESS; }
	bool standby() override { return mConnected && chiaki_session_goto_bed(&mSession) == CHIAKI_ERR_SUCCESS; }

	json stats() override {
		if (!mConnected)
			return json::object();
		return {{"console_link_mbps", mSession.stream_connection.measured_bitrate},
		        {"console_link_loss", mSession.stream_connection.congestion_control.packet_loss},
		        {"rtt_ms", double(mSession.rtt_us) / 1000.0},
		        {"width", mSession.connect_info.video_profile.width},
		        {"height", mSession.connect_info.video_profile.height}};
	}

private:
	static void eventCb(ChiakiEvent *event, void *user) {
		auto *self = static_cast<ChiakiSource *>(user);
		switch (event->type) {
		case CHIAKI_EVENT_CONNECTED:
			self->mConnected = true;
			self->mMgr.onConnected();
			self->mHub.requestKeyframe("session connected");
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			self->mMgr.onLoginPinRequest(event->login_pin_request.pin_incorrect);
			break;
		case CHIAKI_EVENT_QUIT: {
			self->mConnected = false;
			std::string reason = chiaki_quit_reason_string(event->quit.reason);
			if (event->quit.reason_str && *event->quit.reason_str)
				reason += std::string(": ") + event->quit.reason_str;
			self->mMgr.onQuit(reason, chiaki_quit_reason_is_error(event->quit.reason));
			break;
		}
		case CHIAKI_EVENT_RUMBLE: {
			json j = {{"t", "rumble"}, {"l", event->rumble.left}, {"r", event->rumble.right}};
			self->mHub.broadcastControl(j.dump());
			break;
		}
		case CHIAKI_EVENT_VIDEO_FEC_FAILURE:
			LOGD("session", "Console->bridge frame %d unrecoverable (idr requested: %d)",
			     event->video_fec_failure.frame_index, event->video_fec_failure.idr_request_sent);
			break;
		default:
			break;
		}
	}

	static bool videoCb(uint8_t *buf, size_t size, int32_t framesLost, bool recovered, void *user) {
		auto *self = static_cast<ChiakiSource *>(user);
		self->mHub.videoSample(buf, size);
		return true;
	}

	static void audioHeaderCb(ChiakiAudioHeader *h, void *user) {
		static_cast<ChiakiSource *>(user)->mHub.audioHeader(h->rate, h->frame_size, h->channels);
	}

	static void audioFrameCb(uint8_t *buf, size_t size, void *user) {
		static_cast<ChiakiSource *>(user)->mHub.audioFrame(buf, size);
	}

	SessionManager &mMgr;
	MediaHub &mHub;
	ConsoleRecord mConsole;
	StreamRequest mReq;
	std::string mPsn;
	std::string mHost;
	ChiakiSession mSession{};
	bool mInited = false;
	bool mStarted = false;
	std::atomic<bool> mConnected{false};
	std::mutex mEndMutex;
};

std::unique_ptr<SourceBase> makeChiakiSource(SessionManager &mgr, MediaHub &hub, const ConsoleRecord &console,
                                             const StreamRequest &req, const std::string &psnAccountB64) {
	return std::make_unique<ChiakiSource>(mgr, hub, console, req, psnAccountB64);
}

// ---------------------------------------------------------------------------
// SessionManager
// ---------------------------------------------------------------------------
SessionManager::SessionManager(Store &store, ConsoleManager &consoles, MediaHub &hub, std::string fakeVideoPath)
    : mStore(store), mConsoles(consoles), mHub(hub), mFakeVideoPath(std::move(fakeVideoPath)), mStateSinceMs(nowMs()) {}

SessionManager::~SessionManager() { stop(); }

void SessionManager::setState(const std::string &state, const std::string &message) {
	if (state != mState)
		LOGI("session", "State: %s%s%s", state.c_str(), message.empty() ? "" : " - ", message.c_str());
	mState = state;
	mMessage = message;
	mStateSinceMs = nowMs();
	mConsoles.setStreaming(state == "streaming" || state == "connecting");
	json j = {{"t", "session"}, {"state", mState}, {"message", mMessage}, {"pin_incorrect", mPinIncorrect}};
	mHub.broadcastControl(j.dump());
}

bool SessionManager::start(const StreamRequest &req, std::string &err) {
	ConsoleRecord console;
	if (req.consoleId == "test-pattern") {
		if (mFakeVideoPath.empty()) {
			err = "The test pattern is not enabled on this bridge (start it with --test-pattern).";
			return false;
		}
		console.id = "test-pattern";
		console.nickname = "Test pattern";
	} else {
		auto c = mStore.console(req.consoleId);
		if (!c) {
			err = "Unknown console; pair it first.";
			return false;
		}
		console = *c;
	}

	stop(); // one session at a time; a new request replaces the old one

	std::unique_lock<std::mutex> lock(mMutex);
	mGeneration++;
	mRequest = req;
	mConsoleName = console.nickname;
	mPinIncorrect = false;
	setState(console.id == "test-pattern" ? "connecting" : "waking");
	mWorker = std::thread([this, req, console] { worker(req, console); });
	return true;
}

void SessionManager::worker(StreamRequest req, ConsoleRecord console) {
	uint64_t gen;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		gen = mGeneration;
	}
	auto cancelled = [&] {
		std::lock_guard<std::mutex> lock(mMutex);
		return gen != mGeneration;
	};

	std::unique_ptr<SourceBase> source;
	if (console.id == "test-pattern") {
		source = makeFakeSource(*this, mHub, mFakeVideoPath, req.fps, req.hdr);
	} else {
		// Wake the console if it is asleep and wait for it to report ready.
		auto findState = [&]() -> std::string {
			for (const auto &h : mConsoles.discovered())
				if (h.id == console.id)
					return h.state;
			return "unknown";
		};
		std::string st = findState();
		if (st != "ready") {
			std::string werr;
			mConsoles.wake(console, werr);
			int64_t deadline = nowMs() + (st == "standby" ? 45000 : 12000);
			int64_t lastWake = nowMs();
			while (nowMs() < deadline) {
				if (cancelled())
					return;
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
				st = findState();
				if (st == "ready")
					break;
				if (nowMs() - lastWake > 8000) { // wake packets are UDP; repeat now and then
					mConsoles.wake(console, werr);
					lastWake = nowMs();
				}
			}
			if (st == "ready") // give the Remote Play service a moment after boot
				std::this_thread::sleep_for(std::chrono::milliseconds(1500));
			// If discovery never saw it (broadcast blocked), try connecting anyway.
		}
		if (auto fresh = mStore.console(console.id))
			console = *fresh; // discovery may have updated its address
		source = makeChiakiSource(*this, mHub, console, req, mStore.psnAccountId());
	}

	std::unique_lock<std::mutex> lock(mMutex);
	if (gen != mGeneration)
		return;
	if (console.id != "test-pattern")
		setState("connecting");
	std::string err;
	lock.unlock();
	bool ok = source->begin(err);
	lock.lock();
	if (gen != mGeneration) {
		lock.unlock();
		source->end();
		return;
	}
	if (!ok) {
		setState("error", err);
		lock.unlock();
		source->end();
		return;
	}
	mSource = std::move(source);
	if (console.id == "test-pattern")
		setState("streaming");
}

void SessionManager::stop() {
	std::unique_ptr<SourceBase> source;
	std::thread worker;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		mGeneration++;
		source = std::move(mSource);
		worker = std::move(mWorker);
		if (mState != "idle")
			setState("idle");
	}
	if (worker.joinable()) {
		if (worker.get_id() == std::this_thread::get_id())
			worker.detach();
		else
			worker.join();
	}
	if (source)
		source->end();
}

void SessionManager::onConnected() {
	std::lock_guard<std::mutex> lock(mMutex);
	mPinIncorrect = false;
	setState("streaming");
}

void SessionManager::onLoginPinRequest(bool incorrect) {
	std::lock_guard<std::mutex> lock(mMutex);
	mPinIncorrect = incorrect;
	setState("pin", incorrect ? "That passcode was wrong. Try again." : "Enter the console's login passcode.");
}

void SessionManager::onQuit(const std::string &reason, bool error) {
	// Called on chiaki's session thread, which end() joins; hand teardown to another thread.
	std::unique_ptr<SourceBase> source;
	{
		std::lock_guard<std::mutex> lock(mMutex);
		source = std::move(mSource);
		setState(error ? "error" : "ended", reason);
	}
	if (source) {
		std::thread([s = std::move(source)]() mutable { s->end(); }).detach();
	}
}

bool SessionManager::submitLoginPin(const std::string &pin, std::string &err) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (!mSource || mState != "pin") {
		err = "The console is not asking for a passcode right now.";
		return false;
	}
	if (!mSource->setLoginPin(pin)) {
		err = "Could not send the passcode.";
		return false;
	}
	setState("connecting");
	return true;
}

bool SessionManager::goHome(std::string &err) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (!mSource || !mSource->goHome()) {
		err = "Not streaming.";
		return false;
	}
	return true;
}

bool SessionManager::standby(std::string &err) {
	std::lock_guard<std::mutex> lock(mMutex);
	if (!mSource || !mSource->standby()) {
		err = "Not streaming.";
		return false;
	}
	return true;
}

json SessionManager::status() {
	std::lock_guard<std::mutex> lock(mMutex);
	json j = {{"state", mState},
	          {"message", mMessage},
	          {"pin_incorrect", mPinIncorrect},
	          {"console_id", mRequest.consoleId},
	          {"console_name", mConsoleName},
	          {"since_ms", nowMs() - mStateSinceMs},
	          {"request", {{"height", mRequest.height}, {"fps", mRequest.fps}, {"bitrate_kbps", mRequest.bitrateKbps}, {"hdr", mRequest.hdr}}}};
	if (mSource)
		j["source"] = mSource->stats();
	auto s = mHub.stats();
	j["media"] = {{"video_mbps", s.videoMbps},
	              {"fps", s.fps},
	              {"keyframes", s.keyframes},
	              {"keyframe_requests", s.keyframeRequests},
	              {"wifi_loss_reported", s.downstreamLostReported},
	              {"viewers", mHub.peerCount()}};
	return j;
}

} // namespace duo
