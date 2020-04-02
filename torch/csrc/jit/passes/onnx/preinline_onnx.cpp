#include <torch/csrc/jit/passes/onnx/preinline_onnx.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/passes/onnx/helper.h>

namespace torch {
namespace jit {


const auto namespaceName = std::string("___torch_mangle_3");
const auto qualFuncName = std::string("__torch__.torch.nn.functional.") + namespaceName + ".";


void PreInlineCalls(Block* block) {
  for (auto it = block->nodes().begin(), end = block->nodes().end();
       it != end;) {
    Node* cur = *it++;
    switch (cur->kind()) {
      case prim::CallFunction: {
        AT_ASSERT(cur->input(0)->node()->kind() == prim::Constant);
        auto function_constant = cur->input(0)->node();
        auto fun_type =
            function_constant->output()->type()->expect<FunctionType>();

        if (fun_type->function()->qualname().qualifiedName().find(qualFuncName + "interpolate")!= std::string::npos) {
          cur->removeInput(0);
          Node* interpolate_node = block->owningGraph()->create(
              Symbol::fromQualString("aten::__interpolate"),
              {cur->inputs()},
              cur->outputs().size());
          interpolate_node->output()->copyMetadata(cur->output());
          interpolate_node->insertAfter(cur);
          cur->replaceAllUsesWith(interpolate_node);
          cur->removeAllInputs();
          cur->destroy();
          return;
        }
        PreInlineCalls(fun_type->function()->graph()->block());
      } break;
      //case prim::CallMethod: {}
      default: {
        for (auto b : cur->blocks()) {
          PreInlineCalls(b);
        }
      } break;
    }
  }
}

// This pass is to be used for ONNX conversion only. The ONNX converter depends on
// a number of deprecated aten operators. These operators are removed from IR and
// replaced by the compiled python function code. However, in-order to maintain the
// behavior for ONNX conversion, we replace these function calls with the aten symbolic
// which can still be used by the ONNX converter.
void PreInlineONNX(Graph& graph) {
  GRAPH_DUMP("Before Pre-inlining: ", &graph);
  PreInlineCalls(graph.block());
  GRAPH_DUMP("After Pre-inlining: ", &graph);
}

} // namespace jit
} // namespace torch