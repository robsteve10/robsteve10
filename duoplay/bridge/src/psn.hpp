// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>

namespace duo {

// Sony login page used by the official Remote Play app. After signing in it lands on
// a page whose address contains ?code=...; the user pastes that address back to us.
std::string psnLoginUrl();

// Exchanges that code for the numeric account id the console pairs against, and
// returns it in the base64 form chiaki uses.
bool psnAccountIdFromRedirect(const std::string &redirectUrl, std::string &accountB64, std::string &err);

// Accepts an account id typed in directly: base64 (as shown by other tools) or decimal.
bool normalizeAccountId(const std::string &input, std::string &accountB64, std::string &err);

} // namespace duo
