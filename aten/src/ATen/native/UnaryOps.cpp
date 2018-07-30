// define constants like M_PI and C keywords for MSVC
#ifdef _MSC_VER
#define _USE_MATH_DEFINES
#include <math.h>
#endif

#include "ATen/ATen.h"
#include "ATen/Dispatch.h"
#include "ATen/ExpandUtils.h"
#include "ATen/NativeFunctions.h"
#include "ATen/WrapDimUtils.h"

#include "ATen/CPUApplyUtils.h"
#include "ATen/Parallel.h"
#include "ATen/native/cpu/UnaryOpsKernel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <vector>

#include <map>

// NOTE:
// YOU ARE NOT OBLIGED TO USE THESE MACROS
// If you're writing something more specialized, please don't try to make them
// work for your case, but just write something new instead.

namespace at {
namespace native {

Tensor clamp(const Tensor& self, Scalar min, Scalar max) {
  Tensor result = self.type().tensor();
  return clamp_out(result, self, min, max);
}

Tensor clamp_max(const Tensor& self, Scalar max) {
  Tensor result = self.type().tensor();
  return clamp_max_out(result, self, max);
}

Tensor clamp_min(const Tensor& self, Scalar min) {
  Tensor result = self.type().tensor();
  return clamp_min_out(result, self, min);
}

Tensor& _clamp__cpu(Tensor& self, Scalar min, Scalar max) {
  if (!std::isnan(min.toDouble()) && !std::isnan(max.toDouble())) {
    return _th_clamp_(self, min, max);
  } else if (std::isnan(min.toDouble())) {
    return _th_clamp_max_(self, max);
  } else if (std::isnan(max.toDouble())) {
    return _th_clamp_min_(self, min);
  } else {
    return self;
  }
}

Tensor& _clamp_out_cpu(
    Tensor& result,
    const Tensor& self,
    Scalar min,
    Scalar max) {
  result.resize_(self.sizes());
  result.copy_(self);
  if (!std::isnan(min.toDouble()) && !std::isnan(max.toDouble())) {
    _th_clamp_(result, min, max);
  } else if (std::isnan(min.toDouble())) {
    _th_clamp_max_(result, max);
  } else if (std::isnan(max.toDouble())) {
    _th_clamp_min_(result, min);
  }
  return result;
}

Tensor& _clamp_max__cpu(Tensor& self, Scalar max) {
  return _th_clamp_max_(self, max);
}

Tensor& _clamp_max_out_cpu(Tensor& result, const Tensor& self, Scalar max) {
  result.resize_(self.sizes());
  result.copy_(self);
  return _th_clamp_max_(result, max);
}

Tensor& _clamp_min__cpu(Tensor& self, Scalar min) {
  return _th_clamp_min_(self, min);
}

Tensor& _clamp_min_out_cpu(Tensor& result, const Tensor& self, Scalar min) {
  result.resize_(self.sizes());
  result.copy_(self);
  return _th_clamp_min_(result, min);
}

Tensor& fill_(Tensor& self, Scalar value) {
  return self._fill_(value);
}

Tensor& fill_(Tensor& self, const Tensor& value) {
  return self._fill_(value);
}

Tensor mvlgamma(const Tensor& self, int64_t p) {
  AT_CHECK(at::isFloatingType(self.type().scalarType()),
           "mvlgamma is not implemented for ", self.type());
  AT_CHECK((self > 0.5 * (p - 1.)).all().toCByte(),
           "Condition for computing multivariate log-gamma not met");
  AT_CHECK(p >= 1, "p has to be greater than or equal to 1");
  Tensor args = native::arange(-p / 2. + 0.5, 0.5, 0.5, self.options());
  args = args.add(self.unsqueeze(-1));
  return args.lgamma_().sum(-1).add_(p * (p - 1) * std::log(M_PI) / 4.);
}

Tensor& mvlgamma_(Tensor& self, int64_t p) {
  AT_CHECK(at::isFloatingType(self.type().scalarType()),
           "mvlgamma is not implemented for ", self.type());
  AT_CHECK((self > 0.5 * (p - 1.)).all().toCByte(),
           "Condition for computing multivariate log-gamma not met");
  AT_CHECK(p >= 1, "p has to be greater than or equal to 1");
  Tensor args = native::arange(-p / 2. + 0.5, 0.5, 0.5, self.options());
  args = args.add(self.unsqueeze(-1));
  return self.copy_(args.lgamma_().sum(-1).add_(p * (p - 1) * std::log(M_PI) / 4.));
}

// NB: If you use this macro, you may also need to add a CUDA forwarding
// stub in CUDAUnaryOps

#define IMPLEMENT_UNARY_OP_VEC(op)                              \
  Tensor op(const Tensor& self) {                               \
    Tensor result = self.type().tensor();                       \
    return at::op##_out(result, self);                          \
  }                                                             \
  Tensor& _##op##__cpu(Tensor& self_) {                         \
    if (self_.numel() > 0) {                                    \
      Tensor self = sort_strides(self_);                        \
      op##Impl(kCPU, self, self);                               \
    }                                                           \
    return self_;                                               \
  }                                                             \
  Tensor& _##op##_out_cpu(Tensor& result, const Tensor& self) { \
    result.resize_(self.sizes());                               \
    if (result.numel() > 0) {                                   \
      op##Impl(kCPU, result, self);                             \
    }                                                           \
    return result;                                              \
  }

#define IMPLEMENT_UNARY_OP_TH(op)                               \
  Tensor op(const Tensor& self) {                               \
    Tensor result = self.type().tensor();                       \
    return at::op##_out(result, self);                          \
  }                                                             \
  Tensor& _##op##__cpu(Tensor& self) {                          \
    return at::op##_out(self, self);                            \
  }                                                             \
  Tensor& _##op##_out_cpu(Tensor& result, const Tensor& self) { \
    result.resize_(self.sizes());                               \
    return at::_##op##_out(result, self);                       \
  }

// NB: Temp. defaulting to TH implementation of abs due to issues with Apple

IMPLEMENT_UNARY_OP_TH(abs)
IMPLEMENT_UNARY_OP_VEC(acos)
IMPLEMENT_UNARY_OP_VEC(asin)
IMPLEMENT_UNARY_OP_VEC(atan)
IMPLEMENT_UNARY_OP_VEC(ceil)
IMPLEMENT_UNARY_OP_VEC(cos)
IMPLEMENT_UNARY_OP_TH(cosh)
IMPLEMENT_UNARY_OP_VEC(erf)
IMPLEMENT_UNARY_OP_VEC(erfc)
IMPLEMENT_UNARY_OP_VEC(exp)
IMPLEMENT_UNARY_OP_VEC(expm1)
IMPLEMENT_UNARY_OP_VEC(floor)
IMPLEMENT_UNARY_OP_VEC(log)
IMPLEMENT_UNARY_OP_VEC(log10)
IMPLEMENT_UNARY_OP_VEC(log1p)
IMPLEMENT_UNARY_OP_VEC(log2)
IMPLEMENT_UNARY_OP_VEC(round)
IMPLEMENT_UNARY_OP_VEC(rsqrt)
IMPLEMENT_UNARY_OP_VEC(sigmoid)
IMPLEMENT_UNARY_OP_VEC(sin)
IMPLEMENT_UNARY_OP_TH(sinh)
IMPLEMENT_UNARY_OP_VEC(sqrt)
IMPLEMENT_UNARY_OP_VEC(tan)
IMPLEMENT_UNARY_OP_VEC(tanh)
IMPLEMENT_UNARY_OP_VEC(trunc)

DEFINE_DISPATCH(absImpl);
DEFINE_DISPATCH(acosImpl);
DEFINE_DISPATCH(asinImpl);
DEFINE_DISPATCH(atanImpl);
DEFINE_DISPATCH(ceilImpl);
DEFINE_DISPATCH(cosImpl);
DEFINE_DISPATCH(erfImpl);
DEFINE_DISPATCH(erfcImpl);
DEFINE_DISPATCH(expImpl);
DEFINE_DISPATCH(expm1Impl);
DEFINE_DISPATCH(floorImpl);
DEFINE_DISPATCH(logImpl);
DEFINE_DISPATCH(log10Impl);
DEFINE_DISPATCH(log1pImpl);
DEFINE_DISPATCH(log2Impl);
DEFINE_DISPATCH(roundImpl);
DEFINE_DISPATCH(rsqrtImpl);
DEFINE_DISPATCH(sigmoidImpl);
DEFINE_DISPATCH(sinImpl);
DEFINE_DISPATCH(sqrtImpl);
DEFINE_DISPATCH(tanImpl);
DEFINE_DISPATCH(tanhImpl);
DEFINE_DISPATCH(truncImpl);

}
} // namespace at
