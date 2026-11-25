// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace duo {

// A console this bridge has been paired ("registered") with.
struct ConsoleRecord {
	std::string id;        // stable id: server MAC as hex
	std::string nickname;  // e.g. "PS5-123"
	std::string host;      // last known IP address
	bool ps5 = true;
	int target = 0;        // ChiakiTarget
	std::string registKey; // hex of the 16 raw bytes of rp_regist_key
	std::string rpKey;     // hex of the 16 byte rp_key ("morning")
	uint32_t rpKeyType = 0;
};

// Small JSON file with everything that must survive a restart. Thread-safe.
class Store {
public:
	explicit Store(std::string dir);

	const std::string &dir() const { return mDir; }

	std::string psnAccountId() const; // base64 of 8 bytes, or empty
	void setPsnAccountId(const std::string &b64);

	std::vector<ConsoleRecord> consoles() const;
	std::optional<ConsoleRecord> console(const std::string &id) const;
	void upsertConsole(const ConsoleRecord &rec);
	bool removeConsole(const std::string &id);
	void updateHost(const std::string &id, const std::string &host);

private:
	void load();
	void saveLocked();

	std::string mDir;
	std::string mPath;
	mutable std::mutex mMutex;
	std::string mPsnAccountId;
	std::vector<ConsoleRecord> mConsoles;
};

} // namespace duo
