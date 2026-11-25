// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "console.hpp"
#include "media.hpp"
#include "peer.hpp"
#include "store.hpp"
#include "streamer.hpp"
#include "tls.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

namespace duo {

struct ApiContext {
	Store &store;
	ConsoleManager &consoles;
	SessionManager &sessions;
	MediaHub &hub;
	NetworkConfig net;
	std::string webDir;
	TlsFiles tls;
	bool tlsEnabled = false;
	int httpPort = 0;
	int httpsPort = 0;
};

class Api {
public:
	explicit Api(ApiContext ctx);
	~Api();
	void install(httplib::Server &server);
	void closeAllPeers();

private:
	void reap();

	ApiContext mCtx;
	std::mutex mPeersMutex;
	std::map<std::string, std::shared_ptr<Peer>> mPeers;
	std::atomic<bool> mRunning{true};
	int64_t mUnwatchedSinceMs = 0;
	std::thread mReaper;
};

} // namespace duo
