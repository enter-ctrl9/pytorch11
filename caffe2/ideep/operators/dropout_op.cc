/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <caffe2/ideep/ideep_utils.h>

namespace caffe2 {

class IDEEPDropoutOp final : public IDEEPOperator {
 public:
  USE_IDEEP_DEF_ALIASES();
  USE_IDEEP_OPERATOR_FUNCTIONS();

  IDEEPDropoutOp(const OperatorDef& operator_def, Workspace* ws)
      : IDEEPOperator(operator_def, ws),
        ratio_(OperatorBase::GetSingleArgument<float>("ratio", 0.5)),
        is_test_(
            OperatorBase::GetSingleArgument<int>(OpSchema::Arg_IsTest, 0)) {
    CAFFE_ENFORCE_GE(ratio_, 0);
    CAFFE_ENFORCE_LT(ratio_, 1);
  }
  virtual ~IDEEPDropoutOp() {}

  bool RunOnDevice() override {
    const auto& X = Input(INPUT);
    auto* Y = Output(OUTPUT);

    if (is_test_) {
      if (Y != &X) {
        ideep::direct_copy::compute(X, *Y);
      }
      return true;
    }

    auto* mask = Output(MASK);
    ideep::dropout_forward::compute(X, ratio_, *Y, *mask);

    return true;
  }

 private:
  float ratio_;
  bool is_test_;

  INPUT_TAGS(INPUT);
  OUTPUT_TAGS(OUTPUT, MASK);
};

REGISTER_IDEEP_OPERATOR(Dropout, IDEEPDropoutOp);

} // namespace caffe2
