#include "demucs_bridge.hpp"

#include "model.hpp"

#include <exception>
#include <memory>

namespace {
/// Thrown from the progress callback to stop the model part way
struct Stop { };
}

namespace demucs_bridge {
struct Model {
	demucscpp::demucs_model model;
};

Model *Load(std::string const& path) {
	auto m = std::make_unique<Model>();
	if (!demucscpp::load_demucs_model(path, &m->model))
		return nullptr;
	return m.release();
}

void Free(Model *model) {
	delete model;
}

bool Separate(Model const *model, const float *audio, size_t frames,
	float *vocals, float *background,
	std::function<bool()> const& keep_going, std::string& error)
{
	Eigen::MatrixXf in(2, static_cast<Eigen::Index>(frames));
	for (size_t t = 0; t < frames; ++t) {
		in(0, static_cast<Eigen::Index>(t)) = audio[t * 2];
		in(1, static_cast<Eigen::Index>(t)) = audio[t * 2 + 1];
	}

	Eigen::Tensor3dXf out;
	try {
		out = demucscpp::demucs_inference(model->model, in, [&](float, std::string const&) {
			if (keep_going && !keep_going()) throw Stop();
		});
	}
	catch (Stop const&) {
		return false;
	}
	catch (std::exception const& e) {
		error = e.what();
		return false;
	}

	// htdemucs' sources are drums, bass, other, vocals
	const int vocals_source = 3;
	const int sources = static_cast<int>(out.dimension(0));
	for (size_t t = 0; t < frames; ++t) {
		const auto i = static_cast<Eigen::Index>(t);
		for (int ch = 0; ch < 2; ++ch) {
			float all = 0;
			for (int s = 0; s < sources; ++s)
				all += out(s, ch, i);
			float voice = out(vocals_source, ch, i);
			vocals[t * 2 + ch] = voice;
			background[t * 2 + ch] = all - voice;
		}
	}
	return true;
}
}
