import os
import tempfile
from string import Template
import copy
import unittest
import warnings
import inspect

import torch
import common_utils as common
import common_nn
from common_cuda import TEST_CUDA
import torch.utils.cpp_extension
from cpp_api_parity import sample_module, torch_nn_modules, TorchNNTestParams, set_has_parity


torch_nn_has_parity = set([
    'SampleModule',
    'Linear',
])

TORCH_NN_MODULE_COMMON_TEST_HARNESS = """\n
#include <torch/script.h>

const char * const parity_test_error_msg = "Parity test failed";

bool check_tensor_equality(const torch::Tensor& tensor1, const torch::Tensor& tensor2) {
  return tensor1.sizes().vec() == tensor2.sizes().vec() && tensor1.allclose(tensor2);
}

bool check_ivalue_equality(const c10::IValue& ivalue1, const c10::IValue& ivalue2) {
  if (ivalue1.tagKind() != ivalue2.tagKind()) {
    AT_ERROR("Value type mismatch: ", "ivalue1: ", ivalue1.tagKind(), ", ivalue2: ", ivalue2.tagKind());
  }
  if (ivalue1.isInt()) {
    return ivalue1.toInt() == ivalue2.toInt();
  } else if (ivalue1.isDouble()) {
    return ivalue1.toDouble() == ivalue2.toDouble();
  } else if (ivalue1.isBool()) {
    return ivalue1.toBool() == ivalue2.toBool();
  } else if (ivalue1.isString()) {
    return ivalue1.toString() == ivalue2.toString();
  } else if (ivalue1.isTensor()) {
    return check_tensor_equality(ivalue1.toTensor(), ivalue2.toTensor());
  } else {
    AT_ERROR("Unsupported value type: ", ivalue1.tagKind());
  }
}
"""

CHECK_MODULE_PARAM_EQUALITY = Template("""\
TORCH_CHECK(check_tensor_equality(${script_module_prefix}.get_parameter("${param_name}"), ${cpp_module_prefix}->${param_name}), parity_test_error_msg);
""")

CHECK_MODULE_BUFFER_EQUALITY = Template("""\
TORCH_CHECK(check_tensor_equality(${script_module_prefix}.get_buffer("${buffer_name}"), ${cpp_module_prefix}->${buffer_name}), parity_test_error_msg);
""")

CHECK_MODULE_ATTR_EQUALITY = Template("""\
TORCH_CHECK(check_ivalue_equality(${script_module_prefix}.get_attribute("${attr_name}"), c10::IValue(${cpp_module_prefix}->${attr_name})), parity_test_error_msg);
""")

TORCH_NN_MODULE_TEST_CTOR_ARGS = Template("""\n
void ${module_name}_test_ctor_args() {
  ${module_qualified_name} m_init_by_cpp(${module_option});
}
""")

TORCH_NN_MODULE_TEST_INIT = Template("""\n
void ${module_variant_name}_test_init(
    const std::string& saved_module_path,
    const std::string& device) {
  torch::jit::script::Module m_init_by_python = torch::jit::load(saved_module_path);

  torch::manual_seed(2);
  ${module_qualified_name} m_init_by_cpp${cpp_constructor_args};
  m_init_by_cpp->to(device);

  ${extra_stmts}
}
""")

TORCH_NN_MODULE_TEST_FORWARD = Template("""\n
void ${module_variant_name}_test_forward(
    const std::string& saved_module_path,
    const std::string& device,
    torch::Tensor python_output,
    ${input_arg_declarations}) {
  torch::manual_seed(2);
  ${module_qualified_name} module${cpp_constructor_args};
  torch::load(module, saved_module_path);
  module->to(device);

  auto cpp_output = module(${input_args});

  TORCH_CHECK(
    cpp_output.sizes().vec() == python_output.sizes().vec() &&
    cpp_output.allclose(python_output),
    parity_test_error_msg, ": forward output doesn't match");

  ${extra_stmts}
}
""")

TORCH_NN_MODULE_TEST_BACKWARD = Template("""\n
void ${module_variant_name}_test_backward(
    const std::string& saved_module_path,
    const std::string& saved_grad_module_path,
    const std::string& device,
    ${input_arg_declarations}) {
  ${module_qualified_name} grad_module${cpp_constructor_args};
  torch::load(grad_module, saved_grad_module_path);

  torch::manual_seed(2);
  ${module_qualified_name} module${cpp_constructor_args};
  torch::load(module, saved_module_path);
  module->to(device);

  auto cpp_output = module(${input_args});
  cpp_output.sum().backward();

  for (size_t i = 0; i < module->parameters().size(); i++) {
    auto named_param = module->named_parameters()[i];
    auto grad = grad_module->parameters()[i];
    TORCH_CHECK(
      named_param->grad().allclose(grad),
      parity_test_error_msg, ": ", "gradient value of `", named_param.key(), "` doesn't match");
  }

  ${extra_stmts}
}
""")

TORCH_NN_MODULE_IGNORED_ATTRS = ['_backend', '_parameters', '_buffers', '_backward_hooks', '_forward_hooks', '_forward_pre_hooks', '_state_dict_hooks', '_load_state_dict_pre_hooks', '_modules', 'training', 'has_parity']

class TestCppApiParity(common.TestCase):
    def _python_arg_to_cpp_arg(self, python_arg):
        if type(python_arg) == int:
            return 'int64_t', str(python_arg)
        elif type(python_arg) == float:
            return 'double', str(python_arg)
        elif type(python_arg) == bool:
            return 'bool', str(python_arg).lower()
        elif type(python_arg) == str:
            return 'std::string', '"{}"'.format(python_arg)
        elif type(python_arg) == torch.Tensor:
            return 'torch::Tensor', 'torch::empty({})'.format(str(list(python_arg.shape)).replace('[', '{').replace(']', '}'))
        else:
            raise RuntimeError("{} is not a supported arg type for C++ module methods".format(type(python_default_value)))

    def _compile_cpp_code_inline(self, name, cpp_sources, functions):
        # Just-in-time compile the C++ test code
        cpp_module = torch.utils.cpp_extension.load_inline(
            name=name,
            cpp_sources=cpp_sources,
            functions=functions,
            verbose=False,
        )
        return cpp_module

    # This tests that Python and C++ torch.nn modules have matching constructor arg names and types.
    def _test_torch_nn_module_ctor_args(self, module_name):
        python_module_class = getattr(torch.nn, module_name)
        module_metadata = torch_nn_modules.module_metadata_map[module_name]
        python_default_constructor_args = module_metadata['python_default_constructor_args']
        cpp_default_constructor_args = module_metadata['cpp_default_constructor_args']

        cpp_module_option = 'torch::nn::' + module_name + 'Options' + cpp_default_constructor_args
        init_arg_spec = inspect.getfullargspec(python_module_class.__init__)
        for arg_name, python_default_value in zip(init_arg_spec.args[len(python_default_constructor_args) + 1:], init_arg_spec.defaults):
            cpp_module_option += '.{}({})'.format(arg_name, self._python_arg_to_cpp_arg(python_default_value)[1])

        cpp_sources = TORCH_NN_MODULE_COMMON_TEST_HARNESS + module_metadata.get('cpp_sources', '')
        cpp_sources += TORCH_NN_MODULE_TEST_CTOR_ARGS.substitute(
            module_name=module_name,
            module_qualified_name='torch::nn::' + module_name,
            module_option=cpp_module_option)
        cpp_test_name = module_name + '_test_ctor_args'
        cpp_module = self._compile_cpp_code_inline(name=cpp_test_name, cpp_sources=cpp_sources, functions=cpp_test_name)

        getattr(cpp_module, cpp_test_name)()

    def _test_torch_nn_module_variant(self, test_params):
        torch_nn_test_methods = ['init', 'forward', 'backward']

        def generate_test_cpp_sources(test_params, template, extra_stmts):
            example_inputs = test_params.example_inputs
            input_arg_types = [self._python_arg_to_cpp_arg(arg)[0] for arg in example_inputs]
            input_arg_declarations = ',\n'.join([input_arg_types[i] + ' ' + 'arg' + str(i) for i in range(len(input_arg_types))])
            input_args = ',\n'.join(['arg' + str(i) for i in range(len(input_arg_types))])
            test_cpp_sources = template.substitute(
                module_variant_name=test_params.module_variant_name,
                module_qualified_name='torch::nn::' + test_params.module_name,
                cpp_constructor_args=test_params.cpp_constructor_args,
                input_arg_declarations=input_arg_declarations,
                input_args=input_args,
                extra_stmts=extra_stmts)
            return test_cpp_sources

        def setup_init_test(test_params):
            # We are generating the attribute equality checks manually here, because it is not possible to have a `attributes()` API
            # that returns non-parameter / non-buffer attributes in C++ torch::nn module.
            def generate_attr_equality_checks(module, stmts, script_module_prefix='m_init_by_python', cpp_module_prefix='m_init_by_cpp'):
                for name, sub_module in module.named_children():
                    sub_script_module_prefix = '{}.get_module("{}")'.format(script_module_prefix, name)
                    sub_cpp_module_prefix = '{}->{}'.format(cpp_module_prefix, name)
                    generate_attr_equality_checks(sub_module, stmts, sub_script_module_prefix, sub_cpp_module_prefix)
                for name, param in module._parameters.items():
                    stmts += CHECK_MODULE_PARAM_EQUALITY.substitute(
                        script_module_prefix=script_module_prefix,
                        cpp_module_prefix=cpp_module_prefix,
                        param_name=name)
                for name, buffer in module._buffers.items():
                    stmts += CHECK_MODULE_BUFFER_EQUALITY.substitute(
                        script_module_prefix=script_module_prefix,
                        cpp_module_prefix=cpp_module_prefix,
                        buffer_name=name)
                for name, attr in module.__dict__.items():
                    if name not in TORCH_NN_MODULE_IGNORED_ATTRS:
                        stmts += CHECK_MODULE_ATTR_EQUALITY.substitute(
                            script_module_prefix=script_module_prefix,
                            cpp_module_prefix=cpp_module_prefix,
                            attr_name=name)

            device = test_params.device
            python_module_class = test_params.python_module_class
            python_constructor_args = test_params.python_constructor_args
            example_inputs = test_params.example_inputs
            has_parity = test_params.has_parity

            with set_has_parity(has_parity):
                torch.manual_seed(2)
                module = python_module_class(*python_constructor_args).to(device)

            extra_stmt_list = []
            generate_attr_equality_checks(module, extra_stmt_list)
            extra_stmts = ''.join(extra_stmt_list)
            return ([module], device), generate_test_cpp_sources(test_params=test_params, template=TORCH_NN_MODULE_TEST_INIT, extra_stmts=extra_stmts)

        def setup_forward_test(test_params):
            device = test_params.device
            python_module_class = test_params.python_module_class
            python_constructor_args = test_params.python_constructor_args
            example_inputs = test_params.example_inputs
            has_parity = test_params.has_parity

            with set_has_parity(has_parity):
                torch.manual_seed(2)
                module = python_module_class(*python_constructor_args).to(device)
                python_output = module(*example_inputs)

            return ([module], device, python_output, example_inputs), generate_test_cpp_sources(test_params=test_params, template=TORCH_NN_MODULE_TEST_FORWARD, extra_stmts='')

        def setup_backward_test(test_params):
            device = test_params.device
            python_module_class = test_params.python_module_class
            python_constructor_args = test_params.python_constructor_args
            example_inputs = test_params.example_inputs
            has_parity = test_params.has_parity

            with set_has_parity(has_parity):
                torch.manual_seed(2)
                module = python_module_class(*python_constructor_args).to(device)
                python_output = module(*example_inputs)
                python_output.sum().backward()
                # JIT tracing does not save a module's parameters' gradients into ScriptModule.
                # Instead, we create another module `grad_module` with the same structure as `module`,
                # and use `grad_module`'s parameters to save `module`'s corresponding parameters'
                # gradients. Then, we trace both `module` and `grad_module`, serialize them and
                # pass them into C++ for parity testing.
                grad_module = copy.deepcopy(module)
                for param, grad_param in zip(module.parameters(), grad_module.parameters()):
                    if param.grad is not None:
                        grad_param.data = param.grad

            return ([module, grad_module], device, example_inputs), generate_test_cpp_sources(test_params=test_params, template=TORCH_NN_MODULE_TEST_BACKWARD, extra_stmts='')

        def trace_module(module, example_inputs):
            # JIT tracing does not automatically save a module's non-parameter / non-buffer attributes into a ScriptModule's
            # slots, which means we can't access them via `get_attributes()` in C++. Here, we manually register these attributes
            # into the ScriptModule so that we can access them via `get_attributes()` in C++.
            def register_attrs(module, script_module):
                for sub_module, sub_script_module in zip(module.children(), script_module.children()):
                    register_attrs(sub_module, sub_script_module)
                for key, value in module.__dict__.items():
                    if key not in TORCH_NN_MODULE_IGNORED_ATTRS:
                        script_module._c._register_attribute(key, torch.jit.annotations.ann_to_type(type(value)), value)

            # We use JIT tracing to serialize Python module state, so that we can load it into C++
            traced_script_module = torch.jit.trace(module, example_inputs)
            register_attrs(module, traced_script_module)
            return traced_script_module

        def serialize_module_into_file(script_module):
            module_file = tempfile.NamedTemporaryFile(delete=False)
            script_module.save(module_file.name)
            module_file.close()
            return module_file.name

        def test_methods(test_params):
            device = test_params.device
            python_module_class = test_params.python_module_class
            python_constructor_args = test_params.python_constructor_args
            module_variant_name = test_params.module_variant_name
            has_parity = test_params.has_parity
            example_inputs = test_params.example_inputs

            args_map = {}

            cpp_sources = TORCH_NN_MODULE_COMMON_TEST_HARNESS + test_params.cpp_sources

            for method_name in torch_nn_test_methods:
                if method_name == 'init':
                    args_map[method_name], test_cpp_sources = setup_init_test(test_params)
                elif method_name == 'forward':
                    args_map[method_name], test_cpp_sources = setup_forward_test(test_params)
                elif method_name == 'backward':
                    args_map[method_name], test_cpp_sources = setup_backward_test(test_params)
                else:
                    raise RuntimeError("{} is not a supported method to test".format(method_name))
                cpp_sources += test_cpp_sources

            cpp_module = self._compile_cpp_code_inline(
                name=test_params.module_variant_name,
                cpp_sources=cpp_sources,
                functions=[test_params.module_variant_name + '_test_' + method for method in torch_nn_test_methods])

            for method_name in torch_nn_test_methods:
                args = args_map[method_name]
                modules = args[0]
                script_modules = [trace_module(module, example_inputs) for module in modules]
                module_file_names = [serialize_module_into_file(script_module) for script_module in script_modules]

                cpp_args = module_file_names[:]
                for arg in args[1:]:
                    if isinstance(arg, list):
                        cpp_args += arg
                    else:
                        cpp_args.append(arg)

                try:
                    cpp_test_name = module_variant_name + '_test_' + method_name
                    cpp_test_fn = getattr(cpp_module, cpp_test_name)
                    if not test_params.has_parity:
                        with self.assertRaisesRegex(RuntimeError, "Parity test failed"):
                            cpp_test_fn(*cpp_args)
                    else:
                        cpp_test_fn(*cpp_args)
                finally:
                    # Ideally we would like to not have to manually delete the file, but NamedTemporaryFile
                    # opens the file, and it cannot be opened multiple times in Windows. To support Windows,
                    # we close the file after creation and try to remove it manually.
                    for module_file_name in module_file_names:
                        try:
                            os.remove(module_file_name)
                        except OSError as e:
                            warnings.warn("Unable to remove {}, got error: {}".format(module_file_name, str(e)))

        test_methods(test_params)


def _process_test_params(test_params_dict, module_metadata, device):
    module_name = test_params_dict.get('module_name')
    desc = test_params_dict.get('desc', None)
    module_variant_name = module_name + (('_' + desc) if desc else '') + (('_' + device) if device != 'cpu' else '')
    input_size = test_params_dict.get('input_size', None)
    input_fn = test_params_dict.get('input_fn', None)
    if input_size:
        example_inputs = [torch.randn(input_size)]
    elif input_fn:
        example_inputs = list(input_fn())
    else:
        raise RuntimeError("Missing `input_size` or `input_fn` for {}".format(module_variant_name))
    if device != 'cuda' or TEST_CUDA:
        example_inputs = [x.to(device) for x in example_inputs]
    return TorchNNTestParams(
        module_name=module_name,
        module_variant_name = module_variant_name,
        python_constructor_args=test_params_dict.get('constructor_args'),
        cpp_constructor_args=test_params_dict.get('cpp_constructor_args'),
        example_inputs=example_inputs,
        has_parity=test_params_dict.get('has_parity', True),
        python_module_class=getattr(torch.nn, module_name),
        cpp_sources=module_metadata.get('cpp_sources', ''),
        device=device,
    )


def add_test(test_name, test_fn):
    if hasattr(TestCppApiParity, test_name):
        raise RuntimeError("Found two tests with the same name: " + test_name)
    setattr(TestCppApiParity, test_name, test_fn)

torch_nn_modules.module_metadata_map['SampleModule'] = sample_module.module_metadata

torch_nn_test_params_map = {}

torch_nn_module_names = set()

for test_params_dict in sample_module.module_tests + common_nn.module_tests:
    module_name = test_params_dict.get('module_name')
    if module_name in torch_nn_has_parity:
        torch_nn_module_names.add(module_name)
        module_metadata = torch_nn_modules.module_metadata_map[module_name]
        for device in ['cpu', 'cuda']:
            test_params = _process_test_params(
                test_params_dict=test_params_dict,
                module_metadata=module_metadata,
                device=device)
            test_name = 'test_torch_nn_' + test_params.module_variant_name
            torch_nn_test_params_map[test_name] = test_params

            def test_fn(self):
                self._test_torch_nn_module_variant(test_params=torch_nn_test_params_map[self._testMethodName])

            if device == 'cuda':
                test_fn = unittest.skipIf(not TEST_CUDA, "CUDA unavailable")(test_fn)
            add_test(test_name, test_fn)

for module_name in sorted(list(torch_nn_module_names)):
    ctor_args_test_name = 'test_torch_nn_{}_ctor_args'.format(module_name)

    def ctor_args_test(self):
        self._test_torch_nn_module_ctor_args(module_name=self._testMethodName.strip('test_torch_nn_').strip('_ctor_args'))

    add_test(ctor_args_test_name, ctor_args_test)


if __name__ == "__main__":
    common.run_tests()
