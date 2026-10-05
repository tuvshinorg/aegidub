project('eigen', 'cpp', version: '3.4.90', license: 'MPL-2.0')

# Header-only; the unsupported Tensor and FFT modules are used too
eigen_dep = declare_dependency(include_directories: include_directories('.', is_system: true))
meson.override_dependency('eigen3', eigen_dep)
