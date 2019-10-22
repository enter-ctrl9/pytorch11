#pragma once

#include <torch/arg.h>
#include <torch/csrc/WindowsTorchApiMacro.h>
#include <torch/types.h>


namespace torch {
namespace nn {

/// Options for `Dropout` and `FeatureDropout`.
struct TORCH_API DropoutOptions {
  /* implicit */ DropoutOptions(double p = 0.5, bool inplace = false);
  /// The probability with which a particular component of the input is set to
  /// zero.
  /// Changes to this parameter at runtime are effective.
  TORCH_ARG(double, p);
  TORCH_ARG(bool, inplace);
};

} // namespace nn
} // namespace torch
