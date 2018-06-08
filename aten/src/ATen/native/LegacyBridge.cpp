#include <ATen/ATen.h>
#include <ATen/SparseTensorRef.h>

namespace at { namespace native {

namespace {
  // NB: Even though some of the functions we have ported are CUDA
  // friendly, flipping the switch between native and non-native is
  // an all or nothing affair, because the internal representation
  // is different
  static bool _has_native(const Tensor& self) {
    return self.is_sparse() && !self.is_cuda();
  }

  static bool _type_has_native(const Type& dtype) {
    return dtype.is_sparse() && !dtype.is_cuda();
  }
}

// These native operations are not "really" native; they're actually just bridge
// functions that decide whether or not to call native sparse functions, or
// TH functions.  This file should be temporary; when all of TH gets ported, we
// can just use the native mechanism straight.

// TODO: Maybe the foo_ variants should call th_foo_

Tensor norm(const Tensor & self, Scalar p) {
  if (_has_native(self)) {
    return native_norm(self, p);
  } else {
    return th_norm(self, p);
  }
}

Tensor clone(const Tensor& self) {
  if (_has_native(self)) {
    return native_clone(self);
  } else {
    return th_clone(self);
  }
}

Tensor& resize_as_(Tensor& self, const Tensor& the_template) {
  if (_has_native(self)) {
    return native_resize_as_(self, the_template);
  } else {
    return th_resize_as_(self, the_template);
  }
}

Tensor& pow_out(Tensor& result, const Tensor& self, Scalar exponent) {
  if (_has_native(self)) {
    return native_pow_out(result, self, exponent);
  } else {
    return th_pow_out(result, self, exponent);
  }
}

Tensor pow(const Tensor& self, Scalar exponent) {
  Tensor r = self.type().tensor();
  native::pow_out(r, self, exponent);
  return r;
}

Tensor& zero_(Tensor& self) {
  if (_has_native(self)) {
    return native_zero_(self);
  } else {
    return th_zero_(self);
  }
}

Tensor& add_out(Tensor& result, const Tensor& self, const Tensor& other, Scalar alpha) {
  if (_has_native(self)) {
    return native_add_out(result, self, other, alpha);
  } else {
    return th_add_out(result, self, other, alpha);
  }
}

Tensor add(const Tensor& self, const Tensor& other, Scalar alpha) {
  Tensor r = self.type().tensor();
  native::add_out(r, self, other, alpha);
  return r;
}

Tensor& add_(Tensor& self, const Tensor& other, Scalar alpha) {
  return native::add_out(self, self, other, alpha);
}

Tensor& add_out(Tensor& result, const Tensor& self, SparseTensorRef other, Scalar alpha) {
  if (_has_native(self)) {
    return native_add_out(result, self, other, alpha);
  } else {
    return th_add_out(result, self, other, alpha);
  }
}

Tensor add(const Tensor& self, SparseTensorRef other, Scalar alpha) {
  Tensor r = self.type().tensor();
  native::add_out(r, self, other, alpha);
  return r;
}

Tensor& add_(Tensor& self, SparseTensorRef other, Scalar alpha) {
  return native::add_out(self, self, other, alpha);
}


Tensor& sub_out(Tensor& result, const Tensor& self, const Tensor& other, Scalar alpha) {
  if (_has_native(self)) {
    return native_sub_out(result, self, other, alpha);
  } else {
    return th_sub_out(result, self, other, alpha);
  }
}

Tensor sub(const Tensor& self, const Tensor& other, Scalar alpha) {
  Tensor r = self.type().tensor();
  return native::sub_out(r, self, other, alpha);
  return r;
}

Tensor& sub_(Tensor& self, const Tensor& other, Scalar alpha) {
  return native::sub_out(self, self, other, alpha);
}


Tensor& mul_out(Tensor& result, const Tensor& self, const Tensor& other) {
  if (_has_native(self)) {
    return native_mul_out(result, self, other);
  } else {
    return th_mul_out(result, self, other);
  }
}

Tensor mul(const Tensor& self, const Tensor& other) {
  Tensor r = self.type().tensor();
  return native::mul_out(r, self, other);
  return r;
}

Tensor& mul_(Tensor& self, const Tensor& other) {
  return native::mul_out(self, self, other);
}

Tensor& mul_out(Tensor& result, const Tensor& self, Scalar other) {
  if (_has_native(self)) {
    return native_mul_out(result, self, other);
  } else {
    return th_mul_out(result, self, other);
  }
}

Tensor mul(const Tensor& self, Scalar other) {
  Tensor r = self.type().tensor();
  return native::mul_out(r, self, other);
  return r;
}

Tensor& mul_(Tensor& self, Scalar other) {
  return native::mul_out(self, self, other);
}


Tensor& div_out(Tensor& result, const Tensor& self, Scalar other) {
  if (_has_native(self)) {
    return native_div_out(result, self, other);
  } else {
    return th_div_out(result, self, other);
  }
}

Tensor div(const Tensor& self, Scalar other) {
  Tensor r = self.type().tensor();
  return native::div_out(r, self, other);
  return r;
}

Tensor& div_(Tensor& self, Scalar other) {
  return native::div_out(self, self, other);
}

Tensor& addmm_out(Tensor& result, const Tensor& self, SparseTensorRef mat1, const Tensor& mat2, Scalar beta, Scalar alpha) {
  if (_has_native(self)) {
    return native_addmm_out(result, self, mat1, mat2, beta, alpha);
  } else {
    return th_addmm_out(result, self, mat1, mat2, beta, alpha);
  }
}

Tensor addmm(const Tensor& self, SparseTensorRef mat1, const Tensor& mat2, Scalar beta, Scalar alpha) {
  Tensor r = self.type().tensor();
  return native::addmm_out(r, self, mat1, mat2, beta, alpha);
}

Tensor& addmm_(Tensor& self, SparseTensorRef mat1, const Tensor& mat2, Scalar beta, Scalar alpha) {
  return native::addmm_out(self, self, mat1, mat2, beta, alpha);
}


Tensor tensor(const Type& dtype) {
  if (_type_has_native(dtype)) {
    return dtype.native_tensor();
  } else {
    return dtype.th_tensor();
  }
}

Tensor tensor(const Type& dtype, ArrayRef<int64_t> size) {
  if (_type_has_native(dtype)) {
    return dtype.native_tensor(size);
  } else {
    return dtype.th_tensor(size);
  }
}

Tensor sparse_coo_tensor(const Tensor& indices, const Tensor& values) {
  if (!indices.is_cuda()) {
    return native_sparse_coo_tensor(indices, values);
  } else {
    return th_sparse_coo_tensor(indices, values);
  }
}

Tensor sparse_coo_tensor(const Tensor& indices, const Tensor& values, ArrayRef<int64_t> size) {
  if (!indices.is_cuda()) {
    return native_sparse_coo_tensor(indices, values, size);
  } else {
    return th_sparse_coo_tensor(indices, values, size);
  }
}

Tensor _sparse_coo_tensor_unsafe(const Tensor& indices, const Tensor& values, ArrayRef<int64_t> size) {
  if (!indices.is_cuda()) {
    return _native_sparse_coo_tensor_unsafe(indices, values, size);
  } else {
    return _th_sparse_coo_tensor_unsafe(indices, values, size);
  }
}


Tensor& sparse_raw_resize_(Tensor& self, ArrayRef<int64_t> size, int64_t dimI, int64_t dimV) {
  if (_has_native(self)) {
    return native_sparse_raw_resize_(self, size, dimI, dimV);
  } else {
    return th_sparse_raw_resize_(self, size, dimI, dimV);
  }
}


Tensor _sparse_mask(const Tensor& self, SparseTensorRef mask) {
  if (!self.is_cuda()) {
    return _native_sparse_mask(self, mask);
  } else {
    return self._th_sparse_mask(mask);
  }
}

Tensor to_dense(const Tensor& self) {
  if (_has_native(self)) {
    return native_to_dense(self);
  } else {
    return th_to_dense(self);
  }
}

int64_t _dimI(const Tensor& self) {
  if (_has_native(self)) {
    return _native_dimI(self);
  } else {
    return _th_dimI(self);
  }
}

int64_t _dimV(const Tensor& self) {
  if (_has_native(self)) {
    return _native_dimV(self);
  } else {
    return _th_dimV(self);
  }
}

int64_t _nnz(const Tensor& self) {
  if (_has_native(self)) {
    return _native_nnz(self);
  } else {
    return _th_nnz(self);
  }
}

Tensor coalesce(const Tensor& self) {
  if (_has_native(self)) {
    return native_coalesce(self);
  } else {
    return th_coalesce(self);
  }
}

bool is_coalesced(const Tensor& self) {
  if (_has_native(self)) {
    return native_is_coalesced(self);
  } else {
    return th_is_coalesced(self);
  }
}

Tensor _indices(const Tensor& self) {
  if (_has_native(self)) {
    return _native_indices(self);
  } else {
    return _th_indices(self);
  }
}

Tensor _values(const Tensor& self) {
  if (_has_native(self)) {
    return _native_values(self);
  } else {
    return _th_values(self);
  }
}


Tensor& hspmm_out(Tensor& result, const Tensor& mat1, const Tensor& mat2) {
  if (_has_native(mat1)) {
    return native_hspmm_out(result, mat1, mat2);
  } else {
    return th_hspmm_out(result, mat1, mat2);
  }
}

Tensor hspmm(const Tensor& mat1, const Tensor& mat2) {
  if (_has_native(mat1)) {
    return native_hspmm(mat1, mat2);
  } else {
    return th_hspmm(mat1, mat2);
  }
}



}} // namespace at::native
