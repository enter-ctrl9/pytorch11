#include <torch/script.h>

#include "op.h"

#include <memory>
#include <vector>

#include <iostream>

void get_operator_from_registry_and_execute() {
  auto& ops = torch::jit::getAllOperatorsFor(
      torch::jit::Symbol::fromQualString("custom::op"));
  AT_ASSERT(ops.size() == 1);

  auto& op = ops.front();
  AT_ASSERT(op->schema().name == "custom::op");

  torch::jit::Stack stack;
  torch::jit::push(stack, torch::ones(5), 2.0, 3);
  op->getOperation()(stack);
  std::vector<at::Tensor> output;
  torch::jit::pop(stack, output);

  const auto manual = custom_op(torch::ones(5), 2.0, 3);

  AT_ASSERT(output.size() == 3);
  for (size_t i = 0; i < output.size(); ++i) {
    AT_ASSERT(output[i].allclose(torch::ones(5) * 2));
    AT_ASSERT(output[i].allclose(manual[i]));
  }
}

void load_serialized_module_with_custom_op_and_execute(
    const char* path_to_exported_script_module) {
  std::shared_ptr<torch::jit::script::Module> module =
      torch::jit::load(path_to_exported_script_module);
  AT_ASSERT(module != nullptr);

  std::vector<torch::jit::IValue> inputs;
  inputs.push_back(torch::ones(5));
  auto output = module->forward(inputs).toTensor();

  AT_ASSERT(output.allclose(torch::ones(5) + 1));
}

void test_argument_checking_for_serialized_modules(
    const char* path_to_exported_script_module) {
  std::shared_ptr<torch::jit::script::Module> module =
      torch::jit::load(path_to_exported_script_module);
  AT_ASSERT(module != nullptr);

  try {
    module->forward({torch::jit::IValue(1), torch::jit::IValue(2)});
    AT_ASSERT(false);
  } catch (const c10::Error& error) {
    AT_ASSERT(
        std::string(error.what_without_backtrace())
            .find("Expected at most 1 argument(s) for operator 'forward', "
                  "but received 2 argument(s)") == 0);
  }

  try {
    module->forward({torch::jit::IValue(5)});
    AT_ASSERT(false);
  } catch (const c10::Error& error) {
    AT_ASSERT(
        std::string(error.what_without_backtrace())
            .find("Expected value of type Dynamic for argument 'input' in "
                  "position 0, but instead got value of type int") == 0);
  }

  try {
    module->forward({});
    AT_ASSERT(false);
  } catch (const c10::Error& error) {
    AT_ASSERT(
        std::string(error.what_without_backtrace())
            .find("forward() is missing value for argument 'input'") == 0);
  }
}

int main(int argc, const char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: test_custom_ops <path-to-exported-script-module>\n";
    return -1;
  }
  get_operator_from_registry_and_execute();
  load_serialized_module_with_custom_op_and_execute(argv[1]);
  test_argument_checking_for_serialized_modules(argv[1]);
  std::cout << "ok\n";
}
