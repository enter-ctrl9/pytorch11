import torch
import unittest

from common_utils import TestCase, run_tests, load_tests

# load_tests from common_utils is used to automatically filter tests for
# sharding on sandcastle. This line silences flake warnings
load_tests = load_tests

class TestTypePromotion(TestCase):

    def setUp(self):
        super(TestTypePromotion, self).setUp()
        torch.set_default_dtype(torch.float32)
        self.device = 'cpu'

    # In-place operations don't promote.
    # `int+float -> float` but `int.add_(float)` is rejected as an error.
    # Promoting inplace would require re-allocating and copying the memory of the
    # tensor data, since element size could change.
    def test_inplace(self):
        int_tensor = torch.ones([4, 4, 4], dtype=torch.int32, device=self.device)

        self.assertRaisesRegex(RuntimeError, "can't be cast to", lambda: int_tensor.add_(1.5))

        expected = torch.ones([4, 4, 4], device=self.device)
        expected = expected.to(torch.int32)

        long_tensor = torch.ones([4, 4, 4], dtype=torch.int64, device=self.device)
        int_tensor.add_(long_tensor)
        int_tensor.add_(1)
        three = expected + 2
        self.assertEqual(int_tensor, three)
        self.assertEqual(int_tensor.dtype, torch.int32)

        uint8_tensor = torch.tensor([1, 1, 1], dtype=torch.uint8)
        half_tensor = torch.tensor([1, 1, 1], dtype=torch.half)
        # uint8 is unsigned, half is signed.
        self.assertRaisesRegex(RuntimeError, "can't be cast to", lambda: uint8_tensor.add_(half_tensor))

    # some basic examples

    def test_int_promotion(self):
        a = torch.ones([4, 4, 4], dtype=torch.int32, device=self.device)
        b = torch.ones([4, 4, 4], dtype=torch.int64, device=self.device)
        c = a + b
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.int64)

    def test_float_promotion(self):
        a = torch.ones([4, 4, 4], dtype=torch.float, device=self.device)
        b = torch.ones([4, 4, 4], dtype=torch.double, device=self.device)
        c = a + b
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.double)
        c = b + a
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.double)

    def test_add_wrapped(self):
        a = torch.ones([4, 4, 4], dtype=torch.int, device=self.device)
        b = 1
        c = a + b
        self.assertEqual(c, a + a)
        self.assertEqual(c.dtype, torch.int)

    def test_int_to_float(self):
        a = torch.ones([4, 4, 4], dtype=torch.int32, device=self.device)
        b = torch.ones([4, 4, 4], dtype=torch.float, device=self.device)
        c = a + b
        self.assertEqual(c.dtype, torch.float32)

    # some examples from:
    # https://github.com/pytorch/pytorch/issues/9515

    def test_from_issue(self):
        a = torch.rand(3, dtype=torch.float32, device=self.device)
        u = torch.tensor([0, 0, 1], dtype=torch.uint8, device=self.device)
        self.assertEqual((a * 5).dtype, torch.float32)
        self.assertEqual((u + 1).dtype, torch.uint8)
        self.assertEqual((u + 1000).dtype, torch.uint8)  # integer overflow

        # not a "wrapped number"
        other = torch.tensor(5.5, dtype=torch.double, device=self.device)

        self.assertEqual((u + 5.5).dtype, torch.get_default_dtype())
        self.assertEqual((u + other).dtype, torch.double)
        # adding a 0-dim tensor to a float doesn't promote to double unless first
        # type was integral.
        self.assertEqual((a + other).dtype, torch.float32)

    def test_half(self):
        if(self.device == 'cpu'):
            raise unittest.SkipTest('add_cpu not implemented for Half')
        half = torch.tensor(5.5, dtype=torch.float16, device=self.device)
        self.assertEqual((half + 2.2).dtype, torch.float16)
        self.assertEqual((half + 100000).dtype, torch.float16)  # inf
        default_tensor = torch.tensor(100000.0, device=self.device)
        self.assertEqual((half + default_tensor).dtype, torch.get_default_dtype())

    def test_alternate_result(self):
        f = torch.tensor([1, 1, 1, 1], dtype=torch.float, device=self.device)
        o = torch.tensor([0, 0, 0, 0], dtype=torch.long, device=self.device)
        self.assertRaisesRegex(RuntimeError,
                               "can't be cast to",
                               lambda: torch.add(f, f, out=o))
        d = torch.tensor([1, 1, 1, 1], dtype=torch.double, device=self.device)
        torch.add(f, f, out=d)
        self.assertEqual(d.dtype, torch.double)
        self.assertEqual(f + f, d)

    def test_mixed_type_backward(self):
        f = torch.ones([3, 3], dtype=torch.float, requires_grad=True, device=self.device)
        ten = torch.tensor([10], dtype=torch.double, device=self.device)
        tens = f * ten
        s = (tens + 2).sum()
        s.backward()
        self.assertEqual(f.grad, tens)

        # If we don't convert the returned grad_input to the actual input type
        # we get an error like:
        # RuntimeError: Function SubBackward0 returned an invalid gradient at index 0 - expected type \
        # torch.FloatTensor but got torch.DoubleTensor
        f_dtypes = [torch.float, torch.double]
        f_dtypes = f_dtypes if self.device != 'cuda' else f_dtypes + [torch.half]
        i_dtypes = [torch.int, torch.long]
        for f in ['add', 'sub', 'rsub', 'mul', 'div']:
            for dtype1 in f_dtypes:
                for dtype2 in (f_dtypes + i_dtypes):
                    x = torch.ones(10, requires_grad=True, dtype=dtype1, device=self.device)
                    y = torch.ones(10, dtype=dtype2, device=self.device)

                    func = getattr(torch, f)
                    func(x, y).sum().backward()

    # verifies that a.add(b) is the same as a.to(b.dtype).add(b) in cases
    # where that should hold.
    def test_many_promotions(self):
        from_to = {
            torch.float16: torch.float32,
            torch.half: torch.float16,
            torch.int: torch.long,
            torch.uint8: torch.long,
            torch.uint8: torch.float,
            torch.int: torch.float,
            torch.int: torch.double,
            torch.int16: torch.long,
            torch.float16: torch.double
        }

        for k, v in from_to.items():
            a = torch.rand([3, 3], device=self.device).to(k)  # not _th_uniform for half on cpu.
            b = torch.rand([3, 3], device=self.device).to(v)
            c = a.add(b)
            d = a.to(v).add(b)
            self.assertEqual(c.dtype, d.dtype, message='from {} to {}'.format(k, v))
            self.assertEqual(c, d, message='from {} to {}'.format(k, v))

    def test_non_promoting_ops(self):
        x = torch.ones(4, dtype=torch.double)
        err = 'expected dtype .ouble but got dtype .loat'
        self.assertRaisesRegex(RuntimeError, err,
                               lambda: torch.neg(torch.ones(4, dtype=torch.float), out=x))
        self.assertRaisesRegex(RuntimeError, err,
                               lambda: torch.lerp(x, torch.ones(4, dtype=torch.float), 1))

    def test_alpha_mismatch(self):
        x = torch.ones(4, dtype=torch.int)
        err = 'alpha must not be'
        self.assertRaisesRegex(RuntimeError, err,
            lambda: torch.add(x, x, alpha=1.1))


@unittest.skipIf(not torch.cuda.is_available(), "no cuda")
class TestTypePromotionCuda(TestTypePromotion):
    def setUp(self):
        super(TestTypePromotionCuda, self).setUp()
        self.device = 'cuda'

# ensure type promotion logic properly handles an alternate default dtype.
class TestTypePromotionDefaultDouble(TestTypePromotion):
    def setUp(self):
        super(TestTypePromotionDefaultDouble, self).setUp()
        torch.set_default_dtype(torch.double)


if __name__ == '__main__':
    run_tests()
