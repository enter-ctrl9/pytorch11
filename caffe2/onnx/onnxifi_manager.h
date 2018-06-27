#pragma once

#include "onnx/onnxifi_loader.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace caffe2 {
namespace onnx {
class OnnxifiManager {
  public:
    OnnxifiManager() {}
    onnxifi_library* AddOnnxifiLibrary(const std::string& name);
    void RemoveOnnxifiLibrary(const std::string& name);
    void ClearAll();

    static OnnxifiManager* get_onnxifi_manager();
  private:
    mutable std::mutex m_;
    std::unordered_map<std::string, onnxifi_library> onnxifi_interfaces_;
};

} // namespace onnx
} // namespace caffe2
