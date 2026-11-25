// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstddef>

namespace duo {

// The web app compiled into the binary (see scripts/embed-web.py), so a bridge on a
// Raspberry Pi is one file to download.
struct EmbeddedFile {
	const char *path; // relative, e.g. "js/main.js"
	const char *mime;
	const unsigned char *data;
	size_t size;
};

extern const EmbeddedFile kEmbeddedWeb[];
extern const size_t kEmbeddedWebCount;

} // namespace duo
