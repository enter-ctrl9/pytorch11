from functools import wraps

import torch

from common_utils import TestCase, run_tests, load_tests
from common_device_type import instantiate_device_type_tests

# load_tests from common_utils is used to automatically filter tests for
# sharding on sandcastle. This line silences flake warnings
load_tests = load_tests

# Not thread-safe decorator that runs the decorated test once with
# the default dtype being torch.float and again with the default dtype
# being torch.double.
def multiple_default_dtypes(fn):
    @wraps(fn)
    def wrapped_fn(*args, **kwargs):
        cur_dtype = torch.get_default_dtype()
        torch.set_default_dtype(torch.float)
        fn(*args, **kwargs)
        torch.set_default_dtype(torch.double)
        fn(*args, **kwargs)
        torch.set_default_dtype(cur_dtype)

    return wrapped_fn


class TestTypePromotion(TestCase):

    # In-place operations don't promote.
    # `int+float -> float` but `int.add_(float)` is rejected as an error.
    # Promoting inplace would require re-allocating and copying the memory of the
    # tensor data, since element size could change.
    @multiple_default_dtypes
    def test_inplace(self, device):
        int_tensor = torch.ones([4, 4, 4], dtype=torch.int32, device=device)

        self.assertRaisesRegex(RuntimeError, "can't be cast to", lambda: int_tensor.add_(1.5))

        expected = torch.ones([4, 4, 4], dtype=torch.int32, device=device)

        long_tensor = torch.ones([4, 4, 4], dtype=torch.int64, device=device)
        int_tensor.add_(long_tensor)
        int_tensor.add_(1)
        three = expected + 2
        self.assertEqual(int_tensor, three)
        self.assertEqual(int_tensor.dtype, torch.int32)

        bool_tensor = torch.tensor([1, 1, 1], dtype=torch.bool, device=device)
        uint8_tensor = torch.tensor([1, 1, 1], dtype=torch.uint8, device=device)
        # We treat bool as a separate category, which means uint8 cannot cast to bool.
        self.assertRaisesRegex(RuntimeError, "can't be cast to", lambda: bool_tensor.add_(uint8_tensor))

        # We allow demotion from signed to unsigned, unlike numpy, because:
        # * We don't want the performance penalty of inspecting scalar values.
        # * We don't want 'signed' to be considered a distinct 'category'
        # in promotion rules.
        # We don't want signed to be a separate category because if it was,
        # uint16_tensor + 5 would result in a long_tensor, which is not what we want.
        int16_tensor = torch.tensor([1, 1, 1], dtype=torch.int16, device=device)
        uint8_tensor *= int16_tensor

    @multiple_default_dtypes
    def test_unsinged(self, device):
        dont_promote = torch.ones(3, dtype=torch.uint8, device=device) + 5
        self.assertEqual(dont_promote.dtype, torch.uint8)

    # some basic examples

    @multiple_default_dtypes
    def test_int_promotion(self, device):
        a = torch.ones([4, 4, 4], dtype=torch.int32, device=device)
        b = torch.ones([4, 4, 4], dtype=torch.int64, device=device)
        c = a + b
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.int64)

    @multiple_default_dtypes
    def test_float_promotion(self, device):
        a = torch.ones([4, 4, 4], dtype=torch.float, device=device)
        b = torch.ones([4, 4, 4], dtype=torch.double, device=device)
        c = a + b
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.double)
        c = b + a
        self.assertEqual(c, b + b)
        self.assertEqual(c.dtype, torch.double)

    @multiple_default_dtypes
    def test_add_wrapped(self, device):
        a = torch.ones([4, 4, 4], dtype=torch.int, device=device)
        b = 1
        c = a + b
        self.assertEqual(c, a + a)
        self.assertEqual(c.dtype, torch.int)

    @multiple_default_dtypes
    def test_int_to_float(self, device):
        a = torch.ones([4, 4, 4], dtype=torch.int32, device=device)
        b = torch.ones([4, 4, 4], dtype=torch.float, device=device)
        c = a + b
        self.assertEqual(c.dtype, torch.float32)

    # some examples from:
    # https://github.com/pytorch/pytorch/issues/9515

    @multiple_default_dtypes
    def test_from_issue(self, device):
        a = torch.rand(3, dtype=torch.float32, device=device)
        u = torch.tensor([0, 0, 1], dtype=torch.uint8, device=device)
        self.assertEqual((a * 5).dtype, torch.float32)
        self.assertEqual((u + 1).dtype, torch.uint8)
        self.assertEqual((u + 1000).dtype, torch.uint8)  # integer overflow

        # not a "wrapped number"
        other = torch.tensor(5.5, dtype=torch.double, device=device)

        self.assertEqual((u + 5.5).dtype, torch.get_default_dtype())
        self.assertEqual((u + other).dtype, torch.double)
        # adding a 0-dim tensor to a float doesn't promote to double unless first
        # type was integral.
        self.assertEqual((a + other).dtype, torch.float32)

    @multiple_default_dtypes
    def test_half(self, device):
        half = torch.tensor(5.5, dtype=torch.float16, device=device)
        if(self.device_type == 'cpu'):
            self.assertRaisesRegex(RuntimeError, "not implemented for 'Half'",
                                   lambda: half + 2.2)
        else:
            self.assertEqual((half + 2.2).dtype, torch.float16)
            self.assertEqual((half + 100000).dtype, torch.float16)  # inf
            default_tensor = torch.tensor(100000.0, device=device)
            self.assertEqual((half + default_tensor).dtype, torch.get_default_dtype())

    @multiple_default_dtypes
    def test_alternate_result(self, device):
        f = torch.tensor([1, 1, 1, 1], dtype=torch.float, device=device)
        o = torch.tensor([0, 0, 0, 0], dtype=torch.long, device=device)
        self.assertRaisesRegex(RuntimeError,
                               "can't be cast to",
                               lambda: torch.add(f, f, out=o))
        d = torch.tensor([1, 1, 1, 1], dtype=torch.double, device=device)
        torch.add(f, f, out=d)
        self.assertEqual(d.dtype, torch.double)
        self.assertEqual(f + f, d)

    @multiple_default_dtypes
    def test_mixed_type_backward(self, device):
        f = torch.ones([3, 3], dtype=torch.float, requires_grad=True, device=device)
        ten = torch.tensor([10.], dtype=torch.double, device=device)
        tens = f * ten
        s = (tens + 2).sum()
        s.backward()
        self.assertEqual(f.grad, tens)

        # If we don't convert the returned grad_input to the actual input type
        # we get an error like:
        # RuntimeError: Function SubBackward0 returned an invalid gradient at index 0 - expected type \
        # torch.FloatTensor but got torch.DoubleTensor
        f_dtypes = [torch.float, torch.double]
        if self.device_type == 'cuda':
            f_dtypes = f_dtypes + [torch.half]
        i_dtypes = [torch.int, torch.long]
        for f in ['add', 'sub', 'rsub', 'mul', 'div']:
            for dtype1 in f_dtypes:
                for dtype2 in (f_dtypes + i_dtypes):
                    x = torch.ones(10, requires_grad=True, dtype=dtype1, device=device)
                    y = torch.ones(10, dtype=dtype2, device=device)

                    func = getattr(torch, f)
                    func(x, y).sum().backward()

    # verifies that a.add(b) is the same as a.to(b.dtype).add(b) in cases
    # where that should hold.
    @multiple_default_dtypes
    def test_many_promotions(self, device):
        from_to = {
            torch.float16: torch.float32,
            torch.half: torch.float16,
            torch.int: torch.long,
            torch.uint8: torch.long,
            torch.uint8: torch.float,
            torch.int: torch.float,
            torch.int: torch.double,
            torch.int16: torch.long,
            torch.float16: torch.double,
            torch.bool: torch.long,
            torch.bool: torch.float
        }

        for k, v in from_to.items():
            a = torch.rand([3, 3], device=device).to(k)  # no _th_uniform for half on cpu.
            b = torch.rand([3, 3], device=device).to(v)
            c = a.add(b)
            d = a.to(v).add(b)
            self.assertEqual(c.dtype, d.dtype, message='from {} to {}'.format(k, v))
            self.assertEqual(c, d, message='from {} to {}'.format(k, v))

    @multiple_default_dtypes
    def test_non_promoting_ops(self, device):
        x = torch.ones(4, dtype=torch.double, device=device)
        self.assertRaises(RuntimeError,
                          lambda: torch.neg(torch.ones(4, dtype=torch.float, device=device), out=x))
        self.assertRaises(RuntimeError,
                          lambda: torch.lerp(x, torch.ones(4, dtype=torch.float, device=device), 1))

    @multiple_default_dtypes
    def test_alpha_mismatch(self, device):
        x = torch.ones(4, dtype=torch.int, device=device)
        err = 'alpha must not be'
        self.assertRaisesRegex(RuntimeError, err,
                               lambda: torch.add(x, x, alpha=1.1))
        x = x.to(torch.bool)
        self.assertRaisesRegex(RuntimeError, err,
                               lambda: torch.add(x, x, alpha=1.1))
        self.assertEqual(x + x, torch.add(x, x, alpha=True))

    @multiple_default_dtypes
    def test_booleans(self, device):
        onedim = torch.tensor([True], device=device)

        self.assertEqual(onedim + onedim, onedim)
        self.assertEqual(onedim + True, onedim)
        self.assertEqual(torch.add(True, True), True)
        self.assertEqual(torch.add(False, False), False)
        self.assertEqual(torch.add(False, True), True)

        self.assertRaisesRegex(RuntimeError, "Boolean alpha only supported",
                               lambda: torch.add(1, 1, alpha=True))
        self.assertEqual(torch.add(torch.tensor(True, device=device), torch.tensor(True, device=device), True), torch.tensor(True, device=device))

    @multiple_default_dtypes
    def test_create_bool_tensors(self, device):
        expected = torch.tensor([0], dtype=torch.int64, device=device)
        self.assertEqual(torch.arange(False, True, device=device), expected)
        self.assertEqual(torch.arange(True, device=device), expected)
        expected = torch.tensor([0, 0.5], dtype=torch.get_default_dtype(), device=device)
        self.assertEqual(torch.arange(False, True, 0.5, device=device), expected)
        expected = torch.ones(0, dtype=torch.int64, device=device)
        self.assertEqual(torch.arange(False, False, device=device), expected)

        self.assertEqual(torch.linspace(False, True, device=device), torch.linspace(0, 1, device=device))
        self.assertEqual(torch.logspace(False, True, device=device), torch.logspace(0, 1, device=device))

        # this seems like odd behavior but ints also create float tensors, numpy doesn't have this function.
        self.assertEqual(torch.scalar_tensor(False, device=device), torch.tensor(0., device=device))

    @multiple_default_dtypes
    def test_result_type(self, device):
        self.assertEqual(torch.result_type(torch.tensor(1, dtype=torch.int, device=device), 1), torch.int)
        self.assertEqual(torch.result_type(1, torch.tensor(1, dtype=torch.int, device=device)), torch.int)
        self.assertEqual(torch.result_type(1, 1.), torch.get_default_dtype())
        self.assertEqual(torch.result_type(torch.tensor(1, device=device), 1.), torch.get_default_dtype())
        self.assertEqual(torch.result_type(torch.tensor(1, dtype=torch.long, device=device), torch.tensor([1, 1], dtype=torch.int, device=device)), torch.int)
        self.assertEqual(torch.result_type(torch.tensor([1., 1.], dtype=torch.float, device=device), 1.), torch.float)
        self.assertEqual(torch.result_type(torch.tensor(1., dtype=torch.float, device=device), torch.tensor(1, dtype=torch.double, device=device)), torch.double)

    @multiple_default_dtypes
    def test_can_cast(self, device):
        self.assertTrue(torch.can_cast(torch.double, torch.float))
        self.assertFalse(torch.can_cast(torch.float, torch.int))

    @multiple_default_dtypes
    def test_comparison_ops_with_type_promotion(self, device):
        value_for_type = {
            torch.uint8: (1 << 5),
            torch.int8: (1 << 5),
            torch.int16: (1 << 10),
            torch.int32: (1 << 20),
            torch.int64: (1 << 35),
            torch.float16: (1 << 10),
            torch.float32: (1 << 20),
            torch.float64: (1 << 35)
        }
        comparison_ops = [
            dict(
                name="lt",
                out_op=lambda x, y, d: torch.lt(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.lt(x, y),
                compare_op=lambda x, y: x < y,
            ),
            dict(
                name="le",
                out_op=lambda x, y, d: torch.le(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.le(x, y),
                compare_op=lambda x, y: x <= y,
            ),
            dict(
                name="gt",
                out_op=lambda x, y, d: torch.gt(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.gt(x, y),
                compare_op=lambda x, y: x > y,
            ),
            dict(
                name="ge",
                out_op=lambda x, y, d: torch.ge(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.ge(x, y),
                compare_op=lambda x, y: x >= y,
            ),
            dict(
                name="eq",
                out_op=lambda x, y, d: torch.eq(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.eq(x, y),
                compare_op=lambda x, y: x == y,
            ),
            dict(
                name="ne",
                out_op=lambda x, y, d: torch.ne(x, y, out=torch.empty(1, dtype=torch.bool, device=d)),
                ret_op=lambda x, y: torch.ne(x, y),
                compare_op=lambda x, y: x != y,
            ),
        ]
        for op in comparison_ops:
            for dt1 in torch.testing.get_all_math_dtypes(device):
                for dt2 in torch.testing.get_all_math_dtypes(device):
                    val1 = value_for_type[dt1]
                    val2 = value_for_type[dt2]
                    t1 = torch.tensor([val1], dtype=dt1, device=device)
                    t2 = torch.tensor([val2], dtype=dt2, device=device)
                    expected = torch.tensor([op["compare_op"](val1, val2)], dtype=torch.bool)

                    out_res = op["out_op"](t1, t2, device)
                    self.assertEqual(out_res, expected)
                    self.assertTrue(out_res.dtype == torch.bool)
                    self.assertTrue(t1.dtype == dt1)
                    self.assertTrue(t2.dtype == dt2)

                    out_res = op["ret_op"](t1, t2)
                    self.assertEqual(out_res, expected)
                    self.assertTrue(out_res.dtype == torch.bool)
                    self.assertTrue(t1.dtype == dt1)
                    self.assertTrue(t2.dtype == dt2)

                    # test that comparing a zero dim tensor with another zero dim tensor has type promotion behavior
                    t1 = torch.tensor(val1, dtype=dt1, device=device)
                    t2 = torch.tensor(val2, dtype=dt2, device=device)
                    expected = torch.tensor(op["compare_op"](val1, val2), dtype=torch.bool)

                    out_res = op["out_op"](t1, t2, device)
                    self.assertEqual(out_res, expected)
                    self.assertTrue(out_res.dtype == torch.bool)
                    self.assertTrue(t1.dtype == dt1)
                    self.assertTrue(t2.dtype == dt2)

                    out_res = op["ret_op"](t1, t2)
                    self.assertEqual(out_res, expected)
                    self.assertTrue(out_res.dtype == torch.bool)
                    self.assertTrue(t1.dtype == dt1)
                    self.assertTrue(t2.dtype == dt2)

    @multiple_default_dtypes
    def test_lt_with_type_promotion(self, device):
        for dt in torch.testing.get_all_math_dtypes(device):
            x = torch.tensor([0], dtype=dt, device=device)
            expected = torch.tensor([True], dtype=torch.bool, device=device)

            actual = x < 0.5
            self.assertTrue(actual, expected)
            self.assertTrue(actual.dtype == torch.bool)

            actual = x < torch.tensor(0.5)
            self.assertTrue(actual, expected)
            self.assertTrue(actual.dtype == torch.bool)

            x = torch.tensor(0, dtype=dt, device=device)
            expected = torch.tensor(True, dtype=torch.bool, device=device)
            actual = x < 0.5
            self.assertTrue(actual, expected)
            self.assertTrue(actual.dtype == torch.bool)

            actual = x < torch.tensor(0.5)
            self.assertTrue(actual, expected)
            self.assertTrue(actual.dtype == torch.bool)

    @multiple_default_dtypes
    def test_promote_types(self, device):
        self.assertEqual(torch.promote_types(torch.float, torch.int), torch.float)
        self.assertEqual(torch.promote_types(torch.float, torch.double), torch.double)
        self.assertEqual(torch.promote_types(torch.int, torch.uint8), torch.int)

    @multiple_default_dtypes
    def test_promote_self(self, device):
        for dtype in torch.testing.get_all_dtypes():
            self.assertEqual(torch.promote_types(dtype, dtype), dtype)

    @multiple_default_dtypes
    def test_indexing(self, device):
        a = torch.ones(5, 2, dtype=torch.double, device=device)
        b = torch.zeros(5, dtype=torch.int, device=device)

        # lambda cannot contain assignment
        def f():
            a[:, [1]] = b.unsqueeze(-1)
        # https://github.com/pytorch/pytorch/issues/28010
        self.assertRaisesRegex(RuntimeError, 'expected dtype',
                               lambda: f())

        # https://github.com/pytorch/pytorch/issues/27824
        tmp = torch.ones(9, 9, dtype=torch.float, device=device)
        mask = torch.ones(10, 10, dtype=torch.uint8, device=device)
        result = tmp + mask[1:, 1:]
        expected = torch.full([9, 9], 2., dtype=torch.float, device=device).fill_(2.)
        self.assertEqual(result, expected)

    @multiple_default_dtypes
    def test_transpose(self, device):
        # https://github.com/pytorch/pytorch/issues/28502
        a = torch.tensor([[True, True], [False, True]], device=device)
        self.assertEqual(a.t() == 0, a.t() == False)  # noqa: E712


instantiate_device_type_tests(TestTypePromotion, globals())

if __name__ == '__main__':
    run_tests()
