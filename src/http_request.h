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

/// @file http_request.h
/// @brief HTTPS requests to the AI services, retried on transient failures

#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace http {
	/// The request could not be completed
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	/// The cancel callback asked to stop
	struct Cancelled : Error {
		Cancelled() : Error("Cancelled") { }
	};

	struct Response {
		long status = 0;
		std::string body;
	};

	/// Perform a request, retrying dropped connections, rate limiting and
	/// server errors a few times before giving up
	/// @param body      POST body, or nullptr for a GET
	/// @param timeout   Longest a single attempt may take, in seconds
	/// @param cancelled Polled during the request; return true to abort
	/// @return The final response, whatever its HTTP status
	/// @throws Cancelled if cancelled, Error if no response was received
	Response Request(std::string const& url,
		std::vector<std::string> const& headers,
		std::string const* body,
		long timeout,
		std::function<bool()> const& cancelled);

	/// Download a file, following redirects
	/// @param progress Called with the fraction downloaded so far, 0 to 1
	/// @throws Cancelled if cancelled, Error on failure; a partial file is removed
	void Download(std::string const& url,
		std::string const& path,
		std::function<void(double)> const& progress,
		std::function<bool()> const& cancelled);
}
