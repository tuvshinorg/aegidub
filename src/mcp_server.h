// Copyright (c) 2026, aegidub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file mcp_server.h
/// @brief A Model Context Protocol server, so AI agents such as Claude Code
/// and Codex can read and edit the open subtitles
///
/// Serves the Streamable HTTP transport on 127.0.0.1 only, while enabled in
/// the options.

#pragma once

#include <string>

namespace agi { struct Context; }

namespace mcp_server {
	/// Start serving if enabled in the options, or stop if not; call again
	/// after changing them
	void Update(agi::Context *c);

	/// Stop serving and finish any running jobs. Call before the context goes away.
	void Stop();

	bool Running();

	/// The address clients connect to
	std::string Url();

	/// Why the server could not start, or empty
	std::string LastError();
}
