// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace duo {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };
void setLogLevel(LogLevel level);
LogLevel logLevel();
void logf(LogLevel level, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LOGD(tag, ...) ::duo::logf(::duo::LogLevel::Debug, tag, __VA_ARGS__)
#define LOGI(tag, ...) ::duo::logf(::duo::LogLevel::Info, tag, __VA_ARGS__)
#define LOGW(tag, ...) ::duo::logf(::duo::LogLevel::Warn, tag, __VA_ARGS__)
#define LOGE(tag, ...) ::duo::logf(::duo::LogLevel::Error, tag, __VA_ARGS__)

using Clock = std::chrono::steady_clock;
inline int64_t nowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
inline int64_t nowUs() {
	return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
}

std::string base64Encode(const uint8_t *data, size_t len);
std::optional<std::vector<uint8_t>> base64Decode(const std::string &in);
std::string hexEncode(const uint8_t *data, size_t len);
std::optional<std::vector<uint8_t>> hexDecode(const std::string &in);
std::string randomId(size_t bytes = 12);

bool readFile(const std::string &path, std::string &out);
bool writeFileAtomic(const std::string &path, const std::string &data);
bool makeDirs(const std::string &path);
std::string joinPath(const std::string &a, const std::string &b);
std::string defaultDataDir();
std::string executableDir();

struct LocalAddress {
	std::string iface;
	std::string ip;        // dotted IPv4
	std::string broadcast; // dotted IPv4 broadcast for the subnet, may be empty
};
// Non-loopback IPv4 addresses of this machine, best (likely LAN) first.
std::vector<LocalAddress> localIPv4Addresses();
std::string hostName();

} // namespace duo
