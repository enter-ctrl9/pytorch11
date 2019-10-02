#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/Parallel.h>
#include <ATen/NumericUtils.h>
#include <ATen/native/Nonzero.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/cpu/Loops.h>

namespace at { namespace native { namespace {

template<typename scalar_t>
static void nonzero_apply(Tensor& subscript, const Tensor& self) {
  int64_t dimensions = self.dim();
  // +1 faster than additional condition check inside loop
  int64_t *sizes = new int64_t[dimensions+1];
  int64_t *idx = new int64_t[dimensions+1];
  int64_t *ii;
  int64_t *ss;
  std::fill(idx, idx+dimensions+1, 0);
  for (int64_t i = 0; i < dimensions; ++i) {
    sizes[dimensions - i - 1] = self.size(i); // reverse order important
  }
  sizes[dimensions] = 0;
  /* Second pass populates subscripts */
  auto subscript_data = subscript.data_ptr<int64_t>();
  auto subscript_strides = subscript.strides();
  auto stride0 = subscript_strides[0] - subscript_strides[1] * self.dim();
  auto stride1 = subscript_strides[1];

  auto iter = TensorIterator();
  iter.add_input(self);
  iter.build();
  iter.serial_for_each([&](char** data, const int64_t* strides, int64_t n) {
    for (int64_t i = 0; i < n; i++) {
      auto out_ptr = (scalar_t *) (data[0] + i * strides[0]);
      if (*out_ptr != 0) {
        ii = idx + dimensions;
        for (int64_t dim = dimensions - 1; dim >= 0; dim--) {
          --ii;
          *subscript_data = *ii;
          subscript_data += stride1;
        }
        subscript_data += stride0;
      }
      ii = idx;
      ss = sizes;
      ++(*ii);
      while (*ii == *ss) {
        *ii = 0;
        ++ii;
        ++ss;
        ++(*ii);
      }
    }
  }, {0, iter.numel()});
}

static void nonzero_kernel(Tensor& subscript, const Tensor& self) {
  AT_DISPATCH_ALL_TYPES_AND3(kBFloat16, kHalf, kBool, self.scalar_type(), "nonzero", [&] {
    // TODO:Use Norm for large Tensor
    int64_t numel = 0;
    auto iter = TensorIterator();
    iter.add_input(self);
    iter.build();
    iter.serial_for_each([&](char** data, const int64_t* strides, int64_t n) {
      for (int64_t i = 0; i < n; i++) {
        auto out_ptr = (scalar_t*)(data[0] + i * strides[0]);
        if (*out_ptr != 0) {
          numel++;
        }
      }
    }, {0, iter.numel()});
    subscript.resize_({numel, self.dim()});
    if (numel <= 0) {
      return;
    }
    nonzero_apply<scalar_t>(subscript, self);
  });
}

} // anonymous namespace

REGISTER_DISPATCH(nonzero_stub, &nonzero_kernel);

}} //at::native