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

/// @file external_process.h
/// @brief Run a command-line tool such as ffmpeg and follow its output

#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace external_process {
	struct Error : std::runtime_error {
		using std::runtime_error::runtime_error;
	};

	struct Result {
		int exit_code = -1;
		bool cancelled = false;
		/// The last lines of output, for error messages
		std::string tail;
	};

	/// Run a program without a console window and wait for it to finish
	/// @param args      Program followed by its arguments, UTF-8, unquoted
	/// @param on_line   Called with each line of combined stdout and stderr;
	///                  carriage returns also end a line, for progress bars
	/// @param cancelled Polled while running; return true to kill the program
	/// @throws Error if the program can't be started
	Result Run(std::vector<std::string> const& args,
		std::function<void(std::string const&)> const& on_line,
		std::function<bool()> const& cancelled);
}
