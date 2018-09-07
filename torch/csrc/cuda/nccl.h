#pragma once

#include <ATen/ATen.h>
#include <ATen/cuda/CUDAContext.h>
#include <THC/THC.h>

#include <nccl.h>

#include <cstddef>
#include <vector>

namespace torch { namespace cuda { namespace nccl {

// NOTE: this is exposed only so that python_nccl.cpp can some of these helpers.
// Don't use them outside of these files.
namespace detail {

void throw_nccl_error(ncclResult_t status);

static inline void CHECK(ncclResult_t status) {
  if (status != ncclSuccess) {
    throw_nccl_error(status);
  }
}

struct AutoNcclGroup {
  AutoNcclGroup() {
#if defined(NCCL_MAJOR) && (NCCL_MAJOR >= 2)
    CHECK(ncclGroupStart());
#endif
  }
  ~AutoNcclGroup() {
#if defined(NCCL_MAJOR) && (NCCL_MAJOR >= 2)
    CHECK(ncclGroupEnd());
#endif
  }
};

at::ArrayRef<ncclComm_t> _get_communicators(at::TensorList inputs);
void _check_inputs(at::TensorList inputs, at::TensorList outputs,
                   int input_multiplier, int output_multiplier);
ncclDataType_t _get_data_type(const at::Type& type);

} // namespace detail

using comm_list = std::vector<ncclComm_t>;
using stream_list = std::vector<THCStream*>;

std::uint64_t version();

bool is_available(at::TensorList tensors);

void broadcast(at::TensorList tensors,
               const stream_list& streams = {},
               const comm_list& user_comms = {});

size_t get_max_count();

void reduce(
    const std::vector<at::Tensor>& inputs,
    std::vector<at::Tensor>& outputs,
    int32_t root = 0,
    int32_t op = ncclSum,
    at::optional<std::vector<at::cuda::CUDAStream>> streams = at::nullopt,
    at::optional<std::vector<ncclComm_t>> user_comms = at::nullopt);

void reduce(
    std::vector<at::Tensor>& inputs,
    int32_t root = 0,
    int32_t op = ncclSum,
    at::optional<std::vector<at::cuda::CUDAStream>> streams = at::nullopt,
    at::optional<std::vector<ncclComm_t>> user_comms = at::nullopt);

} // namespace nccl
} // namespace cuda
} // namespace torch
