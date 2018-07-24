#pragma once

#include "torch/csrc/jit/ir.h"
#include "torch/csrc/jit/script/module.h"

namespace torch { namespace jit {

TORCH_API std::shared_ptr<Graph> ImportIRGraph(const std::string& serialized_graph, std::vector<at::Tensor> & initializers);

std::shared_ptr<script::Module> ImportIRModule(
    const std::shared_ptr<script::Module> module,
    const std::string& serialized_module,
    const std::unordered_map<std::string, std::string>& storage_map);

std::shared_ptr<script::Module> ImportIRModuleFromPyTorchFile(
    const std::shared_ptr<script::Module> module,
    const std::string& filename);

}}  // namespace torch::jit
