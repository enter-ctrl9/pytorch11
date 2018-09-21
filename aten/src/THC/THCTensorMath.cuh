#ifndef THC_TENSORMATH_CUH
#define THC_TENSORMATH_CUH

// Copy the kth diagonal of a matrix B to a vector A.
template <typename T>
__global__ void THCTensor_copyFromDiagonal(T* a, T* b, ptrdiff_t start, ptrdiff_t size, ptrdiff_t strideSum, ptrdiff_t strideA) {
  for (ptrdiff_t linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < size;
       linearIndex += gridDim.x * blockDim.x) {
    const ptrdiff_t bOffset = start + strideSum * linearIndex;
    a[strideA * linearIndex] = b[bOffset];
  }
}

// Copy vector B to the kth diagonal of a matrix A
template <typename T>
__global__ void THCTensor_copyToDiagonal(T* a, T* b, ptrdiff_t start, ptrdiff_t size, ptrdiff_t strideSum, ptrdiff_t strideB) {
  for (ptrdiff_t linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < size;
       linearIndex += gridDim.x * blockDim.x) {
    const ptrdiff_t aOffset = start + strideSum * linearIndex;
    a[aOffset] = b[strideB * linearIndex];
  }
}

#define CAT_ARRAY_BATCH_SIZE 1024
#define CAT_ARRAY_MAX_INPUT_DIMS 4

inline bool getCatGrid(THCState* state, ptrdiff_t nTensors, dim3& grid) {
  int curDevice = -1;
  cudaGetDevice(&curDevice);

  if (curDevice == -1) {
     return false;
  }

  // Assume a reasonable number of SMs if no state is available
  int numSM =
        state ? THCState_getCurrentDeviceProperties(state)->multiProcessorCount : 15;
  //X dim of grid for cat array cooperates on a single tensor in the cat.
  //Given half of the GPU, full utilization will always occur.
  grid = dim3( 2LL * numSM, (long long) nTensors );

  return true;
}

template<typename IndexType, unsigned int MaxDims>
struct OutputTensorSizeStride {
  IndexType outputSize[MaxDims];
  IndexType outputStride[MaxDims];
};

template<typename IndexType, unsigned int MaxDims>
struct InputTensorSize {
  IndexType inputSize[MaxDims];
  IndexType inputStride[MaxDims];
};

template <typename T, typename IndexType, unsigned int MaxDims>
struct CatArrInputTensor {
  T* input;
  IndexType offset;
  InputTensorSize<IndexType, CAT_ARRAY_MAX_INPUT_DIMS> inputParam;
  IndexType nElements;
  IndexType nElementsOutput;
};

/**
  * Kernel used to concatenated grimDim.y tensors into an output tensor. Uses a grid-stride loop based off of
  * the blockIdx.x, threadIdx.x for each input to copy each element from each input tensor into the output.
  *
  * output: base pointer to the storage associated with the output tensor
  * inputs: GPU-allocated array of input metadata for each input to concatenate in the kernel
  * os: the size/stride vectors for the output tensor
  * concatDim: dimension along which we are concatenating
  * dimStride: the stride of the output tensor at the concatDim
  *
  * The most important assumption made is that the input tensors are contiguous.
  */
template <typename T, typename IndexType, int Dims>
__global__ void CatArrayBatchedCopy(
    T* output,
    CatArrInputTensor<T, IndexType, CAT_ARRAY_MAX_INPUT_DIMS>* inputs,
    OutputTensorSizeStride<IndexType, CAT_ARRAY_MAX_INPUT_DIMS> os,
    const int concatDim,
    T pad_value) {

    IndexType nElementsOutput = inputs[blockIdx.y].nElementsOutput;
    T* data = inputs[blockIdx.y].input;
    IndexType dimOffset = inputs[blockIdx.y].offset;
    IndexType dataOffset = dimOffset * os.outputStride[concatDim];

    for (IndexType linearIndex = (IndexType) blockIdx.x * blockDim.x + threadIdx.x;
      linearIndex < nElementsOutput;
      linearIndex += (IndexType) gridDim.x * blockDim.x) {
      IndexType inputOffset = 0;
      IndexType outputOffset = 0;
      bool inbound = true;
      for (int i = Dims - 1; i >= 1; --i) {
        IndexType inputDimSize = inputs[blockIdx.y].inputParam.inputSize[i];
        IndexType curDimSize = i == concatDim ? inputDimSize : os.outputSize[i];
        IndexType nextDimIndex = linearIndex / curDimSize;
        IndexType curDimIndex = linearIndex - curDimSize * nextDimIndex;
        if (curDimIndex >= inputDimSize && inbound) {
          inbound = false;
        }
        inputOffset += curDimIndex * inputs[blockIdx.y].inputParam.inputStride[i];
        outputOffset += curDimIndex * os.outputStride[i];
        linearIndex = nextDimIndex;
      }
      inputOffset += linearIndex * inputs[blockIdx.y].inputParam.inputStride[0];
      outputOffset += linearIndex * os.outputStride[0];
      if (inbound) {
        output[dataOffset + outputOffset] = data[inputOffset];
      } else {
        output[dataOffset + outputOffset] = pad_value;
      }
    }
}

#endif
