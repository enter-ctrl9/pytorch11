#include "nomnigraph/Representations/NeuralNet.h"
#include <queue>
#include "nomnigraph/Graph/Algorithms.h"
namespace nom {
namespace repr {

NeuralNetOperator::~NeuralNetOperator() {}

const std::string NeuralNetOperator::getName() const {
  switch (getKind()) {
#include "nomnigraph/Generated/OpNames.h"
    case NNKind::While:
      return "While";
    case NNKind::NNPhi:
      return "Phi";
    case NNKind::GenericOperator:
      return dyn_cast<GenericOperator>(this)->getName();
    default:
      return "Unknown";
  }
}

NeuralNetData::~NeuralNetData() {}

const std::string NeuralNetData::getName() const {
  switch (getKind()) {
    case NNDataKind::Tensor: {
      return dyn_cast<Tensor>(this)->getName();
    }
    default:
      return "";
  }
}

namespace nn {

bool hasProducer(NNGraph::NodeRef n) {
  return n->getInEdges().size() != 0;
}

NNGraph::NodeRef getProducer(NNGraph::NodeRef n) {
  assert(
      is<NeuralNetData>(n) &&
      "getProducer only works with NeuralNetData types.");
  auto inEdges = n->getInEdges();
  assert(inEdges.size() > 0 && "Tensor does not have a producer.");
  assert(
      inEdges.size() == 1 &&
      "Malformed NNGraph, NeuralNetData has multiple producers.");
  return inEdges.front()->tail();
}

bool hasConsumer(NNGraph::NodeRef n) {
  return n->getOutEdges().size() != 0;
}

std::vector<NNGraph::NodeRef> getConsumers(NNGraph::NodeRef n) {
  assert(
      is<NeuralNetData>(n) &&
      "getProducer only works with NeuralNetData types.");
  std::vector<NNGraph::NodeRef> out;
  for (auto outEdge : n->getOutEdges()) {
    out.emplace_back(outEdge->head());
  }
  return out;
}

bool hasInputs(NNGraph::NodeRef n) {
  return n->getInEdges().size() != 0;
}

std::vector<NNGraph::NodeRef> getInputs(NNGraph::NodeRef n) {
  assert(
      is<NeuralNetOperator>(n) &&
      "getInputs only works with NeuralNetOperator types.");
  std::vector<NNGraph::NodeRef> out;
  for (auto inEdge : n->getInEdges()) {
    out.emplace_back(inEdge->tail());
  }
  return out;
}

std::vector<NNGraph::NodeRef> getOutputs(NNGraph::NodeRef n) {
  assert(
      is<NeuralNetOperator>(n) &&
      "getOutputs only works with NeuralNetOperator types.");
  std::vector<NNGraph::NodeRef> out;
  for (auto outEdge : n->getOutEdges()) {
    out.emplace_back(outEdge->head());
  }
  return out;
}

std::vector<repr::NNGraph::NodeRef> topologicalSort(
    const std::vector<repr::NNGraph::NodeRef>& instrs) {
  std::vector<repr::NNGraph::NodeRef> result;
  // build dependency graph
  std::unordered_map<
      repr::NNGraph::NodeRef,
      std::unordered_set<repr::NNGraph::NodeRef>>
      dependency_graph;
  for (auto& node : instrs) {
    dependency_graph[node] = std::unordered_set<repr::NNGraph::NodeRef>();
  }
  for (auto& node : instrs) {
    for (auto& output : nn::getOutputs(node)) {
      for (auto& consumer : nn::getConsumers(output)) {
        if (dependency_graph.count(consumer)) {
          dependency_graph[consumer].insert(node);
        }
      }
    }
  }
  // sort the instruction node by execution order
  std::queue<repr::NNGraph::NodeRef> q;
  std::vector<repr::NNGraph::NodeRef> node_to_remove;
  for (auto& kv : dependency_graph) {
    if (kv.second.empty()) {
      q.push(kv.first);
      node_to_remove.push_back(kv.first);
    }
  }
  for (auto& node : node_to_remove) {
    dependency_graph.erase(node);
  }

  while (!q.empty()) {
    auto& node = q.front();
    result.push_back(node);
    q.pop();
    for (auto& output : nn::getOutputs(node)) {
      for (auto& consumer : nn::getConsumers(output)) {
        if (dependency_graph.count(consumer)) {
          dependency_graph[consumer].erase(node);
          if (dependency_graph[consumer].empty()) {
            q.push(consumer);
            dependency_graph.erase(consumer);
          }
        }
      }
    }
  }
  assert(
      dependency_graph.empty() && "dependency graph not empty, loop detected");
  return result;
}

// Get all nodes tracked by CF graph
static std::unordered_set<repr::NNGraph::NodeRef> getTrackedNodes(
    repr::NNCFGraph& cf) {
  std::unordered_set<repr::NNGraph::NodeRef> cfTrackedNodes;
  for (const auto& bbNode : cf.getMutableNodes()) {
    auto bb = repr::nn::get<repr::BasicBlockType<repr::NNGraph>>(bbNode);
    for (const auto node : bb->getInstructions()) {
      cfTrackedNodes.insert(node);
    }
  }
  return cfTrackedNodes;
}

static size_t coalesceInsertedDataDependenciesHelper(repr::NNModule* m) {
  auto cfTrackedNodes = getTrackedNodes(m->controlFlow);

  for (auto& bbNode : m->controlFlow.getMutableNodes()) {
    auto bb = repr::nn::get<repr::BasicBlockType<repr::NNGraph>>(bbNode);
    // We mutate the instructions of the bb, so we copy here.
    // TODO make this an iterator and simply promote it on insertion.
    auto instrsCopy = bb->getInstructions();
    for (const auto instr : instrsCopy) {
      for (const auto input : repr::nn::getInputs(instr)) {
        if (!repr::nn::hasProducer(input)) {
          continue;
        }
        auto producer = repr::nn::getProducer(input);
        if (!cfTrackedNodes.count(producer)) {
          bb->insertInstructionBefore(producer, instr);
          cfTrackedNodes.insert(producer);
        }
      }
    }
  }

  return cfTrackedNodes.size();
}

// TODO: move this to more generic location.
// TODO: [algo] improve this algorithm, as it is horrendously inefficient.
void coalesceInsertedDataDependencies(repr::NNModule* m) {
  size_t oldSize = 0;
  size_t newSize = 0;
  do {
    oldSize = newSize;
    newSize = coalesceInsertedDataDependenciesHelper(m);
  } while (newSize != oldSize);

  // Now we track new nodes that have no relationship to the old CFGraph
  auto cfTrackedNodes = getTrackedNodes(m->controlFlow);
  std::unordered_set<repr::NNGraph::NodeRef> dfNodes;
  for (auto node : m->dataFlow.getMutableNodes()) {
    if (repr::nn::is<NeuralNetOperator>(node) && !cfTrackedNodes.count(node)) {
      dfNodes.insert(node);
    }
  }

  auto newBbNode = m->controlFlow.createNode(
      util::make_unique<repr::BasicBlockType<repr::NNGraph>>());
  auto sccs = algorithm::tarjans(&m->dataFlow);
  for (auto iter = sccs.rbegin(); iter != sccs.rend(); ++iter) {
    for (auto node : iter->getNodes()) {
      if (dfNodes.count(node)) {
        auto currentBasicBlock = newBbNode->mutableData()->get();
        currentBasicBlock->pushInstructionNode(node);
      }
    }
  }

  // Finally we reconcile any data dependency issues (if we can).
  for (auto& bbNode : m->controlFlow.getMutableNodes()) {
    auto bb = bbNode->mutableData()->get();
    auto& instructions = bb->getInstructions();
    if (instructions.size() <= 1) {
      continue;
    }
    vector<repr::NNGraph::NodeRef> ordered_instructions =
        topologicalSort(instructions);
    assert(
        ordered_instructions.size() == instructions.size() &&
        "number of instructions mismatch after topological sort");
    for (size_t idx = ordered_instructions.size() - 1; idx > 0; --idx) {
      bb->moveInstructionBefore(
          ordered_instructions[idx - 1], ordered_instructions[idx]);
    }
  }
}

} // namespace nn

} // namespace repr
} // namespace nom
