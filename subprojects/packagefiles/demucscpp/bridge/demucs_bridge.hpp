// Plain C++ interface to demucs.cpp for aegidub.
//
// Eigen types change layout with the instruction set they are compiled for,
// and demucs.cpp is built with AVX2 while the rest of the program is not, so
// nothing Eigen crosses this interface.

#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace demucs_bridge {
	struct Model;

	/// Load a ggml htdemucs model; nullptr on failure
	Model *Load(std::string const& path);
	void Free(Model *model);

	/// Separate interleaved stereo float audio at 44.1 kHz into the voices
	/// and everything else, both interleaved stereo with the same length
	/// @param keep_going Polled during the work; return false to stop
	/// @return false if stopped or failed; error says why when it failed
	bool Separate(Model const *model, const float *audio, size_t frames,
		float *vocals, float *background,
		std::function<bool()> const& keep_going, std::string& error);
}
