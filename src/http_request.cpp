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

#include "http_request.h"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
/// Attempts after the first one
const int max_retries = 2;

size_t write_cb(char *contents, size_t size, size_t nmemb, void *userp) {
	static_cast<std::string *>(userp)->append(contents, size * nmemb);
	return size * nmemb;
}

int progress_cb(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto cancelled = static_cast<const std::function<bool()> *>(userp);
	return (*cancelled && (*cancelled)()) ? 1 : 0;
}

/// Failures where the same request may well succeed a moment later
bool transient(CURLcode res) {
	switch (res) {
		case CURLE_COULDNT_RESOLVE_HOST:
		case CURLE_COULDNT_CONNECT:
		case CURLE_OPERATION_TIMEDOUT:
		case CURLE_SSL_CONNECT_ERROR:
		case CURLE_SEND_ERROR:
		case CURLE_RECV_ERROR:
		case CURLE_GOT_NOTHING:
		case CURLE_PARTIAL_FILE:
			return true;
		default:
			return false;
	}
}

bool transient(long status) {
	return status == 429 || status == 500 || status == 502 || status == 503 || status == 504;
}

/// Wait before retrying, giving up early if cancelled
void backoff(int attempt, std::function<bool()> const& cancelled) {
	auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2 + attempt * 3);
	while (std::chrono::steady_clock::now() < until) {
		if (cancelled && cancelled()) throw http::Cancelled();
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}

struct DownloadState {
	std::ofstream *out;
	std::function<void(double)> const *progress;
	std::function<bool()> const *cancelled;
};

size_t download_write_cb(char *contents, size_t size, size_t nmemb, void *userp) {
	auto state = static_cast<DownloadState *>(userp);
	state->out->write(contents, static_cast<std::streamsize>(size * nmemb));
	return *state->out ? size * nmemb : 0;
}

int download_progress_cb(void *userp, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
	auto state = static_cast<DownloadState *>(userp);
	if (*state->cancelled && (*state->cancelled)()) return 1;
	if (*state->progress && total > 0)
		(*state->progress)(static_cast<double>(now) / static_cast<double>(total));
	return 0;
}
}

namespace http {
void Download(std::string const& url,
	std::string const& path,
	std::function<void(double)> const& progress,
	std::function<bool()> const& cancelled)
{
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	auto fs_path = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
	CURLcode res;
	long status = 0;
	char error_buffer[CURL_ERROR_SIZE] = {0};
	{
		std::ofstream out(fs_path, std::ios::binary | std::ios::trunc);
		if (!out) throw Error("Could not write " + path);
		DownloadState state{&out, &progress, &cancelled};

		CURL *curl = curl_easy_init();
		if (!curl) throw Error("Could not initialize curl");
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "aegidub");
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
		// Give up on a stalled transfer rather than on a slow one
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
		curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, download_progress_cb);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
		res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
		curl_easy_cleanup(curl);
	}

	if (res != CURLE_OK || status != 200) {
		std::error_code ec;
		std::filesystem::remove(fs_path, ec);
		if (res == CURLE_ABORTED_BY_CALLBACK) throw Cancelled();
		if (res != CURLE_OK)
			throw Error(std::string(curl_easy_strerror(res)) + (error_buffer[0] ? std::string(": ") + error_buffer : std::string()));
		throw Error("HTTP " + std::to_string(status));
	}
}

Response Request(std::string const& url,
	std::vector<std::string> const& headers,
	std::string const* body,
	long timeout,
	std::function<bool()> const& cancelled)
{
	// curl_easy_init would do this itself, but not safely from several
	// threads at once
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

	for (int attempt = 0; ; ++attempt) {
		CURL *curl = curl_easy_init();
		if (!curl)
			throw Error("Could not initialize curl");

		curl_slist *header_list = nullptr;
		for (auto const& header : headers)
			header_list = curl_slist_append(header_list, header.c_str());

		Response response;
		char error_buffer[CURL_ERROR_SIZE] = {0};

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
		if (body) {
			curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
			curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
		}
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "aegidub");
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
		// A model can think for minutes without sending anything; keepalive
		// packets stop routers and firewalls from dropping the idle connection
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 20L);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 10L);
		// Don't fail when the certificate revocation server can't be reached,
		// as happens behind some proxies and antivirus HTTPS scanners
		curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
		curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelled);

		CURLcode res = curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
		curl_slist_free_all(header_list);
		curl_easy_cleanup(curl);

		if (res == CURLE_ABORTED_BY_CALLBACK)
			throw Cancelled();

		if (res != CURLE_OK) {
			if (transient(res) && attempt < max_retries) {
				backoff(attempt, cancelled);
				continue;
			}
			std::string message = std::string(curl_easy_strerror(res)) + " (curl error " + std::to_string(res) + ")";
			if (error_buffer[0])
				message += ": " + std::string(error_buffer);
			if (attempt)
				message += " - gave up after " + std::to_string(attempt + 1) + " attempts";
			throw Error(message);
		}

		if (transient(response.status) && attempt < max_retries) {
			backoff(attempt, cancelled);
			continue;
		}
		return response;
	}
}
}
