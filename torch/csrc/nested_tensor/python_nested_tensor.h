#pragma once

#include <ATen/ATen.h>
#include <torch/csrc/utils/tensor_new.h>
#include <torch/csrc/Exceptions.h>
#include <torch/csrc/tensor/python_tensor.h>
#include <torch/csrc/python_headers.h>
#include <torch/csrc/Device.h>
#include <torch/csrc/Dtype.h>
#include <torch/csrc/autograd/python_variable.h>

// TODO:
// - HANDLE_TH_ERRORS
// - Python exception handling.
// - Implement NestedSize to avoid expensive python ops in *_nested_size/stride
// - map and apply functions that accepted JIT-ed functions to avoid unbind
// - don't make some functions static?

namespace torch {
namespace nested_tensor {

using namespace at;

struct _ListNestedTensor;

struct _VariableNode {
  _VariableNode() {}
  _VariableNode(torch::autograd::Variable variable) : _variable(variable) {}

  torch::autograd::Variable _variable;
};

THP_API PyObject *_ListNestedTensorVariableClass;

// The implicit contract is that, if there are no children, variable_node is defined.
struct _NestedNode {
  _NestedNode() {}
  _NestedNode(const std::vector<_NestedNode> children) : _children(children) {}
  _NestedNode(_VariableNode variable_node) : _variable_node(variable_node) {}
  const std::vector<_NestedNode> _children;
  _VariableNode _variable_node;
};

static size_t _num_tensor(const _NestedNode &meta_node) {
  size_t result = 0;
  for (size_t i = 0; i < meta_node._children.size(); i++) {
    result += _num_tensor(meta_node._children[i]);
  }
  return result;
}

static int64_t _numel(const _NestedNode &meta_node) {
  if (meta_node._children.size() == 0) {
    return meta_node._variable_node._variable.numel();
  } else {
    int64_t result = 0;
    for (size_t i = 0; i < meta_node._children.size(); i++) {
      result += _numel(meta_node._children[i]);
    }
    return result;
  }
}

static _NestedNode _get_structure(PyObject *tensors) {
  if (THPVariable_Check(tensors)) {
    auto variable_ = THPVariable_Unpack(tensors);
    auto variable = make_variable_view(variable_, variable_);
    return _NestedNode(_VariableNode(variable));
  } else {
    std::vector<_NestedNode> meta_nodes;
    Py_ssize_t i, n;
    n = PyObject_Length(tensors);
    PyObject *item;
    if (n < 0) {
      throw python_error();
    }
    for (i = 0; i < n; i++) {
      item = PyList_GetItem(tensors, i);
      _NestedNode node = _get_structure(item);
      meta_nodes.push_back(node);
    }
    return _NestedNode(meta_nodes);
  }
}

static torch::autograd::Variable _get_first_variable(_NestedNode nested_node) {
  const _NestedNode *start = &nested_node;
  while (start->_children.size()) {
    start = &start->_children[0];
  }
  if (start->_variable_node._variable.defined()) {
  return start->_variable_node._variable;
  } else {
    PyObject* fake_args = PyTuple_New(0);
    PyObject* fake_kwargs = PyDict_New();
    // TODO: Update if python_variable updates it too
    return torch::utils::legacy_tensor_ctor(torch::tensors::get_default_tensor_type_id(), torch::tensors::get_default_scalar_type(), fake_args, fake_kwargs);
  }
}

static std::vector<at::IntArrayRef> _get_flat_sizes(_NestedNode nested_node) {
  if (nested_node._children.size() == 0) {
    return std::vector<at::IntArrayRef>(
        {nested_node._variable_node._variable.sizes()});
  } else {
    std::vector<at::IntArrayRef> flat_sizes;
    for (size_t i = 0; i < nested_node._children.size(); i++) {
      auto flat_sizes_i = _get_flat_sizes(nested_node._children[i]);
      for (size_t j = 0; j < flat_sizes_i.size(); j++) {
        flat_sizes.push_back(flat_sizes_i[j]);
      }
    }
    return flat_sizes;
  }
}

template <typename T, class F> static T map(_NestedNode nested_node, F fn) {
  if (nested_node._children.size() == 0) {
    T new_nested_node(_VariableNode(fn(nested_node._variable_node._variable)));
    return new_nested_node;
  } else {
    std::vector<T> new_children;
    for (size_t i = 0; i < nested_node._children.size(); i++) {
      new_children.push_back(T(map<T>(nested_node._children[i], fn)));
    }
    return T(new_children);
  }
}

template <typename T, class F, class G>
static T map_more(_NestedNode nested_node, F fn, G gfn) {
  if (nested_node._children.size() == 0) {
    T result = fn(nested_node._variable_node._variable);
    return result;
  } else {
    std::vector<T> new_children;
    for (size_t i = 0; i < nested_node._children.size(); i++) {
      new_children.push_back(T(map_more<T>(nested_node._children[i], fn, gfn)));
    }
    return gfn(new_children);
  }
}

template <class F>
static void apply2(_NestedNode nested_node1, _NestedNode nested_node2, F fn) {
  if (nested_node1._children.size() == 0) {
    fn(nested_node1._variable_node._variable,
       nested_node2._variable_node._variable);
  } else {
    for (size_t i = 0; i < nested_node1._children.size(); i++) {
      apply2(nested_node1._children[i], nested_node2._children[i], fn);
    }
  }
}

static std::string _NestedNode___str__(const _NestedNode &nested_node) {
  std::stringstream result;
  if (nested_node._children.size() == 0) {
    PyObject *objectsRepresentation =
        PyObject_Str(THPVariable_Wrap(nested_node._variable_node._variable));
    result << PyBytes_AsString(PyUnicode_AsUTF8String(objectsRepresentation));
    return result.str();
  } else {
    result << "nested_tensor([";
    result << std::endl;
    for (_NestedNode node : nested_node._children) {
      result << "  ";
      result << _NestedNode___str__(node);
      result << ",";
      result << std::endl;
    }
    result << "])";
    return result.str();
  }
}

static torch::autograd::Variable
_NestedNode_to_tensor(const _NestedNode &nested_node) {
  if (nested_node._children.size() == 0) {
    return nested_node._variable_node._variable;
  } else {
    std::vector<at::Tensor> variables;
    for (_NestedNode node : nested_node._children) {
      variables.push_back(_NestedNode_to_tensor(node));
    }
    return stack(variables);
  }
}

TORCH_API extern PyTypeObject _ListNestedTensorVariableType;

// TODO: Eventually allow construction from a list of _BufferNestedTensors.
struct TORCH_API _ListNestedTensor {
  _ListNestedTensor() = delete;
  _ListNestedTensor(_NestedNode structure)
      : _structure(structure),
        _first_variable(_get_first_variable(_structure)) {}
  int64_t element_size() { return _first_variable.element_size(); }
  _ListNestedTensor to(at::TensorOptions options, bool non_blocking, bool copy,
                       c10::optional<MemoryFormat> memory_format) {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [&](at::Tensor tensor) -> at::Tensor {
          return tensor.to(options, non_blocking, copy, memory_format);
        }));
  }
  _ListNestedTensor to(ScalarType dtype, bool non_blocking, bool copy,
                       c10::optional<MemoryFormat> memory_format) {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [&](at::Tensor tensor) -> at::Tensor {
          return tensor.to(dtype, non_blocking, copy, memory_format);
        }));
  }
  _ListNestedTensor to(Device device, ScalarType dtype, bool non_blocking,
                       bool copy, c10::optional<MemoryFormat> memory_format) {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [&](at::Tensor tensor) -> at::Tensor {
          return tensor.to(device, dtype, non_blocking, copy, memory_format);
        }));
  }
  _ListNestedTensor pin_memory() {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [](at::Tensor tensor) -> at::Tensor {
          return tensor.pin_memory();
        }));
  }
  _ListNestedTensor grad() {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [](at::Tensor tensor) -> at::Tensor {
          return tensor.grad();
        }));
  }
  _ListNestedTensor detach() {
    return _ListNestedTensor(
        map<_NestedNode>(_structure, [](at::Tensor tensor) -> at::Tensor {
          return tensor.detach();
        }));
  }
  _ListNestedTensor requires_grad_(bool requires_grad) {
    return _ListNestedTensor(map<_NestedNode>(
        _structure, [requires_grad](at::Tensor tensor) -> at::Tensor {
          return tensor.requires_grad_(requires_grad);
        }));
  }
  void backward(_ListNestedTensor gradient, bool retain_graph,
                bool create_graph) {
    apply2(_structure, gradient.get_structure(),
           [retain_graph, create_graph](at::Tensor &tensor1,
                                        const at::Tensor &tensor2) {
             tensor1.backward(tensor2, retain_graph, create_graph);
           });
  }
  int64_t __len__() { return _structure._children.size(); }
  std::string __str__() { return _NestedNode___str__(_structure); }
  // NOTE: Don't delete this. repr is an important concept, this
  // implementation is just faulty due to torch.Tensor.__repr__
  // TODO: Assuming that there is no difference in __str__ and __repr__ for
  // torch.Tensor.
  std::string __repr__() { return _NestedNode___str__(_structure); }
  torch::autograd::Variable to_tensor() {
    return _NestedNode_to_tensor(_structure);
  }
  int64_t nested_dim() {
    const _NestedNode *start_structure = &_structure;
    int64_t depth = 0;
    while (start_structure->_children.size()) {
      depth++;
      start_structure = &start_structure->_children[0];
    }
    return depth;
  }
  at::ScalarType scalar_type() { return _first_variable.scalar_type(); }
  at::Backend backend() { return _first_variable.type().backend(); }
  at::Device device() { return _first_variable.device(); }
  at::TensorOptions options() { return _first_variable.options(); }
  bool requires_grad() { return _first_variable.requires_grad(); }
  int64_t dim() { return _first_variable.dim() + nested_dim(); }
  int64_t numel() { return _numel(_structure); }
  bool is_pinned() { return _first_variable.is_pinned(); }
  bool is_contiguous() { return false; }
  _NestedNode get_structure() { return _structure; }
  // TODO: Implement these and call into them isntead of implementing them
  // _ListNestedTensor to - it's a pain due to the 100s of to overloads
  // std::vector<py::object> nested_size();
  // std::vector<py::object> nested_stride();
  // separately in Variable dispatch functions.
  // std::vector<py::object> unbind();
  // std::string __str__();
  // std::string __repr__();
  // py::tuple size(int64_t dim);

private:
  const _NestedNode _structure;
  const torch::autograd::Variable _first_variable;
};

struct TORCH_API _ListNestedTensorVariable {
  PyObject_HEAD
      /* Type-specific fields go here. */
      _ListNestedTensor cdata;
};

inline bool _ListNestedTensorVariable_Check(PyObject *obj) {
  return _ListNestedTensorVariableClass &&
         PyObject_IsInstance(obj, _ListNestedTensorVariableClass);
}

void initialize_python_bindings();

// Creates a new Python object for a Variable. The Variable must not already
// have a PyObject* associated with it.
static PyObject *
_ListNestedTensorVariable_NewWithVar(PyTypeObject *type,
                                     _ListNestedTensor nested_tensor) {
  PyObject *obj = type->tp_alloc(type, 0);
  if (obj) {
    auto v = (_ListNestedTensorVariable *)obj;
    new (&v->cdata) _ListNestedTensor(std::move(nested_tensor));
    // v->cdata.set_pyobj(obj);
    return obj;
  } else {
    throw python_error();
  }
}

static PyObject *_ListNestedTensorVariable_Wrap(_ListNestedTensor var) {
  return _ListNestedTensorVariable_NewWithVar(
      (PyTypeObject *)_ListNestedTensorVariableClass, std::move(var));
}

} // namespace nested_tensor
} // namespace torch
namespace torch {
namespace autograd {
namespace utils {
inline PyObject *wrap(torch::nested_tensor::_ListNestedTensor nested_tensor) {
  // TODO: Necessary to create new object?
  // What about copy behavior?
  return _ListNestedTensorVariable_Wrap(
      torch::nested_tensor::_ListNestedTensor(nested_tensor));
}
}
}
}
