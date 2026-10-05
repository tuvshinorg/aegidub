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

#include "ai_batch.h"

#include "json_util.h"

#include <libaegisub/background_runner.h>

#include <algorithm>

namespace ai_batch {
std::string Run(openai::Config const& config, Job const& job, agi::ProgressSink *ps) {
	ps->SetProgress(0, job.count);
	for (size_t start = 0; start < job.count; start += job.batch_size) {
		if (ps->IsCancelled()) return {};
		const size_t end = std::min(job.count, start + job.batch_size);
		ps->SetMessage(job.message(start, end));

		try {
			auto content = openai::CompleteJson(config, job.system, job.request(start, end),
				[ps] { return ps->IsCancelled(); });
			auto root = json_util::parse_json(content);
			json::Object const& obj = root;
			auto lines = json_util::find(obj, "lines");
			if (!lines)
				throw openai::Error("Model reply has no \"lines\" array");

			for (auto const& entry : static_cast<json::Array const&>(*lines)) {
				try {
					json::Object const& item = entry;
					auto id = json_util::find(item, "id");
					if (!id) continue;
					auto i = static_cast<size_t>(static_cast<json::Integer const&>(*id));
					if (i < start || i >= end) continue;
					job.on_item(i, item);
				}
				catch (json::Exception const&) {
					// Skip malformed entries; callers report the shortfall
				}
			}
		}
		catch (openai::Error const& e) {
			return ps->IsCancelled() ? std::string() : std::string(e.what());
		}
		catch (std::exception const& e) {
			return std::string("Could not read the model's reply: ") + e.what();
		}

		ps->SetProgress(end, job.count);
	}
	return {};
}
}
