# Copyright (c) 2016-present, Facebook, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##############################################################################

from __future__ import unicode_literals
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import hypothesis.strategies as st
import unittest
import caffe2.python.hypothesis_test_util as hu
from caffe2.python import core
from hypothesis import given, settings, unlimited
import caffe2.python.ideep_test_util as mu


@st.composite
def _tensor_splits(draw, add_axis=False):
    """Generates (axis, split_info, tensor_splits) tuples."""
    tensor = draw(hu.tensor(min_value=4))  # Each dim has at least 4 elements.
    axis = draw(st.integers(0, len(tensor.shape) - 1))
    if add_axis:
        # Simple case: get individual slices along one axis, where each of them
        # is (N-1)-dimensional. The axis will be added back upon concatenation.
        return (
            axis,
            np.ones(tensor.shape[axis], dtype=np.int32),
            [
                np.array(tensor.take(i, axis=axis))
                for i in range(tensor.shape[axis])
            ]
        )
    else:
        # General case: pick some (possibly consecutive, even non-unique)
        # indices at which we will split the tensor, along the given axis.
        splits = sorted(draw(
            st.lists(elements=st.integers(0, tensor.shape[axis]), max_size=4)
        ) + [0, tensor.shape[axis]])
        # Not support empty tensor
        splits = list(set(splits))
        return (
            axis,
            np.array(np.diff(splits), dtype=np.int32),
            [
                tensor.take(range(splits[i], splits[i + 1]), axis=axis)
                for i in range(len(splits) - 1)
            ],
        )


class TestConcatSplitOps(hu.HypothesisTestCase):
    @given(tensor_splits=_tensor_splits(),
           **mu.gcs)
    @settings(deadline=None, timeout=unlimited)
    def test_concat(self, tensor_splits, gc, dc):
        axis, _, splits = tensor_splits

        op = core.CreateOperator(
            "Concat",
            ['X_{}'.format(i) for i in range(len(splits))],
            ['concat_result', 'split_info'],
            axis=axis
        )

        self.assertDeviceChecks(dc, op, splits, [0, 1])

    @given(tensor_splits=_tensor_splits(),
           split_as_arg=st.booleans(),
           **mu.gcs)
    @settings(deadline=None, timeout=unlimited)
    def test_split(self, tensor_splits, split_as_arg, gc, dc):
        axis, split_info, splits = tensor_splits

        split_as_arg = True

        if split_as_arg:
            input_names = ['input']
            input_tensors = [np.concatenate(splits, axis=axis)]
            kwargs = dict(axis=axis, split=split_info)
        else:
            input_names = ['input', 'split']
            input_tensors = [np.concatenate(splits, axis=axis), split_info]
            kwargs = dict(axis=axis)

        op = core.CreateOperator(
            "Split",
            input_names,
            ['X_{}'.format(i) for i in range(len(split_info))],
            **kwargs
        )

        def split_ref(input, split=split_info):
            s = np.cumsum([0] + list(split))
            return [
                np.array(input.take(np.arange(s[i], s[i + 1]), axis=axis))
                for i in range(len(split))
            ]
        outputs_with_grad = range(len(split_info))
        self.assertDeviceChecks(dc, op, input_tensors, outputs_with_grad)

if __name__ == "__main__":
    unittest.main()
