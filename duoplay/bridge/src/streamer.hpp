// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "console.hpp"
#include "media.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace duo {

struct StreamRequest {
	std::string consoleId;
	int height = 1080;      // 360 / 540 / 720 / 1080
	int fps = 60;           // 30 / 60
	int bitrateKbps = 0;    // 0 = default for the resolution
	bool hdr = false;       // HEVC Main10 / PQ, played by Safari's media player
};

class SourceBase;

// Owns the one console session the bridge runs at a time. Browsers come and go
// (folding the phone, a Wi-Fi blip, reopening the app) without touching it, so
// reattaching is a WebRTC handshake plus a keyframe, not a new console session.
class SessionManager {
public:
	SessionManager(Store &store, ConsoleManager &consoles, MediaHub &hub, std::string fakeVideoPath);
	~SessionManager();

	bool start(const StreamRequest &req, std::string &err);
	void stop();
	bool submitLoginPin(const std::string &pin, std::string &err);
	bool goHome(std::string &err);
	bool standby(std::string &err);
	nlohmann::json status();
	bool fakeAvailable() const { return !mFakeVideoPath.empty(); }

	// internal, called by sources
	void onConnected();
	void onLoginPinRequest(bool incorrect);
	void onQuit(const std::string &reason, bool error);

private:
	void worker(StreamRequest req, ConsoleRecord console);
	void setState(const std::string &state, const std::string &message = "");
	void reapLocked(std::unique_lock<std::mutex> &lock);

	Store &mStore;
	ConsoleManager &mConsoles;
	MediaHub &mHub;
	std::string mFakeVideoPath;

	std::mutex mMutex;
	std::unique_ptr<SourceBase> mSource;
	std::thread mWorker;
	uint64_t mGeneration = 0;
	std::string mState = "idle"; // idle, waking, connecting, pin, streaming, ended, error
	std::string mMessage;
	bool mPinIncorrect = false;
	StreamRequest mRequest;
	std::string mConsoleName;
	int64_t mStateSinceMs = 0;
};

// Shared base so the manager can hold either the real PS5 source or the test pattern.
class SourceBase : public StreamSource {
public:
	virtual bool begin(std::string &err) = 0;
	virtual void end() = 0; // blocking; must be safe to call more than once
	virtual bool setLoginPin(const std::string &pin) { return false; }
	virtual bool goHome() { return false; }
	virtual bool standby() { return false; }
	virtual nlohmann::json stats() { return nlohmann::json::object(); }
};

std::unique_ptr<SourceBase> makeChiakiSource(SessionManager &mgr, MediaHub &hub, const ConsoleRecord &console,
                                             const StreamRequest &req, const std::string &psnAccountB64);
// `path` may hold two files separated by '|': an H.264 stream and an HEVC (HDR) one.
std::unique_ptr<SourceBase> makeFakeSource(SessionManager &mgr, MediaHub &hub, const std::string &path, int fps, bool hdr);

} // namespace duo
