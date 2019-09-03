// updateOutput, updateGradInput Kernels ported from Sergey Zagoruyko's pyinn, which itself was a
// port from Caffe

#include <THCUNN/THCUNN.h>
#include <THC/THCTensor.hpp>
#include <THC/THCDeviceTensor.cuh>
#include <THC/THCDeviceTensorUtils.cuh>
#include <THC/THCNumerics.cuh>
#include <THC/THCReduceApplyUtils.cuh>
#include <THC/THCSortUtils.cuh>
#include <THC/THCTensorMathReduce.cuh>
#include <THCUNN/SharedMem.cuh>
#include <THCUNN/common.h>
#include <algorithm>


const int WARP_SIZE = 32;
// Crude benchmarks suggest 256 is better than 512 and 1024
// TODO: Autotune/use better heuristics, improve speed more.
const int MAX_BLOCK_SIZE = 256;

static int getGradParamsNumThreads(int batchSize){
//warp per item in a batch, up to a maximum
   return std::min(batchSize * WARP_SIZE, MAX_BLOCK_SIZE);

}

template <typename T, typename AccT, typename IndexType, int kSize>
__global__ void spatialDepthwiseConvolutionUpdateOutput(
    const THCDeviceTensor<T, 4> input,
    THCDeviceTensor<T, 4> output,
    const THCDeviceTensor<T, 4> weight,
    const THCDeviceTensor<T, 1> bias,
    bool biasEnabled,
    IndexType totalElements,
    const int outputChannels,
    const int depthwiseMultiplier,
    const int inputWidth, const int inputHeight,
    const int outputWidth, const int outputHeight,
    const int kernelWidth, const int kernelHeight,
    const int strideWidth, const int strideHeight,
    const int padWidth, const int padHeight,
    const int dilationWidth, const int dilationHeight)
{
  const int inputChannels = outputChannels / depthwiseMultiplier;

  for (IndexType thread_id = blockIdx.x * blockDim.x + threadIdx.x;
       thread_id < totalElements;
       thread_id += gridDim.x * blockDim.x) {

    const int out_col = thread_id % outputWidth;
    const int out_row = (thread_id / outputWidth) % outputHeight;
    const int out_channel = (thread_id / outputWidth / outputHeight) % outputChannels;
    const int batch = thread_id / outputWidth / outputHeight / outputChannels;

    const int in_channel = out_channel / depthwiseMultiplier;
    const int multiplier = out_channel % depthwiseMultiplier;

    const int input_offset_temp =
        (batch * inputChannels + in_channel) * (inputHeight * inputWidth);

    const int input_row_start = out_row * strideWidth - padHeight;
    const int input_col_start = out_col * strideHeight - padWidth;
    const int input_row_end = input_row_start + kernelHeight;
    const int input_col_end = input_col_start + kernelWidth;

    AccT value = biasEnabled ? ScalarConvert<T, AccT>::to(bias.data()[c]) : ScalarConvert<int, AccT>::to(0);
    if (input_row_start >= 0 && input_col_start >= 0 &&
        input_row_end < inputHeight && input_col_end < inputWidth) {
#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
      for (int filter_row = 0; filter_row < kernelHeight; ++filter_row) {
        const int in_row = input_row_start + filter_row;
        const int filter_offset_temp = kernelWidth * filter_row;
#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
        for (int filter_col = 0; filter_col < kernelWidth; ++filter_col) {
          const int in_col = input_col_start + filter_col;

          const int input_offset =
              (input_offset_temp) + (in_row * inputWidth) + in_col;
          const int filter_offset =
              multiplier +
              depthwiseMultiplier *
                  (in_channel + inputChannels * (filter_col + filter_offset_temp));
          value = THCNumerics<AccT>::add(
            value,
            THCNumerics<AccT>::mul(
              ScalarConvert<T, AccT>::to(weight.data()[filter_offset]),
              ScalarConvert<T, AccT>::to(input.data()[input_offset])));
        }
      }
    } else {
#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
      for (int filter_row = 0; filter_row < kernelHeight; ++filter_row) {
        const int in_row = input_row_start + filter_row;
        const int filter_offset_temp = kernelWidth * filter_row;
#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
        for (int filter_col = 0; filter_col < kernelWidth; ++filter_col) {
          const int in_col = input_col_start + filter_col;

          if (in_row >= 0 && in_row < inputHeight && in_col >= 0 &&
              in_col < inputWidth) {
            const int in_col = input_col_start + filter_col;

            const int input_offset =
                (input_offset_temp) + (in_row * inputWidth) + in_col;

            const int filter_offset =
                multiplier +
                depthwiseMultiplier *
                    (in_channel + inputChannels * (filter_col + filter_offset_temp));
            value = THCNumerics<AccT>::add(
              value,
              THCNumerics<AccT>::mul(
                ScalarConvert<T, AccT>::to(weight.data()[filter_offset]),
                ScalarConvert<T, AccT>::to(input.data()[input_offset])));
          }
        }
      }
    }

    output.data()[linearIndex] = ScalarConvert<AccT, T>::to(value);
  }
}

template <typename T, typename AccT, typename IndexType, int kSize, int stride>
__global__ void spatialDepthwiseConvolutionUpdateGradInput(
    const THCDeviceTensor<T, 4> gradOutput,
    THCDeviceTensor<T, 4> gradInput,
    const THCDeviceTensor<T, 4> weight,
    IndexType totalElements,
    const int inputChannels,
    const int depthwiseMultiplier,
    const int outputChannels,
    const int inputWidth, const int inputHeight,
    const int outputWidth, const int outputHeight,
    const int kernelWidth, const int kernelHeight,
    const int strideWidth, const int strideHeight,
    const int padWidth, const int padHeight,
    const int dilationWidth, const int dilationHeight)
{
  const int KW_LIMIT = (kSize !=0) ? kSize : kernelWidth;
  const int KH_LIMIT = (kSize !=0) ? kSize : kernelHeight;
  const int strideW = (stride !=0) ? stride : strideWidth;
  const int strideH = (stride !=0) ? stride : strideHeight;

  for (IndexType linearIndex = blockIdx.x * blockDim.x + threadIdx.x;
       linearIndex < totalElements;
       linearIndex += gridDim.x * blockDim.x) {

    int indtmp1 = linearIndex/inputWidth;
    const int w = linearIndex - indtmp1 * inputWidth;
    int indtmp2 = indtmp1/inputHeight;
    const int h = indtmp1 - indtmp2 * inputHeight;
    indtmp1 = indtmp2;
    indtmp2 = indtmp1/inputChannels;
    const int c = indtmp1 - indtmp2 * inputChannels;
    const int n = indtmp2;

    AccT value = ScalarConvert<int, AccT>::to(0);

#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
    for (int multiplier = 0; multiplier < depthwiseMultiplier; ++multiplier) {
      int och = (c * depthwiseMultiplier) + multiplier;
      int weightOffset = och * kernelHeight * kernelWidth;
#ifndef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
      for (int kh = 0; kh < KH_LIMIT; ++kh) {
#ifdef __HIP_PLATFORM_HCC__
#pragma unroll
#endif
        for (int kw = 0; kw < KW_LIMIT; ++kw) {
          int h_out = h + padHeight - kh * dilationHeight;
          int w_out = w + padWidth - kw * dilationWidth;
          if ((h_out % strideH == 0) && (w_out % strideW == 0)) {
            h_out = h_out / strideH;
            w_out = w_out / strideW;

            if ((h_out >= 0) && (h_out < outputHeight)
                  && (w_out >= 0) && (w_out < outputWidth)) {

              const int offset = ((n * outputChannels + och) * outputHeight + h_out)
                    * outputWidth + w_out;
              value = THCNumerics<AccT>::add(
                value,
                THCNumerics<AccT>::mul(
                  ScalarConvert<T, AccT>::to(weight.data()[weightOffset]),
                  ScalarConvert<T, AccT>::to(gradOutput.data()[offset])));
            }
          }
          ++weightOffset;
        }
      }
    }
    gradInput.data()[linearIndex] = ScalarConvert<AccT, T>::to(value);
  }
}


template <typename T, typename AccT, typename IndexType>
__global__ void spatialDepthwiseConvolutionAccGradParameters(
    const THCDeviceTensor<T, 4> gradOutput,
    const THCDeviceTensor<T, 4> input,
    THCDeviceTensor<T, 4> gradWeight,
    const int batchSize,
    const int inputChannels,
    const int kernelChannels,
    const int depthwiseMultiplier,
    const int inputWidth, const int inputHeight,
    const int outputWidth, const int outputHeight,
    const int kernelWidth, const int kernelHeight,
    const int strideWidth, const int strideHeight,
    const int padWidth, const int padHeight,
    const int dilationWidth, const int dilationHeight)
{
  const int channelStride = kernelWidth * kernelHeight;

  // Have to use a statically typed Shared Memory pointer
  SharedMem<AccT> smem;

  // Each Block is responsible for accumulating over a permutation of
  // (channels x kH x kW), use blockIdx to determine which one
  int bidx = blockIdx.x;
  int kW = bidx % kernelWidth;
  int kH = (bidx / kernelWidth) % kernelHeight;
  int ch = (bidx / channelStride);

  // Need to calculate which input channel is associated with this filter
  // channel
  int inputCh = ch / depthwiseMultiplier;

  AccT grad = ScalarConvert<float, AccT>::to(0.0);

  const int laneId = threadIdx.x % WARP_SIZE;
  const int batch = threadIdx.x / WARP_SIZE;
  const int nwarps = blockDim.x / WARP_SIZE;
  const int imageElements = outputWidth * outputHeight;
  // Use warp per item.  In the original kernel, a threadblock was used to sum over NHW.
  // Here, we use a warp to sum values over HW dimension, and if batchSize is larger than the
  // number of warps, a warp would loop over remaining batch items (e.g. if there are 8 warps,
  // warp 0 would go over 0-8-16 etc image, warp 1 over 1-9-17 etc). Later in blockReduce,
  // all the warps will be reduced anyway, thus the full reduction will be over NHW, like it
  // should be. That allows to get rid of one modulo operation inside the loop (because n/batchIdx
  // now does not have to be computed through modulo, you are just looping over it), and
  // bring a nice speed-up.
  for (int batchIdx = batch; batchIdx < batchSize; batchIdx += nwarps){
    // Warp-stride loop over elements in a batch item
    for (IndexType idx = laneId; idx < imageElements; idx += WARP_SIZE) {
    // Need to calculate the following: batch position, and offset into the gradOutput
    // in height, and width. We can intuit the corresponding position in the input from
    // the other parameters we have
      int go_w_offset = idx % outputWidth;
      int go_h_offset = (idx / outputWidth);

      int i_w_offset = (go_w_offset * strideWidth) + (kW * dilationWidth) - padWidth;
      int i_h_offset = (go_h_offset * strideHeight) + (kH * dilationHeight) - padHeight;

      if (i_w_offset >= 0 && i_h_offset >= 0 && i_w_offset < inputWidth && i_h_offset < inputHeight) {
        int inputOffset = ((batchIdx * inputChannels + inputCh) * inputHeight + i_h_offset) * inputWidth + i_w_offset;
        int outputOffset = ((batchIdx * kernelChannels + ch) * outputHeight ) * outputWidth + idx;
        grad = THCNumerics<AccT>::add(
            grad,
            THCNumerics<AccT>::mul(
              ScalarConvert<T, AccT>::to(input.data()[inputOffset]),
              ScalarConvert<T, AccT>::to(gradOutput.data()[outputOffset])));
      }
    }
  }
  __syncthreads();

  // At this point each thread in the block has a local gradient, which we need to
  // accumulate prior to writing the global value
  AccT *buf = smem.getPointer();
  AccT tval = reduceBlock<AccT, ReduceAdd<AccT>>(
      buf, blockDim.x, grad, ReduceAdd<AccT>(), ScalarConvert<float, AccT>::to(0));

  // After reduction, first thread in the block has the gradient, so its responsible
  // for writing it to gradWeight
  if (threadIdx.x == 0) {
    int weightOffset = kW + (kernelWidth * kH) + (kernelWidth * kernelHeight * ch);
    gradWeight.data()[weightOffset] = ScalarConvert<AccT, T>::to(tval);
  }
}

#include <THCUNN/generic/SpatialDepthwiseConvolution.cu>
#include <THC/THCGenerateFloatTypes.h>
