#pragma once

#include "torch/csrc/assertions.h"
#include "torch/csrc/autograd/edge.h"
#include "torch/csrc/autograd/grad_mode.h"
#include "torch/csrc/autograd/profiler.h"
#include "torch/csrc/autograd/saved_variable.h"
#include "torch/csrc/autograd/variable.h"
#include "torch/csrc/jit/tracer.h"
#include "torch/csrc/utils/auto_unique_ptr.h"
#include "torch/csrc/utils/python_stub.h"
#include "torch/csrc/utils/variadic.h"

#include <ATen/ATen.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace torch { namespace autograd {

struct Edge;
struct FunctionPostHook;
struct FunctionPreHook;

using tensor_list = std::vector<at::Tensor>;
using variable_list = std::vector<Variable>;
using edge_list = std::vector<Edge>;
using saved_variable_list = std::vector<SavedVariable>;
using IndexRange = std::pair<size_t, size_t>;

///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
///                               Function
///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/// A `Function` is an abstract class that represents an operation taking zero
/// or more input `Variable`s and producing zero or more output `Variable`s. All
/// functions in PyTorch's autograd machinery derive from this class and
/// override its `apply` method. Instances of such subclasses will then be
/// invokeable via the call operator.
///
///                    Functions in the Autograd Graph
///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/// When viewing the autograd system as a graph, `Function`s are the vertices or
/// nodes, connected to each other via (directed) `Edge`s, which themselves are
/// represented via (`Function`, input_nr) pairs. `Variable`s are the outputs to
/// and inputs of `Function`s, and travel between these edges during execution
/// of the graph. When two or more `Edge`s (from different sources) point at the
/// same input to a `Function`, the values produced along all of these edges are
/// implicitly summed prior to being forwarded to the target `Function`.
///
///                              Hierarchy
///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/// Subclasses will often represent forward passes (functions) or backward
/// passes (derivatives). Note, however, that due to the very general
/// definition of a `Function` taking *zero* or more inputs and producing
/// *zero* or more outputs, uses of `Function`s are flexible and extend beyond
/// purely mathematical operations. For example, the `AccumulateGrad` function
/// is a *sink*: it takes one input, but produces no outputs, instead adding
/// the input to its internal accumulator as a side effect. At the other
/// extreme, the `GraphRoot` function receives no inputs from other functions,
/// but produces multiple outputs.
///
///                              Interface
///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/// The most important method on `Function` is the call operator, which takes in
/// a list of variables and produces a list of variables. The precise size of
/// these lists can be determined with `num_inputs()` and `num_outputs()`.
/// `Function`s are stitched together via their `next_edge` interface, which let
/// you manipulate the set of outgoing edges of a `Function`. You can add an
/// edge with `add_next_edge()`, retrieve an edge with `next_edge(index)` and
/// iterate over them via the `next_edges()` method. Other methods exist for
/// integration with the JIT and other parts of PyTorch. Every `Function` has a
/// *sequence number* that increases monotonically in the order of `Function`
/// construction. It can be retrieved via the `sequence_nr()` method. Note that
/// this sequence number is *thread local*. This means that when `Function`s
/// `A`, `B` and `C` are created consecutively in the same thread, their
/// sequence numbers will be ordered `A` < `B` < `C`. If, however, `A` and `B`
/// are created in one thread and `C` is created in a new thread, `C` will have
/// a *lower* sequence number than `B`.
///~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
struct Function : std::enable_shared_from_this<Function> {
 public:
  /// Create a context edge for the JIT.
  static void set_up_context_edge(
      jit::Node* this_node,
      const variable_list& inputs,
      const variable_list& outputs);

  /// Construct a new `Function` with `num_inputs` inputs and the given
  /// `next_edges`.
  explicit Function(
      uint32_t num_inputs = 0,
      edge_list&& next_edges = edge_list())
      : sequence_nr_(next_sequence_nr_++),
        num_inputs_(num_inputs),
        next_edges_(std::move(next_edges)) {}

  /// Functions are neither copyable nor moveable.
  Function(const Function& other) = delete;
  Function(Function&& other) = delete;
  Function& operator=(const Function& other) = delete;
  Function& operator=(Function&& other) = delete;
  virtual ~Function() = default;

  /// Evaluates the function on the given inputs and return the result of the
  /// function call.
  variable_list operator()(const variable_list& inputs) {
    profiler::RecordFunction rec(this);
    if (jit::tracer::isTracingVar(inputs)) {
      return traced_apply(inputs);
    }
    return apply(inputs);
  }

  // Graph Connectivity API
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  // Inputs

  /// Increments the number of inputs of the function and returns the previous
  /// value.
  uint32_t bump_inputs() noexcept {
    return num_inputs_++;
  }

  void set_num_inputs(uint32_t num_inputs) noexcept {
    num_inputs_ = num_inputs;
  }

  uint32_t num_inputs() const noexcept {
    return num_inputs_;
  }

  // Outputs ("Next Edges")

  const Edge& next_edge(size_t index) const noexcept {
    return next_edges_[index];
  }

  void set_next_edge(size_t index, Edge edge) {
    next_edges_[index] = std::move(edge);
  }

  void add_next_edge(Edge edge) {
    next_edges_.push_back(std::move(edge));
  }

  void set_next_edges(edge_list&& next_edges) {
    next_edges_ = std::move(next_edges);
  }

  void swap_next_edges(edge_list& new_edges) noexcept {
    std::swap(next_edges_, new_edges);
  }

  const edge_list& next_edges() const noexcept {
    return next_edges_;
  }

  uint32_t num_outputs() const noexcept {
    return next_edges_.size();
  }

  // Miscellaneous Methods
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// The unique sequence number of this `Function`.
  uint64_t sequence_nr() const noexcept {
    return sequence_nr_;
  }

  /// Returns a shared pointer to `this`. `PyFunction`s are not managed by
  /// `shared_ptr`s by default, but are bound to the lifetime of their Python
  /// object instead.
  virtual std::shared_ptr<Function> get_shared_ptr() {
    return shared_from_this();
  }

  /// Returns the name of the dynamic type of the function, for debugging.
  virtual std::string name();

  /// Returns true if the particular output edge is active, and that particular
  /// output of this function should be computed.
  bool should_compute_output(size_t output_edge_index) const {
    TORCH_ASSERTM(output_edge_index < num_outputs(), "Index out of range");
    return next_edges_[output_edge_index].is_valid();
  }

  /// Returns true if any of the output edges in any of the ranges are active.
  bool should_compute_output(std::initializer_list<IndexRange> idxs) const {
    return std::any_of(idxs.begin(), idxs.end(), [this](IndexRange range) {
      for (auto i = range.first; i < range.second; i++) {
        if (should_compute_output(i))
          return true;
      }
      return false;
    });
  }

  jit::tracer::FunctionTracingState& tracing_state() noexcept {
    // Dereferencing will create the `TracingState` if the pointer is empty.
    return *tracing_state_;
  }

  /// Returns the `PyObject` stored for this `Function` (for Python
  /// interaction).
  PyObject* pyobj() const noexcept {
    return pyobj_;
  }

  /// Sets the `PyObject` stored for this `Function` (for Python interaction).
  void set_pyobj(PyObject* pyobj) noexcept {
    pyobj_ = pyobj;
  }

  // Hook API
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  void add_post_hook(std::unique_ptr<FunctionPostHook>&& post_hook) {
    post_hooks_.push_back(std::move(post_hook));
  }

  const std::vector<std::unique_ptr<FunctionPostHook>>& post_hooks() const
      noexcept {
    return post_hooks_;
  }

  void clear_post_hooks() {
    post_hooks_.clear();
  }

  void add_pre_hook(std::unique_ptr<FunctionPreHook>&& pre_hook) {
    pre_hooks_.push_back(std::move(pre_hook));
  }

  const std::vector<std::unique_ptr<FunctionPreHook>>& pre_hooks() const
      noexcept {
    return pre_hooks_;
  }

  void clear_pre_hooks() {
    pre_hooks_.clear();
  }

  // Customization Points for Subclasses
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

  /// Releases saved variables if the operation won't be reused.
  virtual void release_variables() {}

  /// Called before an apply if `release_variables()` is going to be called.
  /// Allows larger ops like `InterpreterAutogradFunction` to incrementally
  /// release variables as they run.
  virtual void will_release_variables() {}

  /// Returns true if this function is traceable. An op is traceable if all
  /// operations happening within `apply()` are performed on autograd
  /// `Variables` (i.e. apply mostly instantiates and applies other functions).
  virtual bool is_traceable() {
    return false;
  }

  /// A `Function` is said to pass state transparently to backward, if the
  /// state consists only of (Saved)Variables and only non-variable objects
  /// that parameterize the operation in some way that defines the graph
  /// structure AND the backward function is traceable. In particular,
  /// parametrization MUST NOT depend on the data of any `Variable`.
  /// TODO: it might be possible to handle cases where backward is
  /// non-traceable but state passing could be considered transparent. This
  /// will probably depend on saved_variable_list being mutable.
  /// NOTE: this value matters only if is_traceable() returns false.
  virtual bool passes_state_transparently() {
    return false;
  }

  /// Returns `Variable`s saved by this `Function`.
  /// This let's the JIT find inputs to apply that are not present explicitly
  /// in arguments. Required only for functions that are not traceable, don't
  /// pass state to backward transparently, and are not backwards closures of
  /// functions that don't pass the state transparently. Which means that
  /// hopefully they will hardly ever need to be implemented :)
  virtual std::unique_ptr<saved_variable_list> saved_variables() {
    return nullptr;
  }

 protected:
  /// Monotonically incrementing function counter to supply sequence numbers.
  static thread_local uint64_t next_sequence_nr_;

  /// Performs the `Function`'s actual operation.
  virtual variable_list apply(const variable_list& inputs) = 0;

  /// Calls `apply()`, but instruments it with tracing machinery.
  variable_list traced_apply(variable_list inputs);

  // Since `Function`s are neither copyable nor moveable, we can have const
  // fields.
  const uint64_t sequence_nr_;

  uint32_t num_inputs_;
  edge_list next_edges_;
  PyObject* pyobj_ = nullptr; // weak reference
  std::vector<std::unique_ptr<FunctionPreHook>> pre_hooks_;
  std::vector<std::unique_ptr<FunctionPostHook>> post_hooks_;
  auto_unique_ptr<jit::tracer::FunctionTracingState> tracing_state_;
};

/// See Function::is_traceable() for definition.
struct TraceableFunction : public Function {
  using Function::Function;
  bool is_traceable() final override {
    return true;
  }
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//                       Associated Free Functions
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

namespace detail {
// Implementation of `collect_next_edges` (see below).
struct MakeNextFunctionList : IterArgs<MakeNextFunctionList> {
  edge_list next_edges;
  using IterArgs<MakeNextFunctionList>::operator();
  void operator()(const Variable& variable) {
    if (variable.defined()) {
      next_edges.push_back(variable.gradient_edge());
    } else {
      next_edges.emplace_back();
    }
  }
};
} // namespace detail

/// Create an `Edge` between the given `variable` and the `function`, which is
/// assumed to be the gradient function of this variable (i.e. the function
/// through which this variable is backpropagated during the backward pass).
/// This sets the `grad_fn` property of the `variable`. The particular input of
/// the gradient function this variable will be sent to is determined by the
/// `input_nr` argument. If this variable represents a new input to the gradient
/// function, use the overload of `add_gradient_edge` that does not take an
/// `input_nr` argument, as it will also update the `Function`'s information
/// about the number of inputs it takes.
inline void add_gradient_edge(Variable& variable, Edge edge) {
  variable.set_gradient_edge(std::move(edge));
}

/// Like the previous overload, but assumes that the `Variable` is a new input
/// to the gradient function and its `input_nr` thus equal to
/// `function->num_inputs()`. Additionally, increments the `Function`'s number
/// of inputs by one. Approximately equivalent to add_gradient_edge(variable,
/// Edge(function, function->bump_inputs())).
inline void add_gradient_edge(
    Variable& variable,
    std::shared_ptr<Function> function) {
  // Copy before move.
  const auto input_nr = function->bump_inputs();
  add_gradient_edge(variable, {std::move(function), input_nr});
}

/// Return true if any of the variables in the list require a gradient.
inline bool any_variable_requires_grad(const variable_list& variables) {
  return std::any_of(
      variables.begin(), variables.end(), [](const Variable& variable) {
        return variable.requires_grad();
      });
}

/// Return the next edges of all the given variables, or tuples of variables.
template <typename... Variables>
edge_list collect_next_edges(Variables&&... variables) {
  if (!GradMode::is_enabled())
    return {};
  detail::MakeNextFunctionList make;
  make.apply(std::forward<Variables>(variables)...);
  return std::move(make.next_edges);
}
}} // namespace torch::autograd
