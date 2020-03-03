#pragma once

#include <torch/csrc/jit/fuser/common/arith.h>
#include <torch/csrc/jit/fuser/common/index_compute.h>
#include <torch/csrc/jit/fuser/common/predicate_compute.h>
#include <torch/csrc/jit/fuser/common/iriostream.h>
#include <torch/csrc/jit/fuser/common/iter_visitor.h>
#include <torch/csrc/jit/fuser/common/transform_iter.h>

#include <ostream>
#include <stack>

namespace torch {
namespace jit {
namespace fuser {

/*
std::ostream& operator<<(std::ostream& os, std::vector<Int*> vec) {
  os << "<";
  for (int i = 0; i < vec.size(); i++) {
    Printer(os).print_inline(vec[i]);
    if (i == vec.size() - 1)
      os << ">";
    else
      os << ",";
  }
  return os;
}
*/

struct TORCH_API CodeWrite : public Printer {
 private:
  bool producer = false;
  TensorView* consumer = nullptr;
  int extra_indent = 0;

  void print_indices(const std::vector<Int*>&);
  bool print_predicate(const Expr* const expr);

  void print(const TensorView* const);
  void print(const Val* const);
  void print(const UnaryOp* const);
  void print(const BinaryOp* const);

  void indent();
  void handle(Expr*);
  void handle(UnaryOp*);
  void handle(BinaryOp*);

  void updateView(TensorView*);

  std::vector<Int*> getLoopIndices();
  void openFor(IterDomain*);
  void closeScope();
  void resetFors();
  void clearActiveView();

  std::vector<std::pair<Int*, Int*> > fors;

  const TensorView* active_view = nullptr;
  int active_view_axis = 0;
  bool reset_fors = false;

 public:

  CodeWrite(std::ostream& _os) : Printer(_os){}


  void traverse(
      const Fusion* const fusion,
      bool from_outputs_only = false,
      bool breadth_first = false,
      std::unordered_set<ValType> val_types = {});
};

} // namespace fuser
} // namespace jit
} // namespace torch