// SPDX-License-Identifier: AGPL-3.0-only
#include "api.hpp"
#include "psn.hpp"
#include "util.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

using json = nlohmann::json;

namespace duo {

namespace {

void reply(httplib::Response &res, const json &j, int status = 200) {
	res.status = status;
	res.set_header("Cache-Control", "no-store");
	res.set_content(j.dump(), "application/json");
}

void fail(httplib::Response &res, const std::string &msg, int status = 400) { reply(res, {{"error", msg}}, status); }

bool body(const httplib::Request &req, httplib::Response &res, json &out) {
	try {
		out = req.body.empty() ? json::object() : json::parse(req.body);
		if (!out.is_object())
			throw std::runtime_error("not an object");
		return true;
	} catch (...) {
		fail(res, "Request body must be a JSON object");
		return false;
	}
}

json consoleJson(const ConsoleRecord &c) {
	return {{"id", c.id}, {"nickname", c.nickname}, {"host", c.host}, {"ps5", c.ps5}, {"paired", true}};
}

} // namespace

Api::Api(ApiContext ctx) : mCtx(std::move(ctx)) {
	mReaper = std::thread([this] { reap(); });
}

Api::~Api() {
	mRunning = false;
	if (mReaper.joinable())
		mReaper.join();
	closeAllPeers();
}

void Api::closeAllPeers() {
	std::map<std::string, std::shared_ptr<Peer>> peers;
	{
		std::lock_guard<std::mutex> lock(mPeersMutex);
		peers.swap(mPeers);
	}
	for (auto &kv : peers)
		kv.second->close();
}

void Api::reap() {
	while (mRunning) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::vector<std::shared_ptr<Peer>> dead;
		{
			std::lock_guard<std::mutex> lock(mPeersMutex);
			int64_t now = nowMs();
			for (auto it = mPeers.begin(); it != mPeers.end();) {
				if (it->second->expired(now)) {
					dead.push_back(it->second);
					it = mPeers.erase(it);
				} else {
					++it;
				}
			}
		}
		for (auto &p : dead)
			p->close();

		// Nobody watching for a while (phone put away without disconnecting): let the
		// console go, so it can sleep normally. Reattaching within the grace period is free.
		int64_t now = nowMs();
		std::string state = mCtx.sessions.status().value("state", "idle");
		bool live = state != "idle" && state != "error" && state != "ended";
		if (!live || mCtx.hub.peerCount() > 0 || state == "waking" || state == "connecting") {
			mUnwatchedSinceMs = 0;
		} else if (!mUnwatchedSinceMs) {
			mUnwatchedSinceMs = now;
		} else if (now - mUnwatchedSinceMs > 5 * 60 * 1000) {
			LOGI("api", "No phone attached for 5 minutes; ending the console session");
			mUnwatchedSinceMs = 0;
			mCtx.sessions.stop();
		}
	}
}

void Api::install(httplib::Server &srv) {
	auto &C = mCtx;

	srv.set_default_headers({{"X-Content-Type-Options", "nosniff"}});

	// ---- state ----------------------------------------------------------------
	srv.Get("/api/state", [&](const httplib::Request &req, httplib::Response &res) {
		if (req.has_param("discover"))
			C.consoles.boost();
		json consoles = json::array();
		auto discovered = C.consoles.discovered();
		std::set<std::string> seen;
		for (const auto &c : C.store.consoles()) {
			json j = consoleJson(c);
			j["state"] = "unknown";
			for (const auto &d : discovered) {
				if (d.id == c.id) {
					j["state"] = d.state;
					j["running_app"] = d.runningApp;
					seen.insert(d.id);
				}
			}
			consoles.push_back(j);
		}
		for (const auto &d : discovered) {
			if (seen.count(d.id))
				continue;
			consoles.push_back({{"id", d.id},
			                    {"nickname", d.name},
			                    {"host", d.addr},
			                    {"ps5", d.ps5},
			                    {"paired", false},
			                    {"state", d.state},
			                    {"running_app", d.runningApp}});
		}
		if (C.sessions.fakeAvailable())
			consoles.push_back({{"id", "test-pattern"},
			                    {"nickname", "Test pattern (no console)"},
			                    {"host", "built-in"},
			                    {"ps5", true},
			                    {"paired", true},
			                    {"state", "ready"},
			                    {"test", true}});
		json ips = json::array();
		for (const auto &a : localIPv4Addresses())
			ips.push_back(a.ip);
		reply(res, {{"version", DUOPLAY_VERSION},
		            {"psn_account_set", !C.store.psnAccountId().empty()},
		            {"consoles", consoles},
		            {"session", C.sessions.status()},
		            {"secure", req.ssl != nullptr},
		            {"https_port", C.tlsEnabled ? C.httpsPort : 0},
		            {"http_port", C.httpPort},
		            {"addresses", ips}});
	});

	// ---- console setup ----------------------------------------------------------
	srv.Post("/api/discover", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::string host = b.value("host", "");
		if (!host.empty())
			C.consoles.addProbeHost(host);
		else
			C.consoles.boost();
		reply(res, {{"ok", true}});
	});

	srv.Get("/api/psn/login-url", [&](const httplib::Request &, httplib::Response &res) {
		reply(res, {{"url", psnLoginUrl()}});
	});

	srv.Post("/api/psn/account", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::string acc, err;
		bool ok;
		if (b.contains("redirect_url"))
			ok = psnAccountIdFromRedirect(b.value("redirect_url", ""), acc, err);
		else
			ok = normalizeAccountId(b.value("account_id", ""), acc, err);
		if (!ok)
			return fail(res, err);
		C.store.setPsnAccountId(acc);
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/consoles/register", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::string host = b.value("host", "");
		std::string pinStr = b.value("pin", "");
		pinStr.erase(std::remove_if(pinStr.begin(), pinStr.end(), [](char c) { return !isdigit((unsigned char)c); }),
		             pinStr.end());
		if (host.empty() || pinStr.size() != 8)
			return fail(res, "Need the console's address and the 8-digit pairing PIN.");
		ConsoleRecord rec;
		std::string err;
		if (!C.consoles.registerConsole(host, b.value("ps5", true), uint32_t(std::stoul(pinStr)), rec, err))
			return fail(res, err);
		reply(res, {{"ok", true}, {"console", consoleJson(rec)}});
	});

	srv.Post("/api/consoles/wake", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		auto c = C.store.console(b.value("id", ""));
		if (!c)
			return fail(res, "Unknown console", 404);
		std::string err;
		if (!C.consoles.wake(*c, err))
			return fail(res, err);
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/consoles/remove", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		if (!C.store.removeConsole(b.value("id", "")))
			return fail(res, "Unknown console", 404);
		reply(res, {{"ok", true}});
	});

	// ---- console session ----------------------------------------------------------
	srv.Post("/api/session/start", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		StreamRequest r;
		r.consoleId = b.value("console_id", "");
		r.height = b.value("height", 1080);
		r.fps = b.value("fps", 60);
		r.bitrateKbps = std::clamp(b.value("bitrate_kbps", 0), 0, 60000);
		r.hevc = b.value("hevc", false);
		// Reuse a running session for the same console and settings: a phone
		// reattaching after a fold/unfold or a Wi-Fi blip must not restart the stream.
		json st = C.sessions.status();
		std::string state = st.value("state", "idle");
		bool live = state == "streaming" || state == "connecting" || state == "waking" || state == "pin";
		if (live && st.value("console_id", "") == r.consoleId && !b.value("restart", false)) {
			const auto &q = st["request"];
			if (q.value("height", 0) == r.height && q.value("fps", 0) == r.fps &&
			    q.value("bitrate_kbps", -1) == r.bitrateKbps && q.value("hevc", false) == r.hevc)
				return reply(res, {{"ok", true}, {"reused", true}});
		}
		std::string err;
		if (!C.sessions.start(r, err))
			return fail(res, err);
		reply(res, {{"ok", true}, {"reused", false}});
	});

	srv.Post("/api/session/stop", [&](const httplib::Request &, httplib::Response &res) {
		closeAllPeers();
		C.sessions.stop();
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/session/pin", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::string err;
		if (!C.sessions.submitLoginPin(b.value("pin", ""), err))
			return fail(res, err);
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/session/home", [&](const httplib::Request &, httplib::Response &res) {
		std::string err;
		if (!C.sessions.goHome(err))
			return fail(res, err);
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/session/standby", [&](const httplib::Request &, httplib::Response &res) {
		std::string err;
		if (!C.sessions.standby(err))
			return fail(res, err);
		reply(res, {{"ok", true}});
	});

	// ---- WebRTC signalling ----------------------------------------------------------
	srv.Post("/api/peer", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		PeerOptions o;
		o.clientId = b.value("client_id", "");
		o.adaptiveBitrate = b.value("adaptive", true);
		o.smoothBursts = b.value("smooth_bursts", true);
		o.latencyMode = std::clamp(b.value("latency", 1), 0, 2);
		o.expectedKbps = std::clamp(b.value("expected_kbps", 15000), 1000, 60000);

		// A phone reconnecting (new page after a fold, network change...) replaces its
		// old connection at once, so no airtime is wasted streaming to a dead socket.
		if (!o.clientId.empty()) {
			std::vector<std::shared_ptr<Peer>> old;
			{
				std::lock_guard<std::mutex> lock(mPeersMutex);
				for (auto it = mPeers.begin(); it != mPeers.end();) {
					if (it->second->clientId() == o.clientId) {
						old.push_back(it->second);
						it = mPeers.erase(it);
					} else {
						++it;
					}
				}
			}
			for (auto &p : old)
				p->close();
		}

		auto peer = Peer::create(C.hub, C.net, o, [this](const std::string &id) {
			std::lock_guard<std::mutex> lock(mPeersMutex);
			mPeers.erase(id);
		});
		std::string sdp, err;
		if (!peer->createOffer(sdp, err)) {
			peer->close();
			return fail(res, err, 500);
		}
		{
			std::lock_guard<std::mutex> lock(mPeersMutex);
			mPeers[peer->id()] = peer;
		}
		reply(res, {{"id", peer->id()}, {"sdp", sdp}, {"hevc", C.hub.hevc()}});
	});

	srv.Post("/api/peer/answer", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::shared_ptr<Peer> peer;
		{
			std::lock_guard<std::mutex> lock(mPeersMutex);
			auto it = mPeers.find(b.value("id", ""));
			if (it != mPeers.end())
				peer = it->second;
		}
		if (!peer)
			return fail(res, "Unknown or expired connection; start over", 404);
		std::string err;
		if (!peer->applyAnswer(b.value("sdp", ""), err))
			return fail(res, err);
		reply(res, {{"ok", true}});
	});

	srv.Post("/api/peer/close", [&](const httplib::Request &req, httplib::Response &res) {
		json b;
		if (!body(req, res, b))
			return;
		std::shared_ptr<Peer> peer;
		{
			std::lock_guard<std::mutex> lock(mPeersMutex);
			auto it = mPeers.find(b.value("id", ""));
			if (it != mPeers.end()) {
				peer = it->second;
				mPeers.erase(it);
			}
		}
		if (peer)
			peer->close();
		reply(res, {{"ok", true}});
	});

	// ---- certificate for the phone ----------------------------------------------------
	srv.Get("/duoplay-ca.cer", [&](const httplib::Request &, httplib::Response &res) {
		std::string der;
		if (!C.tlsEnabled || !readFile(C.tls.caCertDer, der))
			return fail(res, "HTTPS is disabled on this bridge", 404);
		res.set_header("Content-Disposition", "attachment; filename=\"DuoPlay-CA.cer\"");
		res.set_content(der, "application/x-x509-ca-cert");
	});

	// ---- the web app ------------------------------------------------------------------
	if (!srv.set_mount_point("/", C.webDir))
		LOGE("http", "Web app folder not found: %s", C.webDir.c_str());
	srv.set_file_extension_and_mimetype_mapping("webmanifest", "application/manifest+json");
	srv.set_file_extension_and_mimetype_mapping("js", "text/javascript");
	srv.set_post_routing_handler([](const httplib::Request &req, httplib::Response &res) {
		// Always revalidate the app's files so an update shows up on the next launch.
		if (req.path.rfind("/api/", 0) != 0 && !res.has_header("Cache-Control"))
			res.set_header("Cache-Control", "no-cache");
	});
}

} // namespace duo
