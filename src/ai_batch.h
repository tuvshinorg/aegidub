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

/// @file ai_batch.h
/// @brief Sending numbered subtitle lines to a chat model a batch at a time

#pragma once

#include "openai_client.h"

#include <libaegisub/cajun/elements.h>

#include <cstddef>
#include <functional>
#include <string>

namespace agi { class ProgressSink; }

namespace ai_batch {
	struct Job {
		/// Number of items, each identified by its index
		size_t count;
		size_t batch_size;
		/// System prompt for every request
		std::string system;
		/// User message for items [start, end); each item's "id" is its index
		std::function<std::string(size_t start, size_t end)> request;
		/// Called for each item of the reply's "lines" array with a valid id
		/// in the batch; may throw json::Exception for a malformed item,
		/// which is then skipped
		std::function<void(size_t id, json::Object const& item)> on_item;
		/// Progress message for a batch, e.g. "Directing lines 1-60 of 900"
		std::function<std::string(size_t start, size_t end)> message;
	};

	/// Run the batches in order, stopping at the first error or on cancel
	/// @return An error message, or empty when finished or cancelled
	std::string Run(openai::Config const& config, Job const& job, agi::ProgressSink *ps);
}
