#pragma once

#include <torch/nn/module.h>
#include <torch/optim/optimizer.h>

#include <ATen/ATen.h>

#include <cereal/access.hpp>
#include <cereal/cereal.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace torch {
namespace optim {
class SGD : public Optimizer {
 public:
  SGD(std::shared_ptr<nn::Module> model, double lr);

  template <typename ModuleType>
  SGD(nn::ModuleHolder<ModuleType> module_holder, double lr)
      : SGD(module_holder.get(), lr) {}

  TORCH_AUTOGRAD_KWARG(SGD, double, momentum, 0, 0);
  TORCH_AUTOGRAD_KWARG(SGD, double, dampening, 0, 0);
  TORCH_AUTOGRAD_KWARG(SGD, double, weight_decay, 0, 0);
  TORCH_AUTOGRAD_KWARG(SGD, bool, nesterov, false, true);
  double lr_;

  at::Scalar step(
      std::function<at::Scalar()> closure = NoLoss) override;

  template <class Archive>
  void serialize(Archive& ar) {
    ar(CEREAL_NVP(momentum_buffers_));
  }

 private:
  friend class cereal::access;
  SGD() = default;
  std::unordered_map<std::string, at::Tensor> momentum_buffers_;
};
} // namespace optim
} // namespace torch
