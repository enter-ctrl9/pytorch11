#include <torch/extension.h>

void foo() { }

TORCH_LIBRARY_IMPL(aten, MSNPU, m) {
  m.impl("foo", foo);
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.def("bar", foo);
}
