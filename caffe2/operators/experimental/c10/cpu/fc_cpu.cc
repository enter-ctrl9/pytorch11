#include "caffe2/core/context.h"
#include <ATen/core/dispatch/KernelRegistration.h>
#include "caffe2/core/operator.h"
#include "caffe2/operators/experimental/c10/schemas/fc.h"
#include "caffe2/utils/conversions.h"
#include "caffe2/utils/math.h"
#include "caffe2/core/tensor.h"

using caffe2::BaseContext;
using caffe2::Tensor;

namespace caffe2 {
namespace {
template <class DataType, class Context>
void fc_op_cpu_impl(
    const at::Tensor& X_,
    const at::Tensor& W_,
    const at::Tensor& b_,
    const at::Tensor& Y_,
    int axis,
    int axis_w,
    intrusive_ptr<Blob> state_) {
  Tensor X{C10Tensor(X_)};
  Tensor W{C10Tensor(W_)};
  Tensor b{C10Tensor(b_)};
  Tensor Y{C10Tensor(Y_)};
  caffe2::ops::FullyConnected::State* state = state_->GetMutable<caffe2::ops::FullyConnected::State>();
  CPUContext context;

  constexpr bool TransposeWeight = true;

  CAFFE_ENFORCE(b.dim() == 1, b.dim());
  // batch size
  const auto canonical_axis = X.canonical_axis_index(axis);
  const auto M = X.size_to_dim(canonical_axis);
  const auto K = X.size_from_dim(canonical_axis);
  const auto canonical_axis_w = W.canonical_axis_index(axis_w);
  const int N = TransposeWeight ? W.size_to_dim(canonical_axis_w)
                                : W.size_from_dim(canonical_axis_w);

  auto dimErrorString = [&]() {
    return c10::str(
        "Dimension mismatch: ",
        "X: ",
        X.sizes(),
        ", W: ",
        W.sizes(),
        ", b: ",
        b.sizes(),
        ", axis: ",
        axis,
        ", M: ",
        M,
        ", N: ",
        N,
        ", K: ",
        K);
  };

  // Error checking
  CAFFE_ENFORCE(M == X.numel() / K, dimErrorString());
  CAFFE_ENFORCE(K == W.numel() / N, dimErrorString());
  CAFFE_ENFORCE(N == b.dim32(0), dimErrorString());
  CAFFE_ENFORCE(N == b.numel(), dimErrorString());

  state->Y_shape_cache_ = X.sizes().vec();
  // This is an invariant of canonical_axis, so we can DCHECK.
  DCHECK_LE(canonical_axis + 1, state->Y_shape_cache_.size());
  state->Y_shape_cache_.resize(canonical_axis + 1);
  state->Y_shape_cache_[canonical_axis] = N;
  Y.Resize(state->Y_shape_cache_);
  CAFFE_ENFORCE(M * N == Y.numel(), dimErrorString());

  if (X.numel() == 0) {
    // skip the rest of the computation if X is empty
    Y.template mutable_data<DataType>();
    return;
  }

  // default to FLOAT as math.h does.
  caffe2::TensorProto::DataType math_type = caffe2::TensorProto_DataType_FLOAT;
  if (caffe2::fp16_type<DataType>()) {
    math_type = caffe2::TensorProto_DataType_FLOAT16;
  }

  // W * x
  caffe2::math::Gemm<DataType, Context, caffe2::DefaultEngine>(
      CblasNoTrans,
      TransposeWeight ? CblasTrans : CblasNoTrans,
      M,
      N,
      K,
      1,
      X.template data<DataType>(),
      W.template data<DataType>(),
      0,
      Y.template mutable_data<DataType>(),
      static_cast<Context*>(&context),
      math_type);
  // Add bias term
  Tensor bias_multiplier(state->bias_multiplier_);
  ReinitializeTensor(&bias_multiplier, {M}, at::dtype<DataType>().device(CPU));
  caffe2::math::Set<DataType, Context>(
      M,
      caffe2::convert::To<float, DataType>(1),
      bias_multiplier.template mutable_data<DataType>(),
      static_cast<Context*>(&context));
  caffe2::math::Gemm<DataType, Context, caffe2::DefaultEngine>(
      CblasNoTrans,
      CblasNoTrans,
      M,
      N,
      1,
      1,
      bias_multiplier.template data<DataType>(),
      b.template data<DataType>(),
      1,
      Y.template mutable_data<DataType>(),
      static_cast<Context*>(&context),
      math_type);
}
} // namespace
} // namespace caffe2

namespace c10 {
C10_REGISTER_KERNEL(caffe2::ops::FullyConnected)
    .kernel<&caffe2::fc_op_cpu_impl<float, caffe2::CPUContext>>()
    .dispatchKey(c10::DispatchKey<3>{
        c10::details::TensorParameterDispatchKey{DeviceTypeId::CPU,
                                                 LayoutId(0),
                                                 caffe2::TypeMeta::Id<float>()},
        c10::details::TensorParameterDispatchKey{DeviceTypeId::CPU,
                                                 LayoutId(0),
                                                 caffe2::TypeMeta::Id<float>()},
        c10::details::TensorParameterDispatchKey{
            DeviceTypeId::CPU,
            LayoutId(0),
            caffe2::TypeMeta::Id<float>()}});
} // namespace c10
