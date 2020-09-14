
#include <torch/csrc/jit/codegen/cuda/kernel.h>
#include <torch/csrc/jit/codegen/cuda/dispatch.h>

#include <unordered_set>

namespace torch {
namespace jit {
namespace fuser {

namespace {

//! Scan all primary expressions in the Kernel IR and build
//! list of specialized nodes
//!
//! \note primary expressions are expressions which are not subexpressions
//!   in a larger expression (things like ForLoop or IfThenElse are not
//!   real expressions)
//!
class KernelIrScanner : private OptOutDispatch {
 public:
  std::vector<kir::Allocate*> global_allocations;
  std::vector<kir::Allocate*> dynamic_allocations;
  std::vector<kir::Allocate*> static_allocations;
  std::unordered_set<Expr*> primary_expressions;

 public:
  explicit KernelIrScanner(const std::vector<Expr*>& exprs) {
    for (auto expr : exprs) {
      handle(expr);
    }
  }

 private:
  void handle(Expr* expr) final {
    TORCH_CHECK(primary_expressions.insert(expr).second);
    OptOutDispatch::handle(expr);
  }

  void handle(kir::ForLoop* fl) final {
    for (auto expr : fl->body().exprs()) {
      handle(expr);
    }
  }

  void handle(kir::IfThenElse* ite) final {
    for (auto expr : ite->thenBody().exprs()) {
      handle(expr);
    }
    for (auto expr : ite->elseBody().exprs()) {
      handle(expr);
    }
  }

  void handle(kir::Allocate* a) final {
    switch (a->getMemoryType()) {
      case MemoryType::Global:
        global_allocations.push_back(a);
        break;
      case MemoryType::Shared:
        if (a->size()->isConstScalar()) {
          static_allocations.push_back(a);
        } else {
          dynamic_allocations.push_back(a);
        }
        break;
      case MemoryType::Local:
        break;
    }
  }
};

} // namespace

// TODO(kir): Kernel IR validation
Kernel::Kernel(
    const std::vector<Expr*>& exprs,
    const ThreadPredicateMap& predicate_map)
    : exprs_(exprs), predicate_map_(predicate_map) {
  analyze();
}

void Kernel::analyze() {
  const KernelIrScanner ir_scanner(exprs_);

  // Cache the list of buffers used within the kernel
  summary_.global_allocations = ir_scanner.global_allocations;
  summary_.dynamic_smem_allocations = ir_scanner.dynamic_allocations;
  summary_.static_smem_allocations = ir_scanner.static_allocations;

  // Figure out if the kernel uses random numbers
  for (auto expr : ir_scanner.primary_expressions) {
    if (expr->getExprType() == ExprType::KirUnaryOp) {
      if (expr->as<kir::UnaryOp>()->getUnaryOpType() == UnaryOpType::RandLike) {
        summary_.is_stochastic = true;
        break;
      }
    }
  }

  // Look for reductions and shared memory buffers
  size_t max_smem_type_size = 0;
  for (auto expr : ir_scanner.primary_expressions) {
    for (auto out : expr->outputs()) {
      if (out->getValType() == ValType::TensorIndex) {
        const auto tv = out->as<kir::TensorIndex>()->view();
        const auto domain = tv->domain();

        // Do we have any reductions?
        summary_.has_block_reductions |= domain->hasBlockReduction();
        summary_.has_grid_reductions |= domain->hasGridReduction();

        // Do we have block broadcasts?
        summary_.has_block_broadcasts |= domain->hasBlockBroadcast();

        // Update the largest smem data type
        if (domain->hasBlockReduction() || domain->hasGridReduction() ||
            tv->memoryType() == MemoryType::Shared) {
          const auto data_type = tv->getDataType().value();
          const size_t type_size = dataTypeSize(data_type);
          if (type_size > max_smem_type_size) {
            max_smem_type_size = type_size;
            summary_.largest_smem_data_type = data_type;
          }
        }
      }
    }
  }
}

} // namespace fuser
} // namespace jit
} // namespace torch
