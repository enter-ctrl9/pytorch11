#include "caffe2/onnx/onnxifi_manager.h"

#include <mutex>

#include "caffe2/core/logging.h"

namespace caffe2 {
namespace onnx {

onnxifi_library* OnnxifiManager::AddOnnxifiLibrary(
    const std::string& name) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = onnxifi_interfaces_.find(name);
  if (it != onnxifi_interfaces_.end()) {
    LOG(INFO) << "Onnx interface " << name <<  " already exists";
    return &it->second;
  }

  onnxifi_library lib;
  auto ret = onnxifi_load(
      ONNXIFI_LOADER_FLAG_VERSION_1_0, nullptr, nullptr, &lib);
  if (!ret) {
    CAFFE_THROW("Cannot load onnxifi lib ", name);
  }
  return &onnxifi_interfaces_.emplace(name, lib).first->second;
}

void OnnxifiManager::RemoveOnnxifiLibrary(const std::string& name) {
  std::lock_guard<std::mutex> lock(m_);
  auto it = onnxifi_interfaces_.find(name);
  if (it == onnxifi_interfaces_.end()) {
    LOG(WARNING) << "Onnxifi lib " << name << "has not been registered";
  }
  onnxifi_unload(&it->second);
  onnxifi_interfaces_.erase(it);
}

void OnnxifiManager::ClearAll() {
  std::lock_guard<std::mutex> lock(m_);
  for (auto& kv : onnxifi_interfaces_) {
    onnxifi_unload(&kv.second);
  }
  onnxifi_interfaces_.clear();
}

OnnxifiManager* OnnxifiManager::get_onnxifi_manager() {
  static OnnxifiManager core{};
  return &core;
}

} // namespace onnx
} // namespace caffe2
