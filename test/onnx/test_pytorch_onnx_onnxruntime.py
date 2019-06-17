from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals

import unittest
import sys
import onnxruntime  # noqa
import torch
import numpy as np
import io

class TestONNXRuntime(unittest.TestCase):

    def test_onnxruntime_installed(self):
        self.assertTrue('onnxruntime' in sys.modules)

    # There is a difference between PyTorch and ONNX/numpy w.r.t index_select by a scaler index.
    # With an input tensor of rank r and a scaler index, PyTorch index_select returns a tensor of rank = r.
    # However, corresponding op in ONNX(Gather) and numpy would return a tensor of rank = r - 1.
    # To maintain a parity between ONNX and PyTorch, scaler index was converted to a 1D tensor
    # before applying an ONNX gather op during ONNX export. 
    # This test is to confirm that equivalence between PyTorch and ONNX is maintained in the above case.
    def test_index_select_scaler_index(self):
        class IndexSelectScalerIndexModel(torch.nn.Module):
            def forward(self, x):
                return torch.index_select(x, 1, torch.tensor(2))

        x = torch.randn(3, 4)
        model = IndexSelectScalerIndexModel()
        pt_out = model(x)

        f = io.BytesIO()
        torch.onnx._export(model, (x,), f)
        ort_sess = onnxruntime.InferenceSession(f.getvalue())
        ort_outs = ort_sess.run(None, {ort_sess.get_inputs()[0].name: x.numpy()})

        numpy_out = x.numpy()[:, [2]]
        np.allclose(pt_out.numpy(), numpy_out)
        np.allclose(ort_outs[0], numpy_out)

if __name__ == '__main__':
    unittest.main()
