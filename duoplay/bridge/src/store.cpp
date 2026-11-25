// SPDX-License-Identifier: AGPL-3.0-only
#include "store.hpp"
#include "util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

using json = nlohmann::json;

namespace duo {

Store::Store(std::string dir) : mDir(std::move(dir)) {
	makeDirs(mDir);
	mPath = joinPath(mDir, "state.json");
	load();
}

void Store::load() {
	std::string text;
	if (!readFile(mPath, text))
		return;
	try {
		json j = json::parse(text);
		mPsnAccountId = j.value("psn_account_id", "");
		for (const auto &c : j.value("consoles", json::array())) {
			ConsoleRecord r;
			r.id = c.value("id", "");
			r.nickname = c.value("nickname", "");
			r.host = c.value("host", "");
			r.ps5 = c.value("ps5", true);
			r.target = c.value("target", 0);
			r.registKey = c.value("regist_key", "");
			r.rpKey = c.value("rp_key", "");
			r.rpKeyType = c.value("rp_key_type", 0u);
			if (!r.id.empty())
				mConsoles.push_back(r);
		}
		LOGI("store", "Loaded %zu paired console(s) from %s", mConsoles.size(), mPath.c_str());
	} catch (const std::exception &e) {
		LOGE("store", "Could not parse %s: %s", mPath.c_str(), e.what());
	}
}

void Store::saveLocked() {
	json j;
	j["psn_account_id"] = mPsnAccountId;
	j["consoles"] = json::array();
	for (const auto &r : mConsoles) {
		j["consoles"].push_back({{"id", r.id},
		                         {"nickname", r.nickname},
		                         {"host", r.host},
		                         {"ps5", r.ps5},
		                         {"target", r.target},
		                         {"regist_key", r.registKey},
		                         {"rp_key", r.rpKey},
		                         {"rp_key_type", r.rpKeyType}});
	}
	if (!writeFileAtomic(mPath, j.dump(2)))
		LOGE("store", "Could not write %s", mPath.c_str());
}

std::string Store::psnAccountId() const {
	std::lock_guard<std::mutex> lock(mMutex);
	return mPsnAccountId;
}

void Store::setPsnAccountId(const std::string &b64) {
	std::lock_guard<std::mutex> lock(mMutex);
	mPsnAccountId = b64;
	saveLocked();
}

std::vector<ConsoleRecord> Store::consoles() const {
	std::lock_guard<std::mutex> lock(mMutex);
	return mConsoles;
}

std::optional<ConsoleRecord> Store::console(const std::string &id) const {
	std::lock_guard<std::mutex> lock(mMutex);
	for (const auto &c : mConsoles)
		if (c.id == id)
			return c;
	return std::nullopt;
}

void Store::upsertConsole(const ConsoleRecord &rec) {
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = std::find_if(mConsoles.begin(), mConsoles.end(), [&](const ConsoleRecord &c) { return c.id == rec.id; });
	if (it == mConsoles.end())
		mConsoles.push_back(rec);
	else
		*it = rec;
	saveLocked();
}

bool Store::removeConsole(const std::string &id) {
	std::lock_guard<std::mutex> lock(mMutex);
	auto it = std::remove_if(mConsoles.begin(), mConsoles.end(), [&](const ConsoleRecord &c) { return c.id == id; });
	if (it == mConsoles.end())
		return false;
	mConsoles.erase(it, mConsoles.end());
	saveLocked();
	return true;
}

void Store::updateHost(const std::string &id, const std::string &host) {
	std::lock_guard<std::mutex> lock(mMutex);
	for (auto &c : mConsoles) {
		if (c.id == id && c.host != host) {
			c.host = host;
			saveLocked();
		}
	}
}

} // namespace duo
