// SPDX-License-Identifier: AGPL-3.0-only
#include "psn.hpp"
#include "util.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace duo {

// The public client of the Remote Play desktop app (as used by chiaki-ng).
static const char *kClientId = "ba495a24-818c-472b-b12d-ff231c1b5745";
static const char *kClientSecret = "mvaiZkRsAsI1IBkY";
static const char *kAuthHost = "auth.api.sonyentertainmentnetwork.com";
static const char *kRedirect = "https://remoteplay.dl.playstation.net/remoteplay/redirect";

std::string psnLoginUrl() {
	return std::string("https://") + kAuthHost +
	       "/2.0/oauth/authorize?service_entity=urn:service-entity:psn&response_type=code&client_id=" + kClientId +
	       "&redirect_uri=" + kRedirect +
	       "&scope=psn:clientapp&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay"
	       "&prompt=always&PlatformPrivacyWs1=minimal&";
}

static std::string urlDecode(const std::string &s) {
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
			out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
			i += 2;
		} else if (s[i] == '+') {
			out += ' ';
		} else {
			out += s[i];
		}
	}
	return out;
}

static std::string accountToB64(uint64_t id) {
	uint8_t b[8];
	for (int i = 0; i < 8; i++)
		b[i] = uint8_t(id >> (8 * i)); // little endian, as the console expects
	return base64Encode(b, 8);
}

bool normalizeAccountId(const std::string &inRaw, std::string &out, std::string &err) {
	std::string in;
	for (char c : inRaw)
		if (!isspace((unsigned char)c))
			in += c;
	if (in.empty()) {
		err = "Empty account id";
		return false;
	}
	bool digits = std::all_of(in.begin(), in.end(), [](char c) { return c >= '0' && c <= '9'; });
	if (digits && in.size() >= 10) { // the long decimal form
		out = accountToB64(std::stoull(in));
		return true;
	}
	auto bytes = base64Decode(in);
	if (!bytes || bytes->size() != 8) {
		err = "That doesn't look like a PSN account id (expected 12 base64 characters ending in '=').";
		return false;
	}
	out = base64Encode(bytes->data(), bytes->size());
	return true;
}

bool psnAccountIdFromRedirect(const std::string &redirectUrl, std::string &out, std::string &err) {
	std::string code;
	size_t pos = redirectUrl.find("code=");
	if (pos != std::string::npos) {
		size_t end = redirectUrl.find_first_of("&#", pos);
		code = urlDecode(redirectUrl.substr(pos + 5, end == std::string::npos ? std::string::npos : end - pos - 5));
	}
	if (code.empty()) {
		err = "That address has no code in it. After signing in, copy the full address of the page that says "
		      "\"redirect\".";
		return false;
	}

	httplib::SSLClient cli(kAuthHost, 443);
	cli.enable_server_certificate_verification(true);
	cli.set_connection_timeout(10);
	cli.set_read_timeout(15);
	cli.set_basic_auth(kClientId, kClientSecret);
	std::string body = "grant_type=authorization_code&code=" + code + "&redirect_uri=" + kRedirect + "&";
	auto res = cli.Post("/2.0/oauth/token", body, "application/x-www-form-urlencoded");
	if (!res) {
		err = "Could not reach Sony's sign-in service from the bridge (" + httplib::to_string(res.error()) + ").";
		return false;
	}
	if (res->status != 200) {
		err = "Sony rejected the sign-in code (HTTP " + std::to_string(res->status) +
		      "). Codes expire quickly and work once; sign in again and paste the new address.";
		return false;
	}
	std::string token;
	try {
		token = json::parse(res->body).value("access_token", "");
	} catch (...) {
	}
	if (token.empty()) {
		err = "Sony's reply had no access token.";
		return false;
	}
	auto info = cli.Get(("/2.0/oauth/token/" + token).c_str());
	if (!info || info->status != 200) {
		err = "Could not read the account details from Sony.";
		return false;
	}
	try {
		auto j = json::parse(info->body);
		std::string userId = j.at("user_id").is_string() ? j.at("user_id").get<std::string>()
		                                                  : std::to_string(j.at("user_id").get<uint64_t>());
		out = accountToB64(std::stoull(userId));
	} catch (...) {
		err = "Sony's reply had no account id.";
		return false;
	}
	LOGI("psn", "Got PSN account id");
	return true;
}

} // namespace duo
