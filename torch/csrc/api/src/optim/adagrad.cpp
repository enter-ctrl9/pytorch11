#include <torch/optim/adagrad.h>

#include <torch/csrc/autograd/variable.h>
#include <torch/serialize/archive.h>
#include <torch/utils.h>

#include <ATen/ATen.h>

#include <functional>

namespace torch {
namespace optim {

AdagradOptions::AdagradOptions(double learning_rate)
    : learning_rate_(learning_rate) {}

Adagrad::Adagrad(ParameterContainer&& parameters, const AdagradOptions& options_)
    : Optimizer(std::forward<ParameterContainer>(parameters)), options(options_) {

}

/// Adapted from
/// https://github.com/pytorch/pytorch/blob/master/torch/optim/adagrad.py
void Adagrad::step() {
  for (size_t i = 0; i < parameters_.size(); ++i) {
    Tensor p = parameters_.at(i);
    if (!p.grad().defined()) {
      continue;
    }
    auto grad = p.grad().data();
    // at::IValue curr_state = state[""]
    // state = self.state[p]
    // state['step'] += 1
    if (options.weight_decay() != 0) {
      TORCH_CHECK(!p.grad().data().is_sparse(), "weight_decay option is not compatible with sparse gradients");
      NoGradGuard guard;
      p.grad() = p.grad() + options.weight_decay() * p;
    }

    buffer_at(step_buffers, i) += 1.0;
    const auto clr = options.learning_rate() /
        (1.0 + (buffer_at(step_buffers, i) - 1.0) * options.lr_decay());

    auto& sum = buffer_at(sum_buffers, i);
    sum.addcmul_(p.grad(), p.grad(), 1.0);
    const auto std = buffer_at(sum_buffers, i).sqrt().add_(1e-10);

    NoGradGuard guard;
    p.addcdiv_(p.grad(), std, -clr);
  }
}

void Adagrad::save(serialize::OutputArchive& archive) const {
  serialize(*this, archive);
}

void Adagrad::load(serialize::InputArchive& archive) {
  serialize(*this, archive);
}
} // namespace optim
} // namespace torch
