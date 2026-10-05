project('demucscpp', 'cpp', version: '0.0.0', license: 'MIT',
        default_options: ['cpp_std=c++17'])

eigen_dep = dependency('eigen3', fallback: ['eigen', 'eigen_dep'])
cpp = meson.get_compiler('cpp')

# Neural network inference is unusably slow without optimisation, so build
# it optimised even in debug builds of the application
args = []
if cpp.get_argument_syntax() == 'msvc'
    args += ['/O2', '/Ob2', '/DNDEBUG', '/bigobj', '/arch:AVX2', '/D__FMA__', '/fp:fast']
else
    args += ['-O3', '-DNDEBUG', '-mavx2', '-mfma', '-ffast-math']
endif

demucscpp_lib = static_library('demucscpp',
    'src/crosstransformer.cpp',
    'src/dsp.cpp',
    'src/encdec.cpp',
    'src/layers.cpp',
    'src/lstm.cpp',
    'src/model_apply.cpp',
    'src/model_inference.cpp',
    'src/model_load.cpp',
    'bridge/demucs_bridge.cpp',
    include_directories: include_directories('src'),
    dependencies: eigen_dep,
    cpp_args: args,
    override_options: ['optimization=3', 'debug=false', 'b_sanitize=none'],
)

# Only the bridge is exposed: see bridge/demucs_bridge.hpp for why
demucscpp_dep = declare_dependency(
    link_with: demucscpp_lib,
    include_directories: include_directories('bridge'),
)
