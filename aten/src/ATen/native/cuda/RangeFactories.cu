#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/AccumulateType.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/detail/FunctionTraits.h>
#include <cmath>
#include <limits>

#define GPU_LAMBDA __device__ __host__

namespace {

constexpr int num_threads = C10_WARP_SIZE * 2;
constexpr int thread_work_size = 4;
constexpr int block_work_size = thread_work_size * num_threads;

template<typename index_t, typename func_t>
C10_LAUNCH_BOUNDS_1(num_threads)
__global__ void elementwise_kernel_with_index(index_t N, func_t f, typename function_traits<func_t>::result_type *data) {
  #pragma unroll
  for (int i = 0; i < thread_work_size; i++) {
    index_t idx = block_work_size * blockIdx.x + num_threads * i + threadIdx.x;
    if (idx < N) {
      data[idx] = f(idx);
    }
  }
}

template<typename func_t>
void gpu_kernel_with_index(at::Tensor &output, func_t f) {
  int64_t N = output.numel();
  int64_t grid = (N + block_work_size - 1) / block_work_size;
  auto stream = at::cuda::getCurrentCUDAStream();
  using scalar_t = typename function_traits<func_t>::result_type;
  if (N <= std::numeric_limits<int>::max()) {
    elementwise_kernel_with_index<int><<<grid, num_threads, 0, stream>>>(N, f, output.data_ptr<scalar_t>());
  } else {
    elementwise_kernel_with_index<int64_t><<<grid, num_threads, 0, stream>>>(N, f, output.data_ptr<scalar_t>());
  }
}

}  // namespace

namespace at {
namespace native {

Tensor& linspace_cuda_out(Tensor& result, Scalar start, Scalar end, int64_t steps) {
  TORCH_CHECK(steps >= 0, "number of steps must be non-negative");

  if (result.numel() != steps) {
    result.resize_({steps});
  }
  bool is_contiguous = result.is_contiguous();
  Tensor r = !is_contiguous ? at::empty_like(result, LEGACY_CONTIGUOUS_MEMORY_FORMAT) : result;

  if (steps == 0) {
    // skip
  } else if (steps == 1) {
    r.fill_(start);
  } else if (isIntegralType(r.scalar_type(), 0)) {
    AT_DISPATCH_INTEGRAL_TYPES(r.scalar_type(), "linspace_cuda", [&]() {
      scalar_t scalar_start = start.to<scalar_t>();
      scalar_t scalar_end = end.to<scalar_t>();
      float step = static_cast<float>(scalar_end - scalar_start) / (steps - 1);

      gpu_kernel_with_index(r, [scalar_start, step]GPU_LAMBDA(int64_t ind) -> scalar_t {
        scalar_t val = scalar_start + step * ind;
        return val;
      });
    });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND_HALF(r.scalar_type(), "linspace_cuda", [&]() {
      scalar_t scalar_start = start.to<scalar_t>();
      scalar_t scalar_end = end.to<scalar_t>();
      scalar_t step = (scalar_end - scalar_start) / static_cast<scalar_t>(steps - 1);

      gpu_kernel_with_index(r, [scalar_start, step]GPU_LAMBDA(int64_t ind) -> scalar_t {
        scalar_t val = scalar_start + step * ind;
        return val;
      });
    });
  }

  if(!is_contiguous) {
    result.copy_(r);
  }

  AT_CUDA_CHECK(cudaGetLastError());
  return result;
}

Tensor& logspace_cuda_out(Tensor& result, Scalar start, Scalar end, int64_t steps, double base) {
  TORCH_CHECK(steps >= 0, "number of steps must be non-negative");

  if (result.numel() != steps) {
    result.resize_({steps});
  }
  bool is_contiguous = result.is_contiguous();
  Tensor r = !is_contiguous ? at::empty_like(result, LEGACY_CONTIGUOUS_MEMORY_FORMAT) : result;

  if (steps == 0) {
    // skip
  } else if (steps == 1) {
    r.fill_(std::pow(base, start.to<double>()));
  } else if (isIntegralType(r.scalar_type(), 0)) {
    AT_DISPATCH_INTEGRAL_TYPES(r.scalar_type(), "logspace_cuda", [&]() {
      float scalar_base = static_cast<float>(base); // Use float to avoid promotion to double
      scalar_t scalar_start = start.to<scalar_t>();
      scalar_t scalar_end = end.to<scalar_t>();
      float step = static_cast<float>(scalar_end - scalar_start) / (steps - 1);

      gpu_kernel_with_index(r, [scalar_start, step, scalar_base]GPU_LAMBDA(int64_t ind) -> scalar_t {
        scalar_t val = std::pow(scalar_base, scalar_start + step * ind);
        return val;
      });
    });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND_HALF(r.scalar_type(), "logspace_cuda", [&]() {
      scalar_t scalar_base = static_cast<scalar_t>(base);
      scalar_t scalar_start = start.to<scalar_t>();
      scalar_t scalar_end = end.to<scalar_t>();
      scalar_t step = (scalar_end - scalar_start) / static_cast<scalar_t>(steps - 1);

      gpu_kernel_with_index(r, [scalar_start, step, scalar_base]GPU_LAMBDA(int64_t ind) -> scalar_t {
          scalar_t val = std::pow(scalar_base, scalar_start + step * ind);
          return val;
        });
    });
  }

  if(!is_contiguous) {
    result.copy_(r);
  }

  AT_CUDA_CHECK(cudaGetLastError());
  return result;
}

Tensor& range_cuda_out(Tensor& result, Scalar start, Scalar end, Scalar step) {
  AT_DISPATCH_ALL_TYPES_AND(at::ScalarType::Half, result.scalar_type(), "range_cuda", [&]() {
    using accscalar_t = at::acc_type<scalar_t, true>;
    auto xstart = start.to<accscalar_t>();
    auto xend = end.to<accscalar_t>();
    auto xstep = step.to<accscalar_t>();

    TORCH_CHECK(xstep > 0 || xstep < 0, "step must be nonzero");
    TORCH_CHECK(std::isfinite(static_cast<double>(xstart)) &&
             std::isfinite(static_cast<double>(xend)),
             "unsupported range: ", xstart, " -> ", xend);
    TORCH_CHECK(((xstep > 0) && (xend >= xstart)) || ((xstep < 0) && (xend <= xstart)),
             "upper bound and larger bound inconsistent with step sign");
    int64_t size = static_cast<int64_t>(((xend - xstart) / xstep) + 1);

    if (result.numel() != size) {
      result.resize_({size});
    }
    bool is_contiguous = result.is_contiguous();
    Tensor r = !is_contiguous ?  at::empty_like(result, LEGACY_CONTIGUOUS_MEMORY_FORMAT) : result;

    gpu_kernel_with_index(r, [xstart, xstep]GPU_LAMBDA(int64_t ind) -> scalar_t {
        accscalar_t inc = xstep * static_cast<accscalar_t>(ind);
        accscalar_t val = xstart + inc;
        return static_cast<scalar_t>(val);
    });

    if(!is_contiguous) {
      result.copy_(r);
    }

  });

  AT_CUDA_CHECK(cudaGetLastError());
  return result;
}

Tensor& arange_cuda_out(Tensor& result, Scalar start, Scalar end, Scalar step) {
  AT_DISPATCH_ALL_TYPES_AND(at::ScalarType::Half, result.scalar_type(), "arange_cuda", [&]() {
    using accscalar_t = at::acc_type<scalar_t, true>;
    auto xstart = start.to<accscalar_t>();
    auto xend = end.to<accscalar_t>();
    auto xstep = step.to<accscalar_t>();

    // we use double precision for (start - end) / step
    // to compute size_d for consistency across devices.
    // The problem with using accscalar_t is that accscalar_t might be float32 on gpu for a float32 scalar_t,
    // but double on cpu for the same,
    // and the effective output size starts differing on CPU vs GPU because of precision issues, which
    // we dont want.
    // the corner-case we do want to take into account is int64_t, which has higher precision than double
    double size_d;
    if (std::is_same<scalar_t, int64_t>::value) {
      size_d = std::ceil(static_cast<double>(end.to<accscalar_t>() - start.to<accscalar_t>())
                         / step.to<accscalar_t>());
    } else {
      size_d = std::ceil(static_cast<double>(end.to<double>() - start.to<double>())
                         / step.to<double>());
    }

    TORCH_CHECK(xstep > 0 || xstep < 0, "step must be nonzero");
    TORCH_CHECK(std::isfinite(static_cast<double>(xstart)) &&
             std::isfinite(static_cast<double>(xend)),
             "unsupported range: ", xstart, " -> ", xend);
    TORCH_CHECK(((xstep > 0) && (xend >= xstart)) || ((xstep < 0) && (xend <= xstart)),
             "upper bound and larger bound inconsistent with step sign");

    TORCH_CHECK(size_d >= 0 && size_d <= static_cast<double>(std::numeric_limits<int64_t>::max()),
             "invalid size, possible overflow?");
    int64_t size = static_cast<int64_t>(size_d);
    int64_t numel = result.numel();

    if (numel != size) {
      if(numel > 0){
        TORCH_WARN("The number of elements in the out tensor of shape ", result.sizes(),
                    " is ", numel, " which does not match the computed number of elements ", size,
                    ". Note that this may occur as a result of rounding error. "
                    "The out tensor will be resized to a tensor of shape (", size, ",).");
      }
      result.resize_({size});
    }
    bool is_contiguous = result.is_contiguous();
    Tensor r = !is_contiguous ? at::empty_like(result, LEGACY_CONTIGUOUS_MEMORY_FORMAT) : result;

    gpu_kernel_with_index(r, [xstart, xstep]GPU_LAMBDA(int64_t ind) -> scalar_t {
        accscalar_t inc = xstep * static_cast<accscalar_t>(ind);
        accscalar_t val = xstart + inc;
        return static_cast<scalar_t>(val);
    });

    if(!is_contiguous) {
      result.copy_(r);
    }

  });

  AT_CUDA_CHECK(cudaGetLastError());
  return result;
}

}} // namespace at::native
