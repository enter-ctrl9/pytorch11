#if defined USE_CUDA && !(defined _WIN32) && !(defined USE_ROCM)
#pragma once

namespace torch { namespace jit { namespace cudafuser {

// Host-side view of TensorInfo (that visivle for the kernel is defined above).
// Note dims[0] - we need to dynamically allocate the dims.
struct TensorInfo {
  uint32_t* sizes(size_t nDim) { return &sizes_strides[0]; }
  uint32_t* strides(size_t nDim) { return &sizes_strides[nDim]; }

  void* data;
  #pragma GCC diagnostic ignored "-Wpedantic"
    uint32_t sizes_strides[0];
  #pragma GCC diagnostic pop
};

} // namespace cudafuser
} // namespace jit
} // namespace torch

#endif // defined USE_CUDA && !(defined _WIN32) && !(defined USE_ROCM)
