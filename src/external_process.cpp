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

#include "external_process.h"

#include <algorithm>
#include <deque>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
/// Lines of output kept for error messages
const size_t tail_lines = 12;

/// Splits output into lines and keeps the last few
class LineSplitter {
	std::function<void(std::string const&)> const& on_line;
	std::string pending;
	std::deque<std::string> recent;

	void emit() {
		if (pending.empty()) return;
		if (on_line) on_line(pending);
		recent.push_back(std::move(pending));
		if (recent.size() > tail_lines) recent.pop_front();
		pending.clear();
	}

public:
	LineSplitter(std::function<void(std::string const&)> const& on_line) : on_line(on_line) { }

	void feed(const char *data, size_t size) {
		for (size_t i = 0; i < size; ++i) {
			if (data[i] == '\n' || data[i] == '\r')
				emit();
			else
				pending += data[i];
		}
	}

	std::string finish() {
		emit();
		std::string tail;
		for (auto const& line : recent)
			tail += line + "\n";
		return tail;
	}
};

#ifdef _WIN32
std::wstring widen(std::string const& str) {
	if (str.empty()) return {};
	int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
	std::wstring out(len, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), out.data(), len);
	return out;
}

/// Quote one argument the way the Microsoft C runtime parses command lines
std::wstring quote(std::wstring const& arg) {
	if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
		return arg;

	std::wstring out = L"\"";
	size_t backslashes = 0;
	for (wchar_t ch : arg) {
		if (ch == L'\\') {
			++backslashes;
			continue;
		}
		if (ch == L'"')
			out.append(backslashes * 2 + 1, L'\\');
		else
			out.append(backslashes, L'\\');
		backslashes = 0;
		out += ch;
	}
	out.append(backslashes * 2, L'\\');
	out += L'"';
	return out;
}
#endif
}

namespace external_process {
#ifdef _WIN32
Result Run(std::vector<std::string> const& args,
	std::function<void(std::string const&)> const& on_line,
	std::function<bool()> const& cancelled)
{
	if (args.empty()) throw Error("No program to run");

	std::wstring command_line;
	for (auto const& arg : args) {
		if (!command_line.empty()) command_line += L' ';
		command_line += quote(widen(arg));
	}

	SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
	HANDLE read_pipe = nullptr, write_pipe = nullptr;
	if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
		throw Error("Could not create a pipe");
	SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si{};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = write_pipe;
	si.hStdError = write_pipe;

	PROCESS_INFORMATION pi{};
	BOOL started = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
	CloseHandle(write_pipe);
	if (!started) {
		CloseHandle(read_pipe);
		throw Error("Could not start " + args[0] + " (Windows error " + std::to_string(GetLastError()) + ")");
	}

	Result result;
	LineSplitter lines(on_line);
	char buffer[4096];
	for (;;) {
		if (cancelled && cancelled()) {
			TerminateProcess(pi.hProcess, 1);
			result.cancelled = true;
			break;
		}

		DWORD available = 0;
		if (!PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr)) {
			// The pipe is closed once the program and its children have exited
			break;
		}
		if (available) {
			DWORD read = 0;
			if (!ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read)
				break;
			lines.feed(buffer, read);
		}
		else if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
			// Drain whatever was written just before exiting
			while (PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) && available) {
				DWORD read = 0;
				if (!ReadFile(read_pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read)
					break;
				lines.feed(buffer, read);
			}
			break;
		}
	}

	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(read_pipe);

	result.exit_code = static_cast<int>(exit_code);
	result.tail = lines.finish();
	return result;
}
#else
Result Run(std::vector<std::string> const& args,
	std::function<void(std::string const&)> const& on_line,
	std::function<bool()> const& cancelled)
{
	if (args.empty()) throw Error("No program to run");

	int fds[2];
	if (pipe(fds) != 0) throw Error("Could not create a pipe");

	pid_t pid = fork();
	if (pid < 0) throw Error("Could not start " + args[0]);
	if (pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		std::vector<char *> argv;
		for (auto const& arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		_exit(127);
	}
	close(fds[1]);

	Result result;
	LineSplitter lines(on_line);
	char buffer[4096];
	for (;;) {
		if (cancelled && cancelled()) {
			kill(pid, SIGTERM);
			result.cancelled = true;
			break;
		}
		pollfd pfd{fds[0], POLLIN, 0};
		if (poll(&pfd, 1, 100) > 0) {
			ssize_t n = read(fds[0], buffer, sizeof(buffer));
			if (n <= 0) break;
			lines.feed(buffer, static_cast<size_t>(n));
		}
	}
	close(fds[0]);

	int status = 0;
	waitpid(pid, &status, 0);
	result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (result.exit_code == 127 && !result.cancelled)
		throw Error("Could not start " + args[0]);
	result.tail = lines.finish();
	return result;
}
#endif
}
