#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/cuda/detail/KernelUtils.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/cuda/CUDAApplyUtils.cuh>
#include <ATen/native/cuda/Loops.cuh>

constexpr float EPSILON = 1e-12;

namespace {

using namespace at;

void binary_cross_entropy_backward_out_kernel(Tensor& grad_input, const Tensor& grad, const Tensor& input, const Tensor& target) {
  at::TensorIterator iter = TensorIteratorConfig()
      .add_output(grad_input)
      .add_input(grad)
      .add_input(input)
      .add_input(target)
      .build();
  AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, iter.common_dtype(), "binary_cross_entropy_backward_out_cuda", [&]() {
    at::native::gpu_kernel(iter, [] GPU_LAMBDA (
        scalar_t grad_val,
        scalar_t input_val,
        scalar_t target_val
      ) -> scalar_t {
        const scalar_t one = 1;
        const scalar_t epsilon = EPSILON;

        scalar_t grad_input_denominator = max(
          (one - input_val) * input_val,
          epsilon
        );

        return grad_val * (input_val - target_val) / grad_input_denominator;
      }
    );
  });
}

} // namespace

namespace at { namespace native {

Tensor kl_div_backward_cuda(const Tensor& grad, const Tensor& input, const Tensor& target, int64_t reduction, bool log_target) {
  auto grad_input = at::empty_like(input);
  if (!log_target) {
    TensorIterator iter = TensorIteratorConfig()
        .add_output(grad_input)
        .add_input(target)
        .add_input(grad)
        .build();
    AT_DISPATCH_FLOATING_TYPES_AND_HALF(input.scalar_type(), "kl_div_backward_cuda", [&]() {
      scalar_t inv = (reduction == at::Reduction::Mean) ? scalar_t(1.0 / input.numel()) : scalar_t(1.0);
      gpu_kernel(iter,
        [inv] GPU_LAMBDA (scalar_t target_val, scalar_t grad_val) {
          return (target_val > 0) ? scalar_t(-target_val * grad_val * inv) : scalar_t(0.0);
        });
    });
  }
  else {
    grad_input = -at::exp(target) * grad;
    if (reduction == at::Reduction::Mean) {
      grad_input /= input.numel();
    }
  }

  return grad_input;
}

Tensor binary_cross_entropy_cuda(const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

    Tensor loss = at::empty_like(input);
    return at::native::binary_cross_entropy_out_cuda(
        input, target, weight, reduction, loss);
}

Tensor& binary_cross_entropy_out_cuda(const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& loss) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  Tensor loss_squeezed = at::squeeze(loss);

  TensorIterator iter = TensorIteratorConfig()
      .add_output(loss_squeezed)
      .add_owned_input(at::squeeze(input))
      .add_owned_input(at::squeeze(target))
      .build();
  AT_DISPATCH_FLOATING_TYPES_AND2(at::ScalarType::Half, at::ScalarType::BFloat16, iter.common_dtype(), "binary_cross_entropy_out_cuda", [&]() {
    gpu_kernel(iter,
      [] GPU_LAMBDA (scalar_t input_val, scalar_t target_val) -> scalar_t {
        const scalar_t zero = 0;
        const scalar_t one = 1;
        const scalar_t neg_100 = -100;

        CUDA_KERNEL_ASSERT(input_val >= zero && input_val <= one);

        scalar_t log_input_val = std::log(input_val);
        scalar_t log_1_minus_input_val = std::log(one - input_val);

        log_input_val = std::max(log_input_val, neg_100);
        log_1_minus_input_val = std::max(log_1_minus_input_val, neg_100);

        return ((target_val - one) * log_1_minus_input_val) - (target_val * log_input_val);
      }
    );
  });
  if (weight.defined()) {
    loss.mul_(weight);
  }

  if (reduction != at::Reduction::None) {
    Tensor loss_reduced;
    if (reduction == at::Reduction::Mean) {
      loss_reduced = loss.mean();
    } else if (reduction == at::Reduction::Sum) {
      loss_reduced = loss.sum();
    }
    loss.resize_as_(loss_reduced).copy_(loss_reduced);
  }

  return loss;
}

Tensor binary_cross_entropy_backward_cuda(const Tensor& grad, const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  Tensor grad_input = at::empty_like(input);
  return at::native::binary_cross_entropy_backward_out_cuda(
      grad, input, target, weight, reduction, grad_input);
}

Tensor& binary_cross_entropy_backward_out_cuda(const Tensor& grad, const Tensor& input, const Tensor& target, const c10::optional<Tensor>& weight_opt, int64_t reduction, Tensor& grad_input) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  Tensor grad_expand = grad.expand_as(input);
  binary_cross_entropy_backward_out_kernel(grad_input, grad_expand, input, target);

  if (weight.defined()) {
    grad_input.mul_(weight);
  }
  if (reduction == at::Reduction::Mean) {
    grad_input.div_(input.numel());
  }
  return grad_input;
}

// -----------------------------------
// nll_loss forward
// -----------------------------------
namespace {

constexpr int N_THREADS = 32;

template <typename scalar_t>
__global__ void nll_loss_forward_no_reduce_cuda_kernel(
    int64_t batch_size,
    PackedTensorAccessor64<scalar_t, 2> input,
    int64_t* target,
    scalar_t* output,
    scalar_t* weights,
    int64_t n_classes,
    int64_t ignore_index) {
  CUDA_KERNEL_LOOP(index, batch_size) {
    int64_t cur_target = target[index];
    if (cur_target == ignore_index) {
      output[index] = static_cast<scalar_t>(0);
      continue;
    }
    CUDA_KERNEL_ASSERT(cur_target >= 0 && cur_target < n_classes);
    auto cur_weight =
        weights != nullptr ? weights[cur_target] : static_cast<scalar_t>(1);
    output[index] = -cur_weight * input[index][cur_target];
  }
}

template <typename scalar_t>
__global__ void nll_loss_forward_reduce_cuda_kernel_1d(
    scalar_t* output,
    scalar_t* total_weight,
    scalar_t* input,
    int64_t* target,
    scalar_t* weights,
    bool size_average,
    int64_t n_classes,
    int64_t ignore_index) {
  CUDA_KERNEL_ASSERT(threadIdx.x == 0 && threadIdx.y == 0 & threadIdx.z == 0);

  int64_t t = static_cast<int64_t>(*target);
  if (t != static_cast<int64_t>(ignore_index)) {
    CUDA_KERNEL_ASSERT(t >= 0 && t < n_classes);
    scalar_t cur_weight =
        weights != nullptr ? weights[t] : scalar_cast<scalar_t>(1);
    *output = -cur_weight * input[t];
    *total_weight = cur_weight;
    if (size_average && *total_weight > 0) {
      *output /= *total_weight;
    }
  }
}

template <typename scalar_t>
__global__ void nll_loss_forward_reduce_cuda_kernel_2d(
    scalar_t* output,
    scalar_t* total_weight,
    PackedTensorAccessor64<scalar_t, 2> input,
    int64_t* target,
    scalar_t* weights,
    bool size_average,
    int64_t n_classes,
    int64_t ignore_index) {
  // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
  __shared__ scalar_t sh_inputs[N_THREADS], acc_weight[N_THREADS];
  // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
  int64_t i, t;
  // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
  scalar_t cur_weight;

  sh_inputs[threadIdx.x] = static_cast<scalar_t>(0);
  acc_weight[threadIdx.x] = static_cast<scalar_t>(0);
  for (i = threadIdx.x; i < input.size(0); i += N_THREADS) {
    t = target[i];
    if (t != ignore_index) {
      CUDA_KERNEL_ASSERT(t >= 0 && t < n_classes);
      cur_weight = weights != nullptr ? weights[t] : static_cast<scalar_t>(1);
      sh_inputs[threadIdx.x] -= input[i][t] * cur_weight;
      acc_weight[threadIdx.x] += cur_weight;
    }
  }

  __syncthreads();

  if (threadIdx.x == 0) {
    *output = *total_weight = static_cast<scalar_t>(0);
    scalar_t output_acc = 0;
    scalar_t total_weight_acc = 0;
    for (i = 0; i < N_THREADS; ++i) {
      output_acc += sh_inputs[i];
      total_weight_acc += acc_weight[i];
    }
    *total_weight = total_weight_acc;
    *output = output_acc;
    if (size_average) {
      if (input.size(0) == 0) {
        // Mean reduction on empty tensors produces NaN
        *output = std::numeric_limits<double>::quiet_NaN();
      }
      if (*total_weight != 0) {
        *output = output_acc / total_weight_acc;
      }
    }
  }
}

} // namespace

void nll_loss_forward_out_cuda_template(
    Tensor& output,
    Tensor& total_weight,
    const Tensor& input,
    const Tensor& target,
    const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned =
      at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;

  TensorArg output_arg{output, "output", 1};
  TensorArg total_weight_arg{total_weight, "total_weight", 2};
  TensorArg input_arg{input, "input", 3};
  TensorArg target_arg{target, "target", 4};

  if (weight.defined()) {
    TensorArg weight_arg{weight, "weight", 5};
    checkAllSameGPU(
        "nll_loss_forward_out_cuda",
        {output_arg, total_weight_arg, input_arg, target_arg, weight_arg});
  } else {
    checkAllSameGPU(
        "nll_loss_forward_out_cuda",
        {output_arg, total_weight_arg, input_arg, target_arg});
  }

  const int64_t n_classes = input.size(-1);
  const int64_t n_dims = input.dim();
  TORCH_CHECK(n_dims > 0 && n_dims <= 2, "input tensor should be 1D or 2D");
  TORCH_CHECK(
      target.dim() == 1,
      "1D target tensor expected, multi-target not supported");
  TORCH_CHECK(
      input.size(0) == target.size(0),
      "size mismatch (got input: ",
      input.sizes(),
      ", target: ",
      target.sizes(),
      ")")

  TORCH_CHECK(
      !weight.defined() || weight.numel() == n_classes,
      "weight tensor should be defined either for all ",
      n_classes,
      " classes or no classes"
      " but got weight tensor of shape: ",
      weight.sizes());

  int64_t batch_size = n_dims == 1 ? 1 : input.size(0);
  int64_t num_targets = target.size(0);

  if (reduction == Reduction::None & n_dims == 2) {
    output.resize_({batch_size});
    if (batch_size == 0) {
      // This guards from unnecessary operations and launching CUDA kernel with
      // 0 blocks.
      return;
    }

    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "nll_loss_forward_no_reduce_cuda_kernel",
        [&] {
          scalar_t* weight_ptr = nullptr;
          if (weight.defined()) {
            auto weight_ = weight.contiguous();
            weight_ptr = weight_.data_ptr<scalar_t>();
          }
          nll_loss_forward_no_reduce_cuda_kernel<scalar_t>
              <<<at::cuda::detail::GET_BLOCKS(batch_size),
                 at::cuda::detail::CUDA_NUM_THREADS,
                 0,
                 at::cuda::getCurrentCUDAStream()>>>(
                  batch_size,
                  input.packed_accessor64<scalar_t, 2>(),
                  target.data_ptr<int64_t>(),
                  output.data_ptr<scalar_t>(),
                  weight_ptr,
                  n_classes,
                  ignore_index);
          C10_CUDA_KERNEL_LAUNCH_CHECK();
        });
    return;
  }

  output.resize_({});
  total_weight.resize_({});

  auto input_ = input.contiguous();
  auto target_ = target.contiguous();

  if (n_dims == 1) {
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "nll_loss_forward_reduce_cuda_kernel_1d",
        [&] {
          scalar_t* weight_ptr = nullptr;
          if (weight.defined()) {
            auto weight_ = weight.contiguous();
            weight_ptr = weight_.data_ptr<scalar_t>();
          }
          nll_loss_forward_reduce_cuda_kernel_1d<scalar_t>
              <<<1, 1, 0, at::cuda::getCurrentCUDAStream()>>>(
                  output.data_ptr<scalar_t>(),
                  total_weight.data_ptr<scalar_t>(),
                  input_.data_ptr<scalar_t>(),
                  target_.data_ptr<int64_t>(),
                  weight_ptr,
                  reduction == at::Reduction::Mean,
                  n_classes,
                  ignore_index);
          C10_CUDA_KERNEL_LAUNCH_CHECK();
        });
  } else if (n_dims == 2) {
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Half,
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "nll_loss_forward_reduce_cuda_kernel_2d",
        [&] {
          scalar_t* weight_ptr = nullptr;
          if (weight.defined()) {
            auto weight_ = weight.contiguous();
            weight_ptr = weight_.data_ptr<scalar_t>();
          }
          nll_loss_forward_reduce_cuda_kernel_2d<scalar_t>
              <<<1, N_THREADS, 0, at::cuda::getCurrentCUDAStream()>>>(
                  output.data_ptr<scalar_t>(),
                  total_weight.data_ptr<scalar_t>(),
                  input_.packed_accessor64<scalar_t, 2>(),
                  target_.data_ptr<int64_t>(),
                  weight_ptr,
                  reduction == at::Reduction::Mean,
                  n_classes,
                  ignore_index);
          C10_CUDA_KERNEL_LAUNCH_CHECK();
        });
    C10_CUDA_KERNEL_LAUNCH_CHECK();
  }
}

std::tuple<Tensor&, Tensor&> nll_loss_forward_out_cuda(
    const Tensor& self,
    const Tensor& target,
    const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index,
    Tensor& output,
    Tensor& total_weight) {
  nll_loss_forward_out_cuda_template(
      output, total_weight, self, target, weight_opt, reduction, ignore_index);
  return std::tuple<Tensor&, Tensor&>(output, total_weight);
}

std::tuple<Tensor, Tensor> nll_loss_forward_cuda(
    const Tensor& self,
    const Tensor& target,
    const c10::optional<Tensor>& weight_opt,
    int64_t reduction,
    int64_t ignore_index) {
  auto output = at::empty({0}, self.options());
  auto total_weight = at::empty({0}, self.options());
  nll_loss_forward_out_cuda_template(
      output, total_weight, self, target, weight_opt, reduction, ignore_index);
  return std::make_tuple(output, total_weight);
}
}}  // namespace at::native
