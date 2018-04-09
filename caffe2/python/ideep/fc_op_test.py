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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import unittest
import hypothesis.strategies as st
from hypothesis import given, settings, unlimited
import numpy as np
from caffe2.python import core, workspace
import caffe2.python.hypothesis_test_util as hu
import caffe2.python.ideep_test_util as mu


class FcTest(hu.HypothesisTestCase):
    @given(n=st.integers(1, 5), m=st.integers(1, 5),
           k=st.integers(1, 5), **mu.gcs)
    @settings(deadline=None, timeout=unlimited)
    def test_fc(self,n, m, k, gc, dc):
        X = np.random.rand(m, k).astype(np.float32) - 0.5
        W = np.random.rand(n, k).astype(np.float32) - 0.5
        b = np.random.rand(n).astype(np.float32) - 0.5

        op = core.CreateOperator(
            'FC',
            ['X', 'W', 'b'],
            ["Y"]
            )

        self.assertDeviceChecks(dc, op, [X, W, b], [0])


if __name__ == "__main__":
    import unittest
    unittest.main()
