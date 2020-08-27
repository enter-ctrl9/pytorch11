#pragma once

#include <string>
#include <memory>
#include <torch/csrc/WindowsTorchApiMacro.h>

namespace torch { namespace autograd {

struct TORCH_API AnomalyMode {
  static bool is_enabled() {
    return _enabled;
  }
  static void set_enabled(bool enabled) {
    _enabled = enabled;
  }

private:
  static bool _enabled;
};


struct TORCH_API AnomalyMetadata {
  virtual ~AnomalyMetadata();
  virtual void store_stack() = 0;
  virtual void print_stack(const std::string& current_node_name) = 0;
  // using shared_ptr<void> instead of shared_ptr<Node> to avoid circular
  // dependency between this file and "function.h"
  virtual void assign_parent(const std::shared_ptr<void>& parent_node) = 0;
};

}}
