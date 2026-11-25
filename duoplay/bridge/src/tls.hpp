// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <string>
#include <vector>

namespace duo {

struct TlsFiles {
	std::string caCertPem; // path
	std::string caCertDer; // path, what the iPhone installs
	std::string certPem;   // path, server certificate (chain: leaf + CA)
	std::string keyPem;    // path
};

// Creates (once) a private local certificate authority and (whenever this machine's
// addresses change) a server certificate it signs for those addresses. Installing
// and trusting the CA on the iPhone gives the page a secure context, which Safari
// requires for game controllers, screen wake lock and motion sensors.
bool ensureTls(const std::string &dir, const std::vector<std::string> &ips, const std::string &hostname,
               TlsFiles &out, std::string &err);

} // namespace duo
