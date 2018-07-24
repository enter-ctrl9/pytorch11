#include "torch/csrc/jit/export.h"
#include "torch/csrc/autograd/symbolic.h"
#include "onnx/onnx.pb.h"
#include "torch/csrc/onnx/onnx.h"

#include "torch/csrc/utils/functional.h"
#include <ATen/ATen.h>
#include <ATen/optional.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#include <string>

namespace torch { namespace jit {

namespace {

namespace onnx_torch = ::torch::onnx;
namespace onnx = ::ONNX_NAMESPACE;

std::string getNodeStackTraceString(const Node* n) {
  std::stringstream ss;
  if (n->getSourceLocation()) {
    n->getSourceLocation()->highlight(ss);
  } else {
    ss << "<unknown location>";
  }
  return ss.str();
}

void validateGraph(const std::shared_ptr<Graph>& graph, onnx_torch::OperatorExportTypes operator_export_type) {
  for (auto node : graph->nodes()) {
      // Macro'ed so we get a marginally better line number on failed export
#define FAIL_EXPORT(name) \
      throw std::runtime_error(std::string("ONNX export failed: ") + name + "\n\nGraph we tried to export:\n" + graph->toString());
    IR_IF(node, PythonOp)
      auto py_node = static_cast<torch::jit::PythonOp*>(value);
      FAIL_EXPORT(
          "Couldn't export Python operator " + py_node->name() +
          "\n\nDefined at:\n" + getNodeStackTraceString(node))
    IR_ELSE()
      // Special error messages for certain types of operators
      if (node->kind() == aten::expand) {
        FAIL_EXPORT(
            "Could not export a broadcasted operation; ONNX likely does not support this form of broadcasting.\n\nBroadcast occurred at:\n" +
            getNodeStackTraceString(node));
      }
      if (node->kind() == prim::PackPadded || node->kind() == prim::PadPacked) {
        FAIL_EXPORT(
            "Cannot export individual pack_padded_sequence or pad_packed_sequence; these operations must occur in pairs.\n\nUsage of this operation occurred at:\n" +
            getNodeStackTraceString(node));
      }
      bool is_aten_fallback = operator_export_type == onnx_torch::OperatorExportTypes::ONNX_ATEN_FALLBACK;
      if (!node->kind().is_onnx() && !is_aten_fallback && node->kind() != prim::Undefined) {
        FAIL_EXPORT(
            "Couldn't export operator " + node->kind().toDisplayString() + "\n\nDefined at:\n" +
            getNodeStackTraceString(node));
      }
    IR_END()
#undef FAIL_EXPORT
  }
}

class EncoderBase {
 public:
  EncoderBase(onnx::ModelProto *model_proto,
             int64_t onnx_opset_version,
             onnx_torch::OperatorExportTypes operator_export_type,
             bool defer_weight_export = false);

  const RawDataExportMap &get_raw_data_export_map() const {
    return raw_data_export_map_;
  }

 protected:
  void EncodeGraph(onnx::GraphProto *graph_proto,
                   const std::shared_ptr<Graph> &graph,
                   const std::vector<at::Tensor> &initializers = {});

  void EncodeBlock(onnx::GraphProto *graph_proto,
                   const Block *block,
                   const std::vector<at::Tensor> &initializers = {});

  virtual void EncodeTensor(onnx::TensorProto *tensor_proto,
                    const at::Tensor &tensor,
                    const at::optional<std::string> external_ref = {});

  void AddAttribute(onnx::NodeProto *node_proto, const jit::Node *node, const jit::Symbol name);

  size_t num_blocks_;
  bool defer_weight_export_;
  onnx_torch::OperatorExportTypes operator_export_type_;
  RawDataExportMap raw_data_export_map_;
};

onnx::TensorProto_DataType ATenTypeToOnnxType(at::ScalarType at_type) {
  switch(at_type) {
    case at::kDouble:
      return onnx::TensorProto_DataType_DOUBLE;
    case at::kFloat:
      return onnx::TensorProto_DataType_FLOAT;
    case at::kHalf:
      return onnx::TensorProto_DataType_FLOAT16;
    case at::kByte:
      return onnx::TensorProto_DataType_UINT8;
    case at::kChar:
      return onnx::TensorProto_DataType_INT8;
    case at::kShort:
      return onnx::TensorProto_DataType_INT16;
    case at::kInt:
      return onnx::TensorProto_DataType_INT32;
    case at::kLong:
      return onnx::TensorProto_DataType_INT64;
    default:
      torch::barf("unexpected tensor scalar type");
  }
}

void EncodeTypeProtoTensorType(onnx::TypeProto_Tensor* tensor_type, const Value* n) {
  onnx::TensorShapeProto* shape = tensor_type->mutable_shape();
  if (TensorType* node_type = n->type()->cast<TensorType>()) {
    const std::vector<std::int64_t>& sizes = node_type->sizes();
    for (size_t i = 0; i < sizes.size(); i++) {
      shape->add_dim();
      shape->mutable_dim(i)->set_dim_value(sizes[i]);
    }
    tensor_type->set_elem_type(ATenTypeToOnnxType(node_type->scalarType()));
  }
}

void EncodeValueInfo(onnx::ValueInfoProto* v, const Value* n) {
  v->set_name(n->uniqueName());
  onnx::TypeProto* t = v->mutable_type();
  onnx::TypeProto_Tensor* tensor_type = t->mutable_tensor_type();
  EncodeTypeProtoTensorType(tensor_type, n);
}

EncoderBase::EncoderBase(
    onnx::ModelProto *model_proto,
    int64_t onnx_opset_version,
    onnx_torch::OperatorExportTypes operator_export_type,
    bool defer_weight_export)
    : num_blocks_(0),
      defer_weight_export_(defer_weight_export),
      operator_export_type_(operator_export_type) {
  model_proto->set_producer_name("pytorch");
  model_proto->set_ir_version(3);
  model_proto->set_producer_version("0.4");
  auto* imp = model_proto->add_opset_import();
  // This is the version of ONNX operator set we are targeting
  imp->set_version(onnx_opset_version);
}

void EncoderBase::EncodeGraph(
    onnx::GraphProto *graph_proto,
    const std::shared_ptr<Graph> &graph,
    const std::vector<at::Tensor> &initializers) {
  EncodeBlock(graph_proto, graph->block(), initializers);
}

void EncoderBase::EncodeBlock(
    onnx::GraphProto *graph_proto, const Block *block,
    const std::vector<at::Tensor> &initializers) {
  JIT_ASSERT(graph_proto != nullptr);
  std::string block_name = "torch-jit-export";
  if (num_blocks_) {
    block_name += std::to_string(num_blocks_);
  }
  num_blocks_++;
  graph_proto->set_name(block_name);

  for (auto input : block->inputs()) {
    onnx::ValueInfoProto* v = graph_proto->add_input();
    EncodeValueInfo(v, input);
  }
  for (auto output : block->outputs()) {
    onnx::ValueInfoProto* v = graph_proto->add_output();
    EncodeValueInfo(v, output);
  }
  for (auto node : block->nodes()) {
    bool is_raw_export = operator_export_type_ == onnx_torch::OperatorExportTypes::RAW;
    if (node->kind() == prim::Undefined && !is_raw_export) {
      // Undefined nodes are used to implement optional inputs. One
      // way to "not provide" an optional input is to create an
      // Undefined node, and pass its output as that input.
      continue;
    }
    auto p_n = graph_proto->add_node();
    if (node->getSourceLocation()) {
      std::stringstream ss;
      node->getSourceLocation()->highlight(ss);
      p_n->set_doc_string(ss.str());
    }
    for(auto input : node->inputs()) {
      if (input->node()->kind() == prim::Undefined && !is_raw_export) {
        p_n->add_input("");
      } else {
        p_n->add_input(input->uniqueName());
      }
    }
    for(auto output : node->outputs()) {
      p_n->add_output(output->uniqueName());
    }
    if (is_raw_export) {
      JIT_ASSERT(!node->kind().is_onnx());
      p_n->set_domain(node->kind().domainString());
    }
    else if (operator_export_type_ == onnx_torch::OperatorExportTypes::ONNX) {
      JIT_ASSERT(node->kind().is_onnx());
    }
    p_n->set_op_type(node->kind().toUnqualString());
    for(auto attr_name : node->attributeNames()) {
      AddAttribute(p_n, node, attr_name);
    }
    if (is_raw_export && node->blocks().size() > 0) {
      auto blocks = p_n->add_attribute();
      blocks->set_name("_blocks");
      blocks->set_type(onnx::AttributeProto_AttributeType_GRAPHS);
      for (auto block : node->blocks()) {
        auto graph = blocks->add_graphs();
        EncodeBlock(graph, block, initializers);
      }
    }
    if (node->kind() == torch::jit::onnx::Loop) {
      JIT_ASSERT(node->blocks().size() == 1);

      auto body = p_n->add_attribute();
      body->set_name("body");
      body->set_type(onnx::AttributeProto_AttributeType_GRAPH);
      auto g = body->mutable_g();
      EncodeBlock(g, node->blocks()[0]);
    }
    if (node->kind() == torch::jit::onnx::If) {
      JIT_ASSERT(node->blocks().size() == 2);

      auto true_branch = p_n->add_attribute();
      true_branch->set_name("then_branch");
      true_branch->set_type(onnx::AttributeProto_AttributeType_GRAPH);
      auto true_g = true_branch->mutable_g();
      EncodeBlock(true_g, node->blocks()[0]);

      auto false_branch = p_n->add_attribute();
      false_branch->set_name("else_branch");
      false_branch->set_type(onnx::AttributeProto_AttributeType_GRAPH);
      auto false_g = false_branch->mutable_g();
      EncodeBlock(false_g, node->blocks()[1]);
    }
  }
  auto num_initializers = initializers.size();
  JIT_ASSERT(block->inputs().size() >= num_initializers);
  size_t inputs_count = block->inputs().size() - num_initializers;
  for (auto & tensor : initializers) {
    // TODO: stop using positions to determine which initializers
    // match to which inputs
    std::string name = graph_proto->input(inputs_count++).name();
    auto p = graph_proto->add_initializer();
    p->set_name(name);
    EncodeTensor(p, tensor, name);
  }
}

void EncoderBase::AddAttribute(onnx::NodeProto *node_proto, const jit::Node *node, const jit::Symbol name) {
  auto attr = node_proto->add_attribute();
  JIT_ASSERT(name.is_attr());
  attr->set_name(name.toUnqualString());
  switch(node->kindOf(name)) {
    case AttributeKind::f:
      attr->set_f(node->f(name));
      attr->set_type(onnx::AttributeProto_AttributeType_FLOAT);
      break;
    case AttributeKind::fs:
      attr->set_type(onnx::AttributeProto_AttributeType_FLOATS);
      for(auto & v : node->fs(name))
        attr->add_floats(v);
      break;
    case AttributeKind::i:
      attr->set_type(onnx::AttributeProto_AttributeType_INT);
      attr->set_i(node->i(name));
      break;
    case AttributeKind::is:
      attr->set_type(onnx::AttributeProto_AttributeType_INTS);
      for(auto & v : node->is(name))
        attr->add_ints(v);
      break;
    case AttributeKind::s:
      attr->set_type(onnx::AttributeProto_AttributeType_STRING);
      attr->set_s(node->s(name));
      break;
    case AttributeKind::ss:
      attr->set_type(onnx::AttributeProto_AttributeType_STRINGS);
      for(auto & v : node->ss(name))
        attr->add_strings(v);
      break;
    case AttributeKind::t: {
      attr->set_type(onnx::AttributeProto_AttributeType_TENSOR);
      auto t = attr->mutable_t();
      EncodeTensor(t, node->t(name));
    } break;
    case AttributeKind::ts:
      attr->set_type(onnx::AttributeProto_AttributeType_TENSORS);
      for(auto & v : node->ts(name)) {
        auto t = attr->add_tensors();
        EncodeTensor(t, v);
      }
      break;
    case AttributeKind::g: {
      attr->set_type(onnx::AttributeProto_AttributeType_GRAPH);
      auto g = attr->mutable_g();
      EncodeGraph(g, node->g(name));
    } break;
    case AttributeKind::gs:
      attr->set_type(onnx::AttributeProto_AttributeType_GRAPHS);
      for(auto & v : node->gs(name)) {
        auto g = attr->add_graphs();
        EncodeGraph(g, v);
      }
      break;
  }
}

void EncoderBase::EncodeTensor(
    onnx::TensorProto *tensor_proto,
    const at::Tensor &tensor,
    const at::optional<std::string> external_ref) {
  for(auto d : tensor.sizes()) {
    tensor_proto->add_dims(d);
  }
  tensor_proto->set_data_type(ATenTypeToOnnxType(tensor.type().scalarType()));
  // CPU's HalfTensor doesn't have contiguous(), so first calling contiguous()
  auto t = tensor.contiguous().toBackend(at::kCPU);
  // Add a buffer to the raw_data_export_map for the caller to dump into an
  // external data store. If external_ref is not specified, we instead dump
  // the contiguous data into the protobuf itself
  if (defer_weight_export_) {
    // For now, we use the name of the tensor as the external lookup name to
    // avoid ONNX protobuf changes.
    JIT_ASSERT(external_ref.value() == tensor_proto->name());
    JIT_ASSERT(raw_data_export_map_.count(external_ref.value()) == 0);
    raw_data_export_map_[external_ref.value()] = t;
    tensor_proto->set_raw_data("__EXTERNAL");
  } else {
    JIT_ASSERT(t.is_contiguous());
    tensor_proto->set_raw_data(std::string(static_cast<char*>(t.data_ptr()),  t.type().elementSizeInBytes() * t.numel()));
  }
}

class GraphEncoder: public EncoderBase {
 public:
  GraphEncoder(onnx::ModelProto *model_proto,
               const std::shared_ptr<Graph> &graph,
               int64_t onnx_opset_version,
               onnx_torch::OperatorExportTypes operator_export_type,
               const std::vector<at::Tensor> &initializers,
               bool defer_weight_export);

};

GraphEncoder::GraphEncoder(
    onnx::ModelProto *model_proto,
    const std::shared_ptr<Graph> &graph,
    int64_t onnx_opset_version,
    onnx_torch::OperatorExportTypes operator_export_type,
    const std::vector<at::Tensor> &initializers,
    bool defer_weight_export)
    : EncoderBase(model_proto, onnx_opset_version, operator_export_type, defer_weight_export) {
  if (operator_export_type != onnx_torch::OperatorExportTypes::RAW) {
    validateGraph(graph, operator_export_type);
  }

  EncodeGraph(model_proto->mutable_graph(), graph, initializers);
}

class ModuleEncoder: public EncoderBase {
 public:
  ModuleEncoder(onnx::ModelProto *model_proto,
                const std::shared_ptr<script::Module> &module,
                int64_t onnx_opset_version,
                onnx_torch::OperatorExportTypes operator_export_type);

 private:
  void EncodeModule(onnx::GraphProto *graph_proto, const std::shared_ptr<script::Module> &module);

  void EncodeParameters(onnx::GraphProto *graph_proto,
                        const std::shared_ptr<script::Module> &module,
                        const std::string prefix);

  void EncodeParameter(onnx::TensorProto *tensor_proto,
                       const script::NamedParameter &parameter,
                       const std::string prefix);

  void EncodeMethods(onnx::GraphProto *graph_proto,
                     const std::shared_ptr<script::Module> &module,
                     const std::string prefix);

  void EncodeMethod(onnx::NodeProto *node_proto,
                    const std::unique_ptr<script::Method> &method,
                    const std::string prefix);

  void EncodeTensor(onnx::TensorProto *tensor_proto,
                    const at::Tensor &tensor,
                    const at::optional<std::string> external_ref);

  // Used to deduplicate tensor storages
  std::unordered_map<const void*, std::string> storage_dedup_map_;

  // Used to keep track of Parameter names so Methods can refer to them
  std::unordered_map<at::Tensor*, std::string> parameter_map_;

  // Used to create sequential tensor storage names
  size_t storage_counter_ = 0;
};

ModuleEncoder::ModuleEncoder(
    onnx::ModelProto *model_proto,
    const std::shared_ptr<script::Module> &module,
    int64_t onnx_opset_version,
    onnx_torch::OperatorExportTypes operator_export_type)
    : EncoderBase(model_proto, onnx_opset_version, operator_export_type,
                 /*defer_weight_export*/ true) {
  EncodeModule(model_proto->mutable_graph(), module);
}

void ModuleEncoder::EncodeModule(
    onnx::GraphProto *graph_proto,
    const std::shared_ptr<script::Module> &module) {
  EncodeParameters(graph_proto, module, "");
  EncodeMethods(graph_proto, module, "");
}

void ModuleEncoder::EncodeParameters(
    onnx::GraphProto *graph_proto,
    const std::shared_ptr<script::Module> &module,
    const std::string prefix) {
  // Encode each parameter as a initializer in the proto
  for (auto &parameter : module->get_parameters()) {
    auto tensor_proto = graph_proto->add_initializer();
    EncodeParameter(tensor_proto, parameter.value, prefix);
  }

  for (auto &submodule : module->get_modules()) {
    EncodeParameters(graph_proto, submodule.value.module, prefix + submodule.key + ".");
  }
}

void ModuleEncoder::EncodeParameter(
    onnx::TensorProto *tensor_proto,
    const script::NamedParameter &parameter,
    const std::string prefix) {
  auto tensor = parameter.slot();

  // Name will be prefixed by submodule. e.g. submodule_foo.parameter_bar
  tensor_proto->set_name(prefix + parameter.name);
  parameter_map_[tensor] = tensor_proto->name();

  for (auto &d : tensor->sizes()) {
    tensor_proto->add_dims(d);
  }
  tensor_proto->set_data_type(ATenTypeToOnnxType(tensor->type().scalarType()));

  // The int64_data field stores requires_grad, is_buffer, offset, and strides
  tensor_proto->add_int64_data(tensor->requires_grad());
  tensor_proto->add_int64_data(parameter.is_buffer);
  tensor_proto->add_int64_data(tensor->storage_offset());
  for (auto &d : tensor->strides()) {
    tensor_proto->add_int64_data(d);
  }

  auto storage_ptr = tensor->data_ptr();
  auto dedup_it = storage_dedup_map_.find(storage_ptr);
  if (dedup_it != storage_dedup_map_.end()) {
    tensor_proto->set_doc_string(dedup_it->second);
  } else {
    tensor_proto->set_doc_string(tensor_proto->name());
    JIT_ASSERT(raw_data_export_map_.count(tensor_proto->name()) == 0);
    storage_dedup_map_[storage_ptr] = tensor_proto->name();
    raw_data_export_map_[tensor_proto->name()] = *tensor;
  }
}

void ModuleEncoder::EncodeMethods(
    onnx::GraphProto *graph_proto,
    const std::shared_ptr<script::Module> &module,
    const std::string prefix) {
  // Encode each parameter as a initializer in the proto
  for (auto &method : module->get_methods()) {
    auto node_proto = graph_proto->add_node();
    EncodeMethod(node_proto, method.value, prefix);
  }

  for (auto &submodule : module->get_modules()) {
    EncodeMethods(graph_proto, submodule.value.module, prefix + submodule.key + ".");
  }
}

void ModuleEncoder::EncodeMethod(
    onnx::NodeProto *node_proto,
    const std::unique_ptr<script::Method> &method,
    const std::string prefix) {
  node_proto->set_name(prefix + method->name());

  // Store member_inputs of Method in input
  for (auto &member_input : method->params()) {
    auto it = parameter_map_.find(member_input);
    JIT_ASSERT(it != parameter_map_.end());
    node_proto->add_input(it->second);
  }

  auto attr_proto = node_proto->add_attribute();
  attr_proto->set_type(onnx::AttributeProto_AttributeType_GRAPH);

  for (auto node : method->graph()->nodes()) {
    IR_IF(node, PythonOp)
      auto py_node = static_cast<torch::jit::PythonOp*>(value);
      throw std::runtime_error(
          "Couldn't export Python operator " + py_node->name() +
          "\n\nDefined at:\n" + getNodeStackTraceString(node));
    IR_END()
  }
  EncodeBlock(attr_proto->mutable_g(), method->graph()->block(), {});
}

void ModuleEncoder::EncodeTensor(
    onnx::TensorProto *tensor_proto,
    const at::Tensor &tensor,
    const at::optional<std::string> external_ref) {
  for(auto d : tensor.sizes()) {
    tensor_proto->add_dims(d);
  }
  tensor_proto->set_data_type(ATenTypeToOnnxType(tensor.type().scalarType()));
  auto t = tensor.contiguous().toBackend(at::kCPU);

  // The int64_data field stores offset, and strides
  tensor_proto->add_int64_data(tensor.storage_offset());
  for (auto &d : tensor.strides()) {
    tensor_proto->add_int64_data(d);
  }

  auto storage_ptr = tensor.data_ptr();
  auto dedup_it = storage_dedup_map_.find(storage_ptr);
  if (dedup_it != storage_dedup_map_.end()) {
    tensor_proto->set_doc_string(dedup_it->second);
  } else {
    std::string storage_name = "$" + std::to_string(storage_counter_++);
    tensor_proto->set_doc_string(storage_name);
    JIT_ASSERT(raw_data_export_map_.count(storage_name) == 0);
    storage_dedup_map_[storage_ptr] = storage_name;
    raw_data_export_map_[storage_name] = tensor;
  }
}

// Pretty printing
namespace {
constexpr char indent_char = ' ';
constexpr size_t indent_multiplier = 2;

std::string idt(size_t indent) {
  return std::string(indent * indent_multiplier, indent_char);
}

std::string nlidt(size_t indent) {
  return std::string("\n") + idt(indent);
}

void dump(const onnx::TensorProto& tensor, std::ostream& stream) {
  stream << "TensorProto shape: [";
  for (int i = 0; i < tensor.dims_size(); ++i) {
    stream << tensor.dims(i) << (i == tensor.dims_size() - 1 ? "" : " ");
  }
  stream << "]";
}

void dump(const onnx::TensorShapeProto& shape, std::ostream& stream) {
  for (int i = 0; i < shape.dim_size(); ++i) {
    auto &dim = shape.dim(i);
    if (dim.has_dim_value()) {
      stream << dim.dim_value();
    } else {
      stream << "?";
    }
    stream << (i == shape.dim_size() - 1 ? "" : " ");
  }
}

void dump(const onnx::TypeProto_Tensor& tensor_type, std::ostream& stream) {
  stream << "Tensor dims: ";
  dump(tensor_type.shape(), stream);
}

void dump(const onnx::TypeProto& type, std::ostream& stream) {
  dump(type.tensor_type(), stream);
}

void dump(const onnx::ValueInfoProto& value_info, std::ostream& stream) {
  stream << "{name: \"" << value_info.name()
         << "\", type:";
  dump(value_info.type(), stream);
  stream << "}";
}

void dump(const onnx::GraphProto& graph, std::ostream& stream, size_t indent);

void dump(const onnx::AttributeProto& attr, std::ostream& stream, size_t indent) {
  stream << "{ name: '" << attr.name() << "', type: ";
  if (attr.has_f()) {
    stream << "float, value: " << attr.f();
  } else if (attr.has_i()) {
    stream << "int, value: " << attr.i();
  } else if (attr.has_s()) {
    stream << "string, value: '" << attr.s() << "'";
  } else if (attr.has_g()) {
    stream << "graph, value:\n";
    dump(attr.g(), stream, indent+1);
    stream << nlidt(indent);
  } else if (attr.has_t()) {
    stream << "tensor, value:";
    dump(attr.t(), stream);
  } else if (attr.floats_size()) {
    stream << "floats, values: [";
    for (int i = 0; i < attr.floats_size(); ++i)
      stream << attr.floats(i) << (i == attr.floats_size() - 1 ? "" : " ");
    stream << "]";
  } else if (attr.ints_size()) {
    stream << "ints, values: [";
    for (int i = 0; i < attr.ints_size(); ++i)
      stream << attr.ints(i) << (i == attr.ints_size() - 1 ? "" : " ");
    stream << "]";
  } else if (attr.strings_size()) {
    stream << "strings, values: [";
    for (int i = 0; i < attr.strings_size(); ++i)
      stream << "'" << attr.strings(i) << "'" << (i == attr.strings_size() - 1 ? "" : " ");
    stream << "]";
  } else if (attr.tensors_size()) {
    stream << "tensors, values: [";
    for (auto& t : attr.tensors()) {
      dump(t, stream);
    }
    stream << "]";
  } else if (attr.graphs_size()) {
    stream << "graphs, values: [";
    for (auto& g : attr.graphs()) {
      dump(g, stream, indent+1);
    }
    stream << "]";
  } else {
    stream << "UNKNOWN";
  }
  stream << "}";
}

void dump(const onnx::NodeProto& node, std::ostream& stream, size_t indent) {
  stream << "Node {type: \"" << node.op_type() << "\", inputs: [";
  for (int i = 0; i < node.input_size(); ++i) {
    stream << node.input(i) << (i == node.input_size() - 1 ? "" : ",");
  }
  stream << "], outputs: [";
  for (int i = 0; i < node.output_size(); ++i) {
    stream << node.output(i) << (i == node.output_size() - 1 ? "" : ",");
  }
  stream << "], attributes: [";
  for (int i = 0; i < node.attribute_size(); ++i) {
    dump(node.attribute(i), stream, indent+1);
    stream << (i == node.attribute_size() - 1 ? "" : ",");
  }
  stream << "]}";
}

void dump(const onnx::GraphProto& graph, std::ostream& stream, size_t indent) {
  stream << idt(indent) << "GraphProto {" << nlidt(indent+1)
         << "name: \"" << graph.name() << "\"" << nlidt(indent+1)
         << "inputs: [";
  for (int i = 0; i < graph.input_size(); ++i) {
    dump(graph.input(i), stream);
    stream << (i == graph.input_size() - 1 ? "" : ",");
  }
  stream << "]" << nlidt(indent+1)
         << "outputs: [";
  for (int i = 0; i < graph.output_size(); ++i) {
    dump(graph.output(i), stream);
    stream << (i == graph.output_size() - 1 ? "" : ",");
  }
  stream << "]" << nlidt(indent+1)
         << "initializers: [";
  for (int i = 0; i < graph.initializer_size(); ++i) {
    dump(graph.initializer(i), stream);
    stream << (i == graph.initializer_size() - 1 ? "" : ",");
  }
  stream << "]" << nlidt(indent+1)
         << "nodes: [" << nlidt(indent+2);
  for (int i = 0; i < graph.node_size(); ++i) {
    dump(graph.node(i), stream, indent+2);
    if (i != graph.node_size() - 1) stream << "," << nlidt(indent+2);
  }
  stream << nlidt(indent+1) << "]\n" << idt(indent) << "}\n";
}

void dump(const onnx::OperatorSetIdProto& operator_set_id, std::ostream& stream) {
  stream << "OperatorSetIdProto { domain: " << operator_set_id.domain() << "}";
}

void dump(const onnx::ModelProto& model, std::ostream& stream, size_t indent) {
  stream << idt(indent)
         << "ModelProto {" << nlidt(indent+1)
         << "producer_name: \"" << model.producer_name() << "\"" << nlidt(indent+1)
         << "domain: \"" << model.domain() << "\"" << nlidt(indent+1)
         << "doc_string: \"" << model.doc_string() << "\"";
  if (model.has_graph()) {
    stream << nlidt(indent+1) << "graph:\n";
    dump(model.graph(), stream, indent+2);
  }
  if (model.opset_import_size()) {
    stream << idt(indent+1) << "opset_import: [";
    for (auto &opset_imp : model.opset_import()) {
      dump(opset_imp, stream);
    }
    stream << "],\n";
  }
  stream << idt(indent) << "}\n";
}
} // namespace

std::string prettyPrint(const onnx::ModelProto& model) {
  std::stringstream ss;
  dump(model, ss, 0);
  return ss.str();
}
}

std::string PrettyPrintExportedGraph(
                        const std::shared_ptr<Graph>& graph,
                        const std::vector<at::Tensor> & initializers,
                        int64_t onnx_opset_version,
                        bool defer_weight_export,
                        ::torch::onnx::OperatorExportTypes operator_export_type) {
  ::ONNX_NAMESPACE::ModelProto model_proto;
  auto graph_encoder = GraphEncoder(
    &model_proto, graph, onnx_opset_version, operator_export_type, initializers, defer_weight_export);
  return prettyPrint(model_proto);
}

// export_raw_ir will export IR ops without turning them into ONNX ops.
// The output will use the ONNX protobuf format, but the ops will not
// conform to the ONNX op specification. Thus, the output will not
// be interpretable by a ONNX-compatible framework. However, PyTorch or
// libtorch will be able to import the IR and play it back.
std::tuple<std::string, RawDataExportMap> ExportGraph(
                        const std::shared_ptr<Graph>& graph,
                        const std::vector<at::Tensor> & initializers,
                        int64_t onnx_opset_version,
                        bool defer_weight_export,
                        ::torch::onnx::OperatorExportTypes operator_export_type) {
  ::ONNX_NAMESPACE::ModelProto model_proto;
  auto graph_encoder = GraphEncoder(
    &model_proto, graph, onnx_opset_version, operator_export_type, initializers, defer_weight_export);
  return std::make_tuple(model_proto.SerializeAsString(), graph_encoder.get_raw_data_export_map());
}

class PyTorchFileWriter {
public:
  PyTorchFileWriter(const std::string& filename){
    fp = std::fopen(filename.c_str(), "wb");
    writeFileHeader();
  }

  // Serialize a tensor to file, then return its offset
  size_t serializeTensor(const std::string& name, at::Tensor t) {
    JIT_ASSERT(!finalized);
    JIT_ASSERT(cursor % kFieldAlignment == 0);
    // Keep track of the offset into the file for this tensor
    JIT_ASSERT(key_to_file_offset.count(name) == 0);
    key_to_file_offset[name] = cursor;

    // Write out local "file" header: size of "file" in bytes + padding
    size_t copy_bytes = t.type().elementSizeInBytes() * t.numel();
    write64BitIntegerLittleEndian(copy_bytes);
    padToNextAlignmentBoundary();

    // Write out the actual data
    std::fwrite(t.data_ptr(), copy_bytes, 1u, fp);
    cursor += copy_bytes;
    padToNextAlignmentBoundary();

    return key_to_file_offset[name];
  }

  size_t tensorNameToOffset(const std::string& name) const {
    return key_to_file_offset.at(name);
  }

  size_t getModelProtoOffset() const {
    return key_to_file_offset.at("__MODEL_PROTO");
  }

  // Serialize the model proto to file, replacing the tensor names with
  // string verisions of their file offsets. This also finalizes the file,
  // and calling serializeTensor after calling this method is illegal.
  // NOTE: this method mutates the model proto
  size_t serializeModelProto(::ONNX_NAMESPACE::ModelProto *model_proto) {
    JIT_ASSERT(!finalized);
    JIT_ASSERT(cursor % kFieldAlignment == 0);
    // Keep track of offset into the file for the model proto
    JIT_ASSERT(key_to_file_offset.count("__MODEL_PROTO") == 0);
    key_to_file_offset["__MODEL_PROTO"] = cursor;

    // Swap initializer names to string versions of their file offsets
    auto *graph = model_proto->mutable_graph();
    for (int i=0; i < graph->initializer_size(); ++i) {
      auto *init_proto = graph->mutable_initializer(i);
      if (key_to_file_offset.count(init_proto->doc_string())) {
        init_proto->set_doc_string(std::to_string(key_to_file_offset[init_proto->doc_string()]));
      }
    }

    // Now swap names for tensor valued attributes to string versions of their
    // file offsets
    swapTensorAttributeNames(graph);

    auto serialized_proto = model_proto->SerializeAsString();

    // Write serialized proto size
    size_t serialized_proto_size = serialized_proto.size();
    write64BitIntegerLittleEndian(serialized_proto_size);
    padToNextAlignmentBoundary();

    // Write serialized proto
    std::fwrite(serialized_proto.c_str(), serialized_proto_size, 1, fp);
    cursor += serialized_proto_size;
    padToNextAlignmentBoundary();

    finalized = true;
    return key_to_file_offset.at("__MODEL_PROTO");
  }

  ~PyTorchFileWriter() {
    std::fclose(fp);
  }
private:
  FILE *fp;
  size_t cursor = 0;
  std::unordered_map<std::string, size_t> key_to_file_offset;
  bool finalized = false;

  static constexpr uint64_t kFileFormatVersion = 0x1L;
  static constexpr uint64_t kFileMagicNumber = 0x314843524f545950L; // PYTORCH1
  static constexpr uint64_t kFieldAlignment = 64L; // 64 byte alignment supports up to AVX512 for mmap
  static constexpr uint8_t kPadValue = 0xEF;

  void write64BitIntegerLittleEndian(const uint64_t value) {
    // TODO endian swap on platforms that need it?
    std::fwrite(&value, 8u, 1u, fp);
    cursor += 8u;
  }

  void writePad(const size_t num_bytes) {
    uint8_t pad_val = kPadValue;
    // TODO is there a more efficient way to do this?
    for (size_t i = 0; i < num_bytes; ++i) {
      std::fwrite(&pad_val, 1u, 1u, fp);
    }
    cursor += num_bytes;
  }

  void padToNextAlignmentBoundary() {
    size_t next_offset = (cursor + kFieldAlignment) - (cursor % kFieldAlignment);
    size_t pad_amount = next_offset - cursor;
    writePad(pad_amount);
  }

  void writeFileHeader() {
    write64BitIntegerLittleEndian(kFileMagicNumber);
    write64BitIntegerLittleEndian(kFileFormatVersion);
    padToNextAlignmentBoundary();
  }

  void swapTensorAttributeNames(::ONNX_NAMESPACE::GraphProto *g) {
    for (int i=0; i < g->node_size(); ++i) {
      auto *node = g->mutable_node(i);
      for (int j=0; j < node->attribute_size(); ++j) {
        auto *attr = node->mutable_attribute(j);
        auto mutate_attribute = [this](::onnx_torch::TensorProto *t) {
          if (key_to_file_offset.count(t->doc_string())) {
            t->set_doc_string(std::to_string(key_to_file_offset.at(t->doc_string())));
          }
        };
        if (attr->type() == ::onnx_torch::AttributeProto_AttributeType_TENSOR) {
          mutate_attribute(attr->mutable_t());
        } else if (attr->type() == ::onnx_torch::AttributeProto_AttributeType_TENSORS) {
          for (int k = 0; k < attr->tensors_size(); ++k) {
            mutate_attribute(attr->mutable_tensors(k));
          }
        } else if (attr->type() == ::onnx_torch::AttributeProto_AttributeType_GRAPH) {
          swapTensorAttributeNames(attr->mutable_g());
        } else if (attr->type() == ::onnx_torch::AttributeProto_AttributeType_GRAPHS) {
          for (int k = 0; k < attr->graphs_size(); ++k) {
            swapTensorAttributeNames(attr->mutable_graphs(k));
          }  // for k
        }  // if type
      }  // for j
    }  // for i
  }

};

void writeStoragesAndProtoToFile(const std::string& filename,
                                 ::ONNX_NAMESPACE::ModelProto *model_proto,
                                 const ModuleEncoder &module_encoder) {
  PyTorchFileWriter writer(filename);
  for (const auto& kv : module_encoder.get_raw_data_export_map()) {
    writer.serializeTensor(kv.first, kv.second);
  }
  writer.serializeModelProto(model_proto);
}


void ExportModuleAsPytorchFile(
                        const std::string& filename,
                        const std::shared_ptr<script::Module>& module,
                        int64_t onnx_opset_version,
                        ::torch::onnx::OperatorExportTypes operator_export_type) {
  ::ONNX_NAMESPACE::ModelProto model_proto;
  auto module_encoder = ModuleEncoder(
    &model_proto, module, onnx_opset_version, operator_export_type);
  writeStoragesAndProtoToFile(filename, &model_proto, module_encoder);
}

std::tuple<std::string, RawDataExportMap> ExportModule(
                        const std::shared_ptr<script::Module>& module,
                        int64_t onnx_opset_version,
                        ::torch::onnx::OperatorExportTypes operator_export_type) {
  ::ONNX_NAMESPACE::ModelProto model_proto;
  auto module_encoder = ModuleEncoder(
    &model_proto, module, onnx_opset_version, operator_export_type);
  return std::make_tuple(model_proto.SerializeAsString(), module_encoder.get_raw_data_export_map());
}

}}
