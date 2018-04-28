#pragma once

#include "detail/IndexUtils.cuh"
#include "ATen/TensorUtils.h"
#include "THC/THCAtomics.cuh"

//
// This file contains pointwise operation functions and kernels that
// work on both contiguous and non-contiguous tensor arguments of
// arbitrary (up to MAX_CUTORCH_DIMS) dimensioned arguments without
// copying or temporary storage.
//

namespace at {
namespace cuda {

// TODO: combine with TensorArg?  So far that's been for debugging, and this is functional...
enum class TensorArgType { ReadWrite, ReadOnly };

// Rearrange dimensions for pointwise operations so that strides are in
// decreasing order as much as possible, so that kernels have better memory
// access patterns.
//
// For example, consider a binary operation on two "transposed" 2-dim tensors:
//    sizes:          256 512
//    aInfo->strides:   1 256
//    bInfo->strides:   1 256
//
// Given this, each concurrent memory access inside kernelPointwiseApply2() is
// exactly 256 elements apart, resulting in poor performance.
//
// This function exchanges dimensions so that memory access is contiguous:
//    sizes:          512 256
//    aInfo->strides: 256   1
//    bInfo->strides: 256   1
//
// (Actually, it becomes even better because now collapseDims() can turn each
// input into one contiguous array.)
//
// In general, given M (<=4) TensorInfo's with N dimensions, we can view each
// strides[i] (0 <= i < N) as an M-tuple.  Given each pair i < j, we exchange
// strides[i] and [j] if
//    (1) strides[i][k] < strides[j][k] for some k (0 <= k < M)
//        (exchanging them will benefit input #k), and
//    (2) strides[i][k] <= strieds[j][k] for all k
//        (exchanging them will not make any input worse).
template <typename T1, typename IndexType,
          typename T2 = void, typename T3 = void, typename T4 = void>
void rearrangeDims(detail::TensorInfo<T1, IndexType>* aInfo,
                   detail::TensorInfo<T2, IndexType>* bInfo = nullptr,
                   detail::TensorInfo<T3, IndexType>* cInfo = nullptr,
                   detail::TensorInfo<T4, IndexType>* dInfo = nullptr) {
  int numInfos = 1;
  int dims = aInfo->dims;
  IndexType *sizes[4] = { aInfo->sizes, };
  IndexType *strides[4] = { aInfo->strides, };

  if (bInfo != nullptr) {
    ++numInfos;
    if (bInfo->dims != dims) return;
    sizes[1] = bInfo->sizes;
    strides[1] = bInfo->strides;
  }

  if (cInfo != nullptr) {
    ++numInfos;
    if (cInfo->dims != dims) return;
    sizes[2] = cInfo->sizes;
    strides[2] = cInfo->strides;
  }

  if (dInfo != nullptr) {
    ++numInfos;
    if (dInfo->dims != dims) return;
    sizes[3] = dInfo->sizes;
    strides[3] = dInfo->strides;
  }

  // Bail out if sizes do not match: we are using "deprecated pointwise
  // behavior" among tensors of different shapes but same number of elements.
  for (int i = 1; i < numInfos; ++i) {
    for (int j = 0; j < dims; ++j) {
      if (sizes[i][j] != sizes[0][j]) return;
    }
  }

  for (int i = 0; i < dims - 1; ++i) {
    // No need to consider dimensions of size 1.
    if (sizes[0][i] == 1) continue;

    for (int j = i + 1; j < dims; ++j) {
      if (sizes[0][j] == 1) continue;

      // Compare the relative sizes of strides between dim #i and dim #j.
      bool hasIncreasingStrides = false;
      bool hasDecreasingStrides = false;

      for (int k = 0; k < numInfos; k++) {
        IndexType stride_i = strides[k][i];
        IndexType stride_j = strides[k][j];
        if (stride_i < stride_j) {
          hasIncreasingStrides = true;
        } else if (stride_i > stride_j) {
          hasDecreasingStrides = true;
        }
      }

      if (hasIncreasingStrides && !hasDecreasingStrides) {
        for (int k = 0; k < numInfos; k++) {
          IndexType size = sizes[k][i];
          sizes[k][i] = sizes[k][j];
          sizes[k][j] = size;

          IndexType stride = strides[k][i];
          strides[k][i] = strides[k][j];
          strides[k][j] = stride;
        }
      }
    }
  }
}

// Threads per block for our apply kernel
// FIXME: use occupancy calculator instead
#define AT_APPLY_THREADS_PER_BLOCK 32 * 16
#define AT_APPLY_BLOCKS_PER_SM 4

template <typename Op,
          typename scalar1,
          typename scalar2,
          typename IndexType,
          int ADims, int BDims>
#if __CUDA_ARCH__ >= 350
__launch_bounds__(AT_APPLY_THREADS_PER_BLOCK, AT_APPLY_BLOCKS_PER_SM)
#endif
__global__ void
kernelPointwiseApply2(detail::TensorInfo<scalar1, IndexType> a,
                      detail::TensorInfo<scalar2, IndexType> b,
                      IndexType totalElements,
                      Op op) {
  for (IndexType linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < totalElements;
       linearIndex += gridDim.x * blockDim.x) {
    // Convert `linearIndex` into an offset of `a`
    const IndexType aOffset =
      detail::IndexToOffset<scalar1, IndexType, ADims>::get(linearIndex, a);

    // Convert `linearIndex` into an offset of `b`
    const IndexType bOffset =
      detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);

    bool earlyExit = false;
    op(a.data[aOffset], b.data[bOffset], earlyExit);
  }
}


template <typename Op,
          typename scalar1,
          typename scalar2,
          typename scalar3,
          typename IndexType,
          int ADims, int BDims, int CDims>
#if __CUDA_ARCH__ >= 350
__launch_bounds__(AT_APPLY_THREADS_PER_BLOCK, AT_APPLY_BLOCKS_PER_SM)
#endif
__global__ void
kernelPointwiseApply3(detail::TensorInfo<scalar1, IndexType> a,
                      detail::TensorInfo<scalar2, IndexType> b,
                      detail::TensorInfo<scalar3, IndexType> c,
                      IndexType totalElements,
                      Op op) {
  for (IndexType linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < totalElements;
       linearIndex += gridDim.x * blockDim.x) {
    // Convert `linearIndex` into an offset of `a`
    const IndexType aOffset =
      detail::IndexToOffset<scalar1, IndexType, ADims>::get(linearIndex, a);

    // Convert `linearIndex` into an offset of `b`
    const IndexType bOffset =
      detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);

    // Convert `linearIndex` into an offset of `c`
    const IndexType cOffset =
      detail::IndexToOffset<scalar3, IndexType, CDims>::get(linearIndex, c);

    op(a.data[aOffset], b.data[bOffset], c.data[cOffset]);
  }
}

template <typename Op,
          typename scalar1,
          typename scalar2,
          typename scalar3,
          typename scalar4,
          typename IndexType,
          int ADims, int BDims, int CDims, int DDims>
#if __CUDA_ARCH__ >= 350
__launch_bounds__(AT_APPLY_THREADS_PER_BLOCK, AT_APPLY_BLOCKS_PER_SM)
#endif
__global__ void
kernelPointwiseApply4(detail::TensorInfo<scalar1, IndexType> a,
                      detail::TensorInfo<scalar2, IndexType> b,
                      detail::TensorInfo<scalar3, IndexType> c,
                      detail::TensorInfo<scalar4, IndexType> d,
                      IndexType totalElements,
                      Op op) {
  for (IndexType linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < totalElements;
       linearIndex += gridDim.x * blockDim.x) {
    // Convert `linearIndex` into an offset of `a`
    const IndexType aOffset =
      detail::IndexToOffset<scalar1, IndexType, ADims>::get(linearIndex, a);

    // Convert `linearIndex` into an offset of `b`
    const IndexType bOffset =
      detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);

    // Convert `linearIndex` into an offset of `c`
    const IndexType cOffset =
      detail::IndexToOffset<scalar3, IndexType, CDims>::get(linearIndex, c);

    // Convert `linearIndex` into an offset of `d`
    const IndexType dOffset =
      detail::IndexToOffset<scalar4, IndexType, DDims>::get(linearIndex, d);

    op(a.data[aOffset], b.data[bOffset], c.data[cOffset], d.data[dOffset]);
  }
}

/**
   Computes ceil(a / b)
*/
template <typename T>
__host__ __device__ __forceinline__ T ATenCeilDiv(T a, T b) {
  return (a + b - 1) / b;
}

inline bool getApplyGrid(uint64_t totalElements, dim3& grid) {
  int curDevice = -1;
  cudaGetDevice(&curDevice);
  if (curDevice == -1) return false;

  uint64_t numBlocks = ATenCeilDiv(totalElements, static_cast<uint64_t>(AT_APPLY_THREADS_PER_BLOCK));
  uint64_t maxGridX = at::globalContext().getCurrentDeviceProperties()->maxGridSize[0];
  if (numBlocks > maxGridX)
      numBlocks = maxGridX;
  grid = dim3(numBlocks);
  return true;
}

inline dim3 getApplyBlock() {
  return dim3(AT_APPLY_THREADS_PER_BLOCK);
}

/*
  Apply a pointwise operator to two tensors.

  The calling convention for op is a function/functor that takes takes two references to
  type scalar; at least one of these references should be non-const in order to write the output.
  For example, to compute a = b^2, op would be of the form:
  [] __device__ (scalar &a_val, const scalar &b_val) { a_val = b_val * b_val; };
*/
template <typename scalar1, typename scalar2, typename Op>
bool CUDA_tensor_apply2(at::Tensor a,
                        at::Tensor b,
                        Op op,
                        TensorArgType aType = TensorArgType::ReadWrite,
                        TensorArgType bType = TensorArgType::ReadOnly) {
  checkBackend("CUDA_tensor_apply2", {a, b}, Backend::CUDA);
  int64_t totalElements = a.numel();

  if (totalElements != b.numel()) {
    return false;
  }

  if (a.dim() > MAX_TENSORINFO_DIMS ||
      b.dim() > MAX_TENSORINFO_DIMS) {
    return false;
  }

  if (a.numel() == 0) {
    // Empty tensor; do nothing
    return true;
  }
  const dim3 block = getApplyBlock();

  dim3 grid;
  if (!getApplyGrid(totalElements, grid)) {
    return false;
  }

  /*
  Expands readable/writable tensors whose indices may be "overlapped."
  This ensures that each element of the tensor is operated on once and only 
  once.
  */
  Tensor oldA;
  Tensor oldB;

  if (aType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(a)) {
    // Must perform in contiguous space
    oldA = a;
    a = a.contiguous();
  }
  if (bType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(b)) {
    // Must perform in contiguous space
    oldB = b;
    b = b.contiguous();
  }

  // It is possible that the tensor dimensions are able to be collapsed,
  // and thus we can reduce the actual code complexity of the copy by
  // exploiting this knowledge statically, since the div/mod is the
  // most expensive part of the operation, more so than memory accesses.
  // For instance, when copying a non-contiguous to a contiguous tensor
  // (or vice versa), the contiguous tensor can be collapsed to one
  // dimension, and the loop to translate the linear index to the array
  // index can be similarly collapsed. That is what this unrolling is for.

#define HANDLE_CASE(TYPE, A, B)                                         \
  kernelPointwiseApply2<Op,                                             \
                        scalar1,                                        \
                        scalar2,                                        \
                        TYPE, A, B>                                     \
   <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(    \
       aInfo, bInfo, (TYPE) totalElements, op);

#define HANDLE_B_CASE(TYPE, A, B)               \
  {                                             \
    if (bInfo.isContiguous()) {                 \
      HANDLE_CASE(TYPE, A, -2);                 \
    } else {                                    \
      switch (B) {                              \
        case 1:                                 \
        HANDLE_CASE(TYPE, A, 1);                \
        break;                                  \
        case 2:                                 \
        HANDLE_CASE(TYPE, A, 2);                \
        break;                                  \
        default:                                \
        HANDLE_CASE(TYPE, A, -1);               \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_A_CASE(TYPE, A, B)               \
  {                                             \
    if (aInfo.isContiguous()) {                 \
      HANDLE_B_CASE(TYPE, -2, B);               \
    } else {                                    \
      switch (A) {                              \
        case 1:                                 \
        HANDLE_B_CASE(TYPE, 1, B);              \
        break;                                  \
        case 2:                                 \
        HANDLE_B_CASE(TYPE, 2, B);              \
        break;                                  \
        default:                                \
        HANDLE_B_CASE(TYPE, -1, B);             \
        break;                                  \
      }                                         \
    }                                           \
  }

  if (detail::canUse32BitIndexMath(a) &&
      detail::canUse32BitIndexMath(b)) {
    detail::TensorInfo<scalar1, unsigned int> aInfo =
      detail::getTensorInfo<scalar1, unsigned int>(a);

    detail::TensorInfo<scalar2, unsigned int> bInfo =
      detail::getTensorInfo<scalar2, unsigned int>(b);
    rearrangeDims(&aInfo, &bInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();
#if CUDA_VERSION < 9000
    if (!(aInfo.isContiguous() && bInfo.isContiguous()))
        grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif

    HANDLE_A_CASE(unsigned int, aInfo.dims, bInfo.dims);
  } else {
    detail::TensorInfo<scalar1, uint64_t> aInfo =
      detail::getTensorInfo<scalar1, uint64_t>(a);

    detail::TensorInfo<scalar2, uint64_t> bInfo =
      detail::getTensorInfo<scalar2, uint64_t>(b);
    rearrangeDims(&aInfo, &bInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();

    // For large tensors, we only compile the completely contiguous
    // version and the completely generic version, to reduce
    // compilation time.
    if (aInfo.isContiguous() && bInfo.isContiguous()) {
      kernelPointwiseApply2<Op,
                            scalar1,
                            scalar2,
                          uint64_t, -2, -2>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
           aInfo, bInfo, (uint64_t) totalElements, op);
    } else {
#if CUDA_VERSION < 9000
      grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif
      kernelPointwiseApply2<Op,
                            scalar1,
                            scalar2,
                            uint64_t, -1, -1>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
           aInfo, bInfo, (uint64_t) totalElements, op);
    }
  }
#undef HANDLE_CASE
#undef HANDLE_B_CASE
#undef HANDLE_A_CASE

  if (oldA.defined()) {
    // Ignore overlaps when copying back; if we use copy
    // instead, it will recursively try and invoke ourselves to make
    // oldA contiguous.
    oldA._copy_ignoring_overlaps_(a);
    a = oldA;
  }

  if (oldB.defined()) {
    // Ignore overlaps when copying back; if we use copy
    // instead, it will recursively try and invoke ourselves to make
    // oldB contiguous.
    oldB._copy_ignoring_overlaps_(b);
    b = oldB;
  }

  return true;
}

/*
  Apply a pointwise operator to three tensors.

  The calling convention for op is a function/functor that takes takes three references to
  type scalar; at least one of these references should be non-const in order to write the output.
  For example, to compute a = b + c, op would be of the form:
  [] __device__ (scalar &a_val, const scalar &b_val, const scalar &c_val) {
    a_val = b_val + c_val;
  };
*/
template <typename scalar1, typename scalar2, typename scalar3, typename Op>
bool CUDA_tensor_apply3(at::Tensor a,
                        at::Tensor b,
                        at::Tensor c,
                        const Op& op,
                        TensorArgType aType = TensorArgType::ReadWrite,
                        TensorArgType bType = TensorArgType::ReadOnly,
                        TensorArgType cType = TensorArgType::ReadOnly) {
  checkBackend("CUDA_tensor_apply3", {a, b, c}, Backend::CUDA);
  int64_t totalElements = a.numel();

  if (totalElements != b.numel() ||
      totalElements != c.numel()) {
    return false;
  }

  if (a.dim() > MAX_TENSORINFO_DIMS ||
      b.dim() > MAX_TENSORINFO_DIMS ||
      c.dim() > MAX_TENSORINFO_DIMS) {
    return false;
  }

  if (a.numel() == 0) {
    // Empty tensor; do nothing
    return true;
  }

  const dim3 block = getApplyBlock();

  dim3 grid;
  if (!getApplyGrid(totalElements, grid)) {
    return false;
  }

  /*
  Expands readable/writable tensors whose indices may be "overlapped."
  This ensures that each element of the tensor is operated on once and only 
  once.
  */
  Tensor oldA;
  Tensor oldB;
  Tensor oldC;

  if (aType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(a)) {
    // Must perform in contiguous space
    oldA = a;
    a = a.contiguous();
  }
  if (bType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(b)) {
    // Must perform in contiguous space
    oldB = b;
    b = b.contiguous();
  }
  if (cType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(c)) {
    // Must perform in contiguous space
    oldC = c;
    c = c.contiguous();
  }

#define HANDLE_CASE(TYPE, A, B, C)                                      \
  kernelPointwiseApply3<Op,                                             \
                        scalar1,                                        \
                        scalar2,                                        \
                        scalar3,                                        \
                        TYPE, A, B, C>                                  \
    <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(   \
      aInfo, bInfo, cInfo, (TYPE) totalElements, op);

#define HANDLE_C_CASE(TYPE, A, B, C)            \
  {                                             \
    if (cInfo.isContiguous()) {                 \
      HANDLE_CASE(TYPE, A, B, -2);              \
    } else {                                    \
      switch (C) {                              \
        case 1:                                 \
        HANDLE_CASE(TYPE, A, B, 1);             \
        break;                                  \
        case 2:                                 \
        HANDLE_CASE(TYPE, A, B, 2);             \
        break;                                  \
        default:                                \
        HANDLE_CASE(TYPE, A, B, -1);            \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_B_CASE(TYPE, A, B, C)            \
  {                                             \
    if (bInfo.isContiguous()) {                 \
      HANDLE_C_CASE(TYPE, A, -2, C);            \
    } else {                                    \
      switch (B) {                              \
        case 1:                                 \
        HANDLE_C_CASE(TYPE, A, 1, C);           \
        break;                                  \
        case 2:                                 \
        HANDLE_C_CASE(TYPE, A, 2, C);           \
        break;                                  \
        default:                                \
        HANDLE_C_CASE(TYPE, A, -1, C);          \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_A_CASE(TYPE, A, B, C)            \
  {                                             \
    if (aInfo.isContiguous()) {                 \
      HANDLE_B_CASE(TYPE, -2, B, C);            \
    } else {                                    \
      switch (A) {                              \
        case 1:                                 \
        HANDLE_B_CASE(TYPE, 1, B, C);           \
        break;                                  \
        case 2:                                 \
        HANDLE_B_CASE(TYPE, 2, B, C);           \
        break;                                  \
        default:                                \
        HANDLE_B_CASE(TYPE, -1, B, C);          \
        break;                                  \
      }                                         \
    }                                           \
  }

  if (detail::canUse32BitIndexMath(a) &&
      detail::canUse32BitIndexMath(b) &&
      detail::canUse32BitIndexMath(c)) {
    detail::TensorInfo<scalar1, unsigned int> aInfo =
      detail::getTensorInfo<scalar1, unsigned int>(a);

    detail::TensorInfo<scalar2, unsigned int> bInfo =
      detail::getTensorInfo<scalar2, unsigned int>(b);

    detail::TensorInfo<scalar3, unsigned int> cInfo =
      detail::getTensorInfo<scalar3, unsigned int>(c);

    rearrangeDims(&aInfo, &bInfo, &cInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();
    cInfo.collapseDims();

#if CUDA_VERSION < 9000
    if (!(aInfo.isContiguous() && bInfo.isContiguous() && cInfo.isContiguous()))
      grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif
    HANDLE_A_CASE(unsigned int, aInfo.dims, bInfo.dims, cInfo.dims);
  } else {
    detail::TensorInfo<scalar1, uint64_t> aInfo =
      detail::getTensorInfo<scalar1, uint64_t>(a);

    detail::TensorInfo<scalar2, uint64_t> bInfo =
      detail::getTensorInfo<scalar2, uint64_t>(b);

    detail::TensorInfo<scalar3, uint64_t> cInfo =
      detail::getTensorInfo<scalar3, uint64_t>(c);

    rearrangeDims(&aInfo, &bInfo, &cInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();
    cInfo.collapseDims();

    // For large tensors, we only compile the completely contiguous
    // version and the completely generic version, to reduce
    // compilation time.
    if (aInfo.isContiguous() && bInfo.isContiguous() && cInfo.isContiguous()) {
      kernelPointwiseApply3<Op,
                            scalar1,
                            scalar2,
                            scalar3,
                            uint64_t, -2, -2, -2>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
          aInfo, bInfo, cInfo, (uint64_t) totalElements, op);
    } else {
#if CUDA_VERSION < 9000
  grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif

	kernelPointwiseApply3<Op,
                        scalar1,
                        scalar2,
                        scalar3,
                        uint64_t, -1, -1, -1>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
          aInfo, bInfo, cInfo, (uint64_t) totalElements, op);
    }
  }
#undef HANDLE_CASE
#undef HANDLE_C_CASE
#undef HANDLE_B_CASE
#undef HANDLE_A_CASE

  if (oldA.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldA contiguous.
    oldA._copy_ignoring_overlaps_(a);
    a = oldA;
  }

  if (oldB.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldB contiguous.
    oldB._copy_ignoring_overlaps_(b);
    b = oldB;
  }

  if (oldC.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldC contiguous.
    oldC._copy_ignoring_overlaps_(c);
    c = oldC;
  }

  return true;
}

/*
  Apply a pointwise operator to four tensors.

  The calling convention for op is a function/functor that takes takes four references to
  type scalar; at least one of these references should be non-const in order to write the output.
  For example, to compute a = b + c * d, op would be of the form:
  [] __device__ (scalar &a_val, const scalar &b_val, const scalar &c_val, const scalar &d_val) {
    a_val = b_val + c_val * d_val;
  };
*/
template <typename scalar1, typename scalar2, typename scalar3, typename scalar4, typename Op>
bool CUDA_tensor_apply4(at::Tensor a,
                        at::Tensor b,
                        at::Tensor c,
                        at::Tensor d,
                        const Op& op,
                        TensorArgType aType = TensorArgType::ReadWrite,
                        TensorArgType bType = TensorArgType::ReadOnly,
                        TensorArgType cType = TensorArgType::ReadOnly,
                        TensorArgType dType = TensorArgType::ReadOnly) {
  checkBackend("CUDA_tensor_apply4", {a, b, c, d}, Backend::CUDA);
  int64_t totalElements = a.numel();

  if (totalElements != b.numel() ||
      totalElements != c.numel() ||
      totalElements != d.numel()) {
    return false;
  }

  if (a.dim() > MAX_TENSORINFO_DIMS ||
      b.dim() > MAX_TENSORINFO_DIMS ||
      c.dim() > MAX_TENSORINFO_DIMS ||
      d.dim() > MAX_TENSORINFO_DIMS) {
    return false;
  }

  if (a.numel() == 0) {
    // Empty tensor; do nothing
    return true;
  }

  const dim3 block = getApplyBlock();

  dim3 grid;
  if (!getApplyGrid(totalElements, grid)) {
    return false;
  }

  /*
  Expands readable/writable tensors whose indices may be "overlapped."
  This ensures that each element of the tensor is operated on once and only 
  once.
  */
  Tensor oldA;
  Tensor oldB;
  Tensor oldC;
  Tensor oldD;

  if (aType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(a)) {
    // Must perform in contiguous space
    oldA = a;
    a = a.contiguous();
  }
  if (bType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(b)) {
    // Must perform in contiguous space
    oldB = b;
    b = b.contiguous();
  }
  if (cType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(c)) {
    // Must perform in contiguous space
    oldC = c;
    c = c.contiguous();
  }
  if (dType == TensorArgType::ReadWrite && detail::maybeOverlappingIndices(c)) {
    // Must perform in contiguous space
    oldD = d;
    d = d.contiguous();
  }

#define HANDLE_CASE(TYPE, A, B, C, D)                                   \
  kernelPointwiseApply4<Op,                                             \
                        scalar1,                                        \
                        scalar2,                                        \
                        scalar3,                                        \
                        scalar4,                                        \
                        TYPE, A, B, C, D>                               \
    <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(   \
    aInfo, bInfo, cInfo, dInfo, (TYPE) totalElements, op);

#define HANDLE_D_CASE(TYPE, A, B, C, D)         \
  {                                             \
    if (dInfo.isContiguous()) {                 \
      HANDLE_CASE(TYPE, A, B, C, -2);           \
    } else {                                    \
      switch (D) {                              \
        case 1:                                 \
        HANDLE_CASE(TYPE, A, B, C, 1);          \
        break;                                  \
        case 2:                                 \
        HANDLE_CASE(TYPE, A, B, C, 2);          \
        break;                                  \
        default:                                \
        HANDLE_CASE(TYPE, A, B, C, -1);         \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_C_CASE(TYPE, A, B, C, D)         \
  {                                             \
    if (cInfo.isContiguous()) {                 \
      HANDLE_D_CASE(TYPE, A, B, -2, D);         \
    } else {                                    \
      switch (C) {                              \
        case 1:                                 \
        HANDLE_D_CASE(TYPE, A, B, 1, D);        \
        break;                                  \
        case 2:                                 \
        HANDLE_D_CASE(TYPE, A, B, 2, D);        \
        break;                                  \
        default:                                \
        HANDLE_D_CASE(TYPE, A, B, -1, D);       \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_B_CASE(TYPE, A, B, C, D)         \
  {                                             \
    if (bInfo.isContiguous()) {                 \
      HANDLE_C_CASE(TYPE, A, -2, C, D);         \
    } else {                                    \
      switch (B) {                              \
        case 1:                                 \
        HANDLE_C_CASE(TYPE, A, 1, C, D);        \
        break;                                  \
        case 2:                                 \
        HANDLE_C_CASE(TYPE, A, 2, C, D);        \
        break;                                  \
        default:                                \
        HANDLE_C_CASE(TYPE, A, -1, C, D);       \
        break;                                  \
      }                                         \
    }                                           \
  }

#define HANDLE_A_CASE(TYPE, A, B, C, D)         \
  {                                             \
    if (aInfo.isContiguous()) {                 \
      HANDLE_B_CASE(TYPE, -2, B, C, D);         \
    } else {                                    \
      switch (A) {                              \
        case 1:                                 \
        HANDLE_B_CASE(TYPE, 1, B, C, D);        \
        break;                                  \
        case 2:                                 \
        HANDLE_B_CASE(TYPE, 2, B, C, D);        \
        break;                                  \
        default:                                \
        HANDLE_B_CASE(TYPE, -1, B, C, D);       \
        break;                                  \
      }                                         \
    }                                           \
  }

  if (detail::canUse32BitIndexMath(a) &&
      detail::canUse32BitIndexMath(b) &&
      detail::canUse32BitIndexMath(c) &&
      detail::canUse32BitIndexMath(d)) {
    detail::TensorInfo<scalar1, unsigned int> aInfo =
      detail::getTensorInfo<scalar1, unsigned int>(a);

    detail::TensorInfo<scalar2, unsigned int> bInfo =
      detail::getTensorInfo<scalar2, unsigned int>(b);

    detail::TensorInfo<scalar3, unsigned int> cInfo =
      detail::getTensorInfo<scalar3, unsigned int>(c);

    detail::TensorInfo<scalar4, unsigned int> dInfo =
      detail::getTensorInfo<scalar4, unsigned int>(d);

    rearrangeDims(&aInfo, &bInfo, &cInfo, &dInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();
    cInfo.collapseDims();
    dInfo.collapseDims();

#if CUDA_VERSION < 9000
    if (!(aInfo.isContiguous() && bInfo.isContiguous() && cInfo.isContiguous() && dInfo.isContiguous()))
      grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif
    HANDLE_A_CASE(unsigned int, aInfo.dims, bInfo.dims, cInfo.dims, dInfo.dims);
  } else {
    detail::TensorInfo<scalar1, uint64_t> aInfo =
      detail::getTensorInfo<scalar1, uint64_t>(a);

    detail::TensorInfo<scalar2, uint64_t> bInfo =
      detail::getTensorInfo<scalar2, uint64_t>(b);

    detail::TensorInfo<scalar3, uint64_t> cInfo =
      detail::getTensorInfo<scalar3, uint64_t>(c);

    detail::TensorInfo<scalar4, uint64_t> dInfo =
      detail::getTensorInfo<scalar4, uint64_t>(d);

    rearrangeDims(&aInfo, &bInfo, &cInfo, &dInfo);
    aInfo.collapseDims();
    bInfo.collapseDims();
    cInfo.collapseDims();
    dInfo.collapseDims();

    // For large tensors, we only compile the completely contiguous
    // version and the completely generic version, to reduce
    // compilation time.
    if (aInfo.isContiguous() && bInfo.isContiguous() && cInfo.isContiguous() && dInfo.isContiguous()) {
      kernelPointwiseApply4<Op,
                            scalar1,
                            scalar2,
                            scalar3,
                            scalar4,
                            uint64_t, -2, -2, -2, -2>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
          aInfo, bInfo, cInfo, dInfo, (uint64_t) totalElements, op);
    } else {
#if CUDA_VERSION < 9000
  grid.x = std::min((unsigned int)at::globalContext().getCurrentDeviceProperties()->multiProcessorCount * AT_APPLY_BLOCKS_PER_SM , grid.x);
#endif

	kernelPointwiseApply4<Op,
                        scalar1,
                        scalar2,
                        scalar3,
                        scalar4,
                        uint64_t, -1, -1, -1, -1>
        <<<grid, block, 0, at::globalContext().getCurrentCUDAStream()>>>(
          aInfo, bInfo, cInfo, dInfo, (uint64_t) totalElements, op);
    }
  }
#undef HANDLE_CASE
#undef HANDLE_D_CASE
#undef HANDLE_C_CASE
#undef HANDLE_B_CASE
#undef HANDLE_A_CASE

  if (oldA.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldA contiguous.
    oldA._copy_ignoring_overlaps_(a);
    a = oldA;
  }

  if (oldB.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldB contiguous.
    oldB._copy_ignoring_overlaps_(b);
    b = oldB;
  }

  if (oldC.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldC contiguous.
    oldC._copy_ignoring_overlaps_(c);
    c = oldC;
  }

  if (oldD.defined()) {
    // Ignore overlaps when copying back; if we use THCTensor_copy
    // instead, it will recursively try and invoke ourselves to make
    // oldC contiguous.
    oldD._copy_ignoring_overlaps_(c);
    d = oldD;
  }

  return true;
}

#define MIN_NUMBER_BINS_FOR_GLOBAL_MEM 5000
#define FOR_KERNEL_LOOP(i, lim)                                      \
  for (IndexType i = blockIdx.x * blockDim.x + threadIdx.x; i < lim; \
       i += gridDim.x * blockDim.x)

/*
Memory types used for the 3 histogram implementations.
See `CUDA_tensor_histogram` below.
*/
enum class CUDAHistogramMemoryType { MULTI_BLOCK, SHARED, GLOBAL };

/*
  Kernel for computing the histogram of the input.
 */
template <
    typename scalar1,
    typename scalar2,
    typename IndexType,
    int ADims,
    int PDims,
    int BDims,
    CUDAHistogramMemoryType MemoryType = CUDAHistogramMemoryType::MULTI_BLOCK,
    typename Op>
__global__ void kernelHistogram1D(
    detail::TensorInfo<scalar1, IndexType> a, /* output */
    detail::TensorInfo<scalar1, IndexType> p, /* partial output */
    detail::TensorInfo<scalar2, IndexType> b, /* input */
    int binsize,
    IndexType totalElements,
    Op getOp) {
  extern __shared__ unsigned char my_smem[];
  scalar1* smem = nullptr;

  if (MemoryType == CUDAHistogramMemoryType::SHARED) {
    ////////////////////////// Shared memory //////////////////////////
    // atomically add to block specific shared memory
    // then atomically add to the global output tensor
    smem = reinterpret_cast<scalar1*>(my_smem);
    for (IndexType i = threadIdx.x; i < a.sizes[0]; i += blockDim.x) {
      smem[i] = 0;
    }
    __syncthreads();
    FOR_KERNEL_LOOP(linearIndex, totalElements) {
      // Convert `linearIndex` into an offset of `b`
      const IndexType bOffset =
          detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);
      // Use value at `b` as an offset of `smem`
      const IndexType pOffset = b.data[bOffset] / binsize;
      atomicAdd(&smem[pOffset], getOp(linearIndex));
    }
    __syncthreads();
    // NOTE: atomically update output bin count.
    //   Atomic update is imp since __syncthread() will only synchronize threads
    //   in a given block, not across blocks.
    for (IndexType i = threadIdx.x; i < a.sizes[0]; i += blockDim.x) {
      const IndexType aOffset =
          detail::IndexToOffset<scalar1, IndexType, ADims>::get(i, a);
      atomicAdd(&a.data[aOffset], smem[i]);
    }

  } else if (MemoryType == CUDAHistogramMemoryType::MULTI_BLOCK) {
    ////////////////////////// Multi Block memory //////////////////////////
    // atomically add to block specific global tensor
    // then atomically add to the global output tensor
    // compute histogram for the block
    FOR_KERNEL_LOOP(linearIndex, totalElements) {
      // Convert `linearIndex` into an offset of `b`
      const IndexType bOffset =
          detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);
      const auto bVal = b.data[bOffset];
      // Use value at `b` as an offset of `p`
      const IndexType pIdx = p.strides[0] * blockIdx.x + bVal / binsize;
      const IndexType pOffset =
          detail::IndexToOffset<scalar1, IndexType, PDims>::get(pIdx, p);
      atomicAdd(&p.data[pOffset], getOp(linearIndex));
    }
    __syncthreads();
    // NOTE: atomically update output bin count.
    //   Atomic update is imp since __syncthread() will only synchronize threads
    //   in a given block, not across blocks.
    const IndexType pIdx = p.strides[0] * blockIdx.x;
    const IndexType pOffset =
        detail::IndexToOffset<scalar1, IndexType, PDims>::get(pIdx, p);
    for (IndexType i = threadIdx.x; i < a.sizes[0]; i += blockDim.x) {
      const IndexType aOffset =
          detail::IndexToOffset<scalar1, IndexType, ADims>::get(i, a);
      atomicAdd(&a.data[aOffset], p.data[pOffset + i]);
    }

  } else {
    ////////////////////////// Global memory //////////////////////////
    // atomically add to the output tensor
    // compute histogram for the block
    FOR_KERNEL_LOOP(linearIndex, totalElements) {
      // Convert `linearIndex` into an offset of `b`
      const IndexType bOffset =
          detail::IndexToOffset<scalar2, IndexType, BDims>::get(linearIndex, b);
      const auto bVal = b.data[bOffset];
      // Use value at `b` as an offset of `a`
      const IndexType aIdx = bVal / binsize;
      const IndexType aOffset =
          detail::IndexToOffset<scalar1, IndexType, ADims>::get(aIdx, a);
      atomicAdd(&a.data[aOffset], getOp(linearIndex));
    }
  }
}

#define HANDLE_CASE(MEMORY_TYPE, WEIGHTS_OP)                               \
  kernelHistogram1D<scalar1, scalar2, IndexType, 1, 2, 1, MEMORY_TYPE>     \
      <<<grid,                                                             \
         block,                                                            \
         (MEMORY_TYPE == CUDAHistogramMemoryType::SHARED) ? sharedMem : 0, \
         at::globalContext().getCurrentCUDAStream()>>>(                    \
          aInfo, pInfo, bInfo, binsize, totalElements, WEIGHTS_OP);        \
  AT_ASSERT(cudaGetLastError() == cudaSuccess, "kernelHistogram1D failed");

#define HANDLE_SWITCH_CASE(mType, getOp)                                      \
  switch (mType) {                                                            \
    case CUDAHistogramMemoryType::SHARED:                                     \
      HANDLE_CASE(CUDAHistogramMemoryType::SHARED, getOp);                    \
      break;                                                                  \
    case CUDAHistogramMemoryType::MULTI_BLOCK:                                \
      HANDLE_CASE(CUDAHistogramMemoryType::MULTI_BLOCK, getOp);               \
      break;                                                                  \
    default:                                                                  \
      std::cerr << "WARNING: Potentially slow. "                              \
                   "CUDA_tensor_histogram with nbins = "                      \
                << nbins << " uses global memory with atomics." << std::endl; \
      HANDLE_CASE(CUDAHistogramMemoryType::GLOBAL, getOp);                    \
  }

/*
  Calculate the frequesncy of the input values.

  `a` contains the final output or the histogram. Input `b` is assumed to
  be 1-D non-negative int array. `c` optionally contains the weight vector.
  See `help torch.bincount` for details on the math.

  3 implementations based of input size and memory usage:
    SHARED: Each block atomically adds to it's own **shared** hist copy, then
        atomically updates the global tensor.
        case: #bins < blockDim.x
    MULTI_BLOCK: Each block atomically adds to it's own **global** hist copy,
        then  atomically updates the global tensor.
        case: blockDim.x <= #bins < MIN_NUMBER_BINS_FOR_GLOBAL_MEM
    GLOBAL: all threads atomically update to a single **global** hist copy.
        case: MIN_NUMBER_BINS_FOR_GLOBAL_MEM <= #bins
 */
template <typename scalar1, typename scalar2>
bool CUDA_tensor_histogram(
    at::Tensor a, /* output */
    at::Tensor b, /* input */
    at::Tensor c, /* weights(optional) */
    int64_t nbins,
    int binsize,
    TensorArgType aType = TensorArgType::ReadWrite,
    TensorArgType bType = TensorArgType::ReadOnly,
    TensorArgType cType = TensorArgType::ReadOnly) {
  checkBackend("CUDA_tensor_histogram", {a, b}, Backend::CUDA);
  constexpr int has_weights = !std::is_integral<scalar1>::value;
  if (has_weights) {
    checkBackend("CUDA_tensor_histogram", {c}, Backend::CUDA);
  }
  int64_t totalElements = b.numel();

  const dim3 block = getApplyBlock();
  dim3 grid;
  if (!getApplyGrid(totalElements, grid)) {
    return false;
  }
#if CUDA_VERSION < 9000
  grid.x = std::min(
      (unsigned int)at::globalContext()
              .getCurrentDeviceProperties()
              ->multiProcessorCount *
          AT_APPLY_BLOCKS_PER_SM,
      grid.x);
#endif

  CUDAHistogramMemoryType memType = CUDAHistogramMemoryType::SHARED;
  auto maxSharedMem =
      at::globalContext().getCurrentDeviceProperties()->sharedMemPerBlock;
  auto sharedMem = nbins * sizeof(scalar1) + 8; // 8 guard bytes
  // determine memory type to use in the kernel
  if (nbins < block.x && sharedMem < maxSharedMem) {
    memType = CUDAHistogramMemoryType::SHARED;
  } else if (nbins < MIN_NUMBER_BINS_FOR_GLOBAL_MEM) {
    memType = CUDAHistogramMemoryType::MULTI_BLOCK;
  } else {
    memType = CUDAHistogramMemoryType::GLOBAL;
  }
  // alloc memory for SHARED and MULTI_BLOCK
  using IndexType = uint64_t;
  auto aInfo = detail::getTensorInfo<scalar1, IndexType>(a);
  auto bInfo = detail::getTensorInfo<scalar2, IndexType>(b);
  detail::TensorInfo<scalar1, IndexType> pInfo = aInfo;
  Tensor partial_output;
  if (memType == CUDAHistogramMemoryType::MULTI_BLOCK) {
    partial_output = a.type().zeros({grid.x, nbins});
    pInfo = detail::getTensorInfo<scalar1, IndexType>(partial_output);
  }

  if (has_weights) {
    auto cInfo = detail::getTensorInfo<scalar1, IndexType>(c);
    const auto getWeightsOp = [cInfo] __device__(IndexType cIndex) {
      const IndexType cOffset =
          detail::IndexToOffset<scalar1, IndexType, 1>::get(cIndex, cInfo);
      return cInfo.data[cOffset];
    };
    HANDLE_SWITCH_CASE(memType, getWeightsOp)
  } else {
    static const auto getDummyOp = [] __device__(IndexType) { return 1L; };
    HANDLE_SWITCH_CASE(memType, getDummyOp)
  }
  return true;
}

#undef HANDLE_CASE
#undef HANDLE_SWITCH_CASE
#undef FOR_KERNEL_LOOP
#undef MIN_NUMBER_BINS_FOR_GLOBAL_MEM

} // cuda
} // at
