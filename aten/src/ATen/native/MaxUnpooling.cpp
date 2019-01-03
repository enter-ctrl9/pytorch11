#include <tuple>
#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>

namespace at {
namespace native {

template <typename scalar_t>
Tensor max_unpooling2d_forward_out_cpu_frame(
    Tensor& output,
    const Tensor& input,
    const Tensor& indices,
    int64_t outputHeight,
    int64_t outputWidth) {
  int64_t numBatch = 1;
  int64_t dimc = 0;
  int64_t dimh = 1;
  int64_t dimw = 2;
  if (input.ndimension() == 4) {
    numBatch = input.size(0);
    dimc++;
    dimh++;
    dimw++;
  }
  int64_t numChannels = input.size(dimc);
  int64_t inputHeight = input.size(dimh);
  int64_t inputWidth = input.size(dimw);

  auto* rawInput = input.data<scalar_t>();
  auto* rawIndices = indices.data<int64_t>();
  auto* rawOutput = output.data<scalar_t>();

  for (int64_t n = 0; n < numBatch; n++) {
    int64_t nOutputOffset = n * numChannels * outputWidth * outputHeight;
    int64_t nInputOffset = n * numChannels * inputWidth * inputHeight;
    int64_t k;
    bool has_error = false;
    int64_t error_index = 0;
#pragma omp parallel for private(k)
    for (k = 0; k < numChannels; k++) {
      int64_t finalOutputOffset = nOutputOffset + k * outputWidth * outputHeight;
      int64_t finalInputOffset = nInputOffset + k * inputWidth * inputHeight;
      scalar_t* output_p_k = rawOutput + finalOutputOffset;
      scalar_t* input_p_k = rawInput + finalInputOffset;
      int64_t* ind_p_k = rawIndices + finalInputOffset;

      int64_t maxp;
      for (int64_t i = 0; i < inputHeight; i++) {
        for (int64_t j = 0; j < inputWidth; j++) {
          maxp = ind_p_k[i * inputWidth + j];
          if (maxp < 0 || maxp >= outputWidth * outputHeight) {
#pragma omp critical
            {
              has_error = true;
              error_index = maxp;
            }
          } else {
            output_p_k[maxp] = input_p_k[i * inputWidth + j];
          }
        }
      }
    }
    if (has_error) {
      AT_ERROR(
          "Found an invalid max index",
          error_index,
          " (output volumes are of size ",
          outputHeight,
          "x",
          outputWidth);
    }
  }
  return output;
}

Tensor& max_unpooling2d_forward_out_cpu(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  AT_CHECK(
      indices.scalar_type() == at::ScalarType::Long,
      "elements in indices should be type Long");
  AT_CHECK(
      output_size.size() == 2,
      "There should be exactly two elements (height, width) in output_size");
  AT_CHECK(
      (self.ndimension() == 3 || self.ndimension() == 4),
      "Input to max_unpooling2d should be a 3d or 4d Tensor");
  AT_CHECK(
      self.sizes() == indices.sizes(),
      "Shape of indices should match shape of input");

  AT_CHECK(self.numel() > 0, "Input must be non-empty");

  auto outputHeight = output_size[0];
  auto outputWidth = output_size[1];

  auto self_contiguous = self.contiguous();
  auto indices_contiguous = indices.contiguous();

  if (self_contiguous.ndimension() == 3) {
    int64_t numBatch = 1;
    int64_t numChannels = self.size(0);
    output.resize_({numChannels, outputHeight, outputWidth});
  } else {
    int64_t numBatch = self.size(0);
    int64_t numChannels = self.size(1);
    output.resize_({numBatch, numChannels, outputHeight, outputWidth});
  }
  output.zero_();

  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "max_unpooling2d_forward_out_cpu_frame", ([&] {
        max_unpooling2d_forward_out_cpu_frame<scalar_t>(
            output,
            self_contiguous,
            indices_contiguous,
            output_size[0],
            output_size[1]);
      }));
  return output;
};

Tensor max_unpooling2d_forward_cpu(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  auto output = at::empty(
      {0},
      self.options());
  max_unpooling2d_forward_out_cpu(output, self, indices, output_size);
  return output;
}

template <typename scalar_t>
Tensor max_unpooling3d_forward_out_cpu_frame(
    Tensor& output,
    const Tensor& input,
    const Tensor& indices,
    int64_t oT,
    int64_t oW,
    int64_t oH,
    int64_t dT,
    int64_t dW,
    int64_t dH,
    int64_t pT,
    int64_t pW,
    int64_t pH) {
  int64_t nBatch = 1;
  int64_t dimw = 3;
  int64_t dimh = 2;
  int64_t dimt = 1;

  if (input.ndimension() == 5) {
    nBatch = input.size(0);
    dimw++;
    dimh++;
    dimt++;
  }

  int64_t nSlices = input.size(dimt - 1);
  int64_t iT = input.size(dimt);
  int64_t iH = input.size(dimh);
  int64_t iW = input.size(dimw);

  scalar_t* input_data = input.data<scalar_t>();
  scalar_t* output_data = output.data<scalar_t>();
  int64_t* indices_data = indices.data<int64_t>();

  for (int64_t p = 0; p < nBatch; p++) {
    int64_t inputOffset = p * nSlices * iT * iW * iH;
    int64_t outputOffset = p * nSlices * oT * oW * oH;
    int64_t k;
    bool has_error = false;
    int error_index = 0;
#pragma omp parallel for private(k)
    for (k = 0; k < nSlices; k++) {
      int64_t finalInputOffset = inputOffset + k * iT * iW * iH;
      int64_t finalOutputOffset = outputOffset + k * oT * oW * oH;

      scalar_t* output_p_k = output_data + finalOutputOffset;
      scalar_t* input_p_k = input_data + finalInputOffset;
      int64_t* ind_p_k = indices_data + finalInputOffset;
      int maxp;
      for (int64_t t = 0; t < iT; t++) {
        for (int64_t i = 0; i < iH; i++) {
          for (int64_t j = 0; j < iW; j++) {
            int64_t index = t * iH * iW + i * iW + j;
            maxp = ind_p_k[index];
            if (maxp < 0 || maxp >= oT * oW * oH) {
#pragma omp critical
              {
                has_error = true;
                error_index = maxp;
              }
            } else {
              output_p_k[maxp] = input_p_k[index];
            }
          }
        }
      }
      if (has_error) {
        AT_ERROR(
            "found an invalid max index %ld (output volumes are of size %dx%dx%d)",
            error_index,
            oT,
            oH,
            oW);
      }
    }
  }
  return output;
}

void max_unpooling3d_shape_check(
    const Tensor& input,
    const Tensor& gradOutput,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding,
    bool check_grad) {

  AT_CHECK(input.numel() > 0, "Input must be non-empty");
  AT_CHECK(
      (input.ndimension() == 4 || input.ndimension() == 5),
      "Input must be 4d or 5d tensor");
  AT_CHECK(input.sizes() == indices.sizes());
  AT_CHECK(
      stride[0] > 0 && stride[1] > 0 && stride[2] > 0,
      "stride should be never greater than zero, but got stride: ",
      stride);

  int dimw = 3;
  int dimh = 2;
  int dimt = 1;
  int dimn = 0;

  if (input.ndimension() == 5) {
    dimw++;
    dimh++;
    dimt++;
    dimn++;
  }

  int nslices = input.size(dimn);
  if (check_grad) {
    if (output_size[0] != gradOutput.size(dimt) ||
        output_size[1] != gradOutput.size(dimh) ||
        output_size[2] != gradOutput.size(dimw)) {
      AT_ERROR(
          "Inconsistent gradOutput size. output_size: ,",
          output_size,
          ". gradOutput: ",
          gradOutput);
    }
    AT_CHECK(
        gradOutput.ndimension() == input.ndimension() &&
        gradOutput.size(dimn) == nslices);
  }
}

Tensor& max_unpooling3d_forward_out_cpu(
    Tensor& output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  AT_CHECK(
      indices.scalar_type() == at::ScalarType::Long,
      "elements in indices should be type Long");
  AT_CHECK(
      (self.ndimension() == 4 || self.ndimension() == 5),
      "Input to max_unpooling3d should be a 4d or 5d Tensor",
      self.sizes());
  AT_CHECK(
      output_size.size() == 3,
      "There should be exactly three elements (depth, height, width) in output_size");
  AT_CHECK(
      stride.size() == 3,
      "There should be exactly three elements (depth, height, width) in stide");
  AT_CHECK(
      padding.size() == 3,
      "There should be exactly three elements (depth, height, width) in padding");
  AT_CHECK(
      self.sizes() == indices.sizes(),
      "Shape of indices should match shape of input");
  max_unpooling3d_shape_check(
      self, at::empty({}), indices, output_size, stride, padding, false);

  if (self.ndimension() == 5) {
    output.resize_({self.size(0),
                    self.size(1),
                    output_size[0],
                    output_size[1],
                    output_size[2]});
  } else {
    output.resize_(
        {self.size(0), output_size[0], output_size[1], output_size[2]});
  }
  output.zero_();

  AT_DISPATCH_FLOATING_TYPES(
      self.type(), "max_unpooling3d_forward_out_cpu_frame", ([&] {
        max_unpooling3d_forward_out_cpu_frame<scalar_t>(
            output,
            self.contiguous(),
            indices.contiguous(),
            output_size[0],
            output_size[1],
            output_size[2],
            stride[0],
            stride[1],
            stride[2],
            padding[0],
            padding[1],
            padding[2]);
      }));
  return output;
}

Tensor max_unpooling3d_forward_cpu(
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  AT_CHECK(
      (self.ndimension() == 4 || self.ndimension() == 5),
      "Input to max_unpooling2d should be a 4d or 5d Tensor",
      self.sizes());
  AT_CHECK(
      output_size.size() == 3,
      "There should be exactly three elements (depth, height, width) in output_size");
  auto output = at::empty(
      {0},
      self.options());
  max_unpooling3d_forward_out_cpu(
      output, self, indices, output_size, stride, padding);
  return output;
}

template <typename scalar_t>
static void max_unpooling2d_backward_out_cpu_frame(
    scalar_t* gradInput_p,
    scalar_t* gradOutput_p,
    int64_t* ind_p,
    int64_t nslices,
    int64_t iwidth,
    int64_t iheight,
    int64_t owidth,
    int64_t oheight) {
  int k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++) {
    scalar_t* gradInput_p_k = gradInput_p + k * iwidth * iheight;
    scalar_t* gradOutput_p_k = gradOutput_p + k * owidth * oheight;
    int64_t* ind_p_k = ind_p + k * iwidth * iheight;

    int64_t i, j;
    int64_t maxp;
    for (i = 0; i < iheight; i++) {
      for (j = 0; j < iwidth; j++) {
        maxp = ind_p_k[i * iwidth + j]; /* retrieve position of max */
        if (maxp < 0 || maxp >= owidth * oheight) {
          AT_ERROR(
              "invalid max index ",
              maxp,
              ", owidth= ",
              owidth,
              ", oheight= ",
              oheight);
        }
        gradInput_p_k[i * iwidth + j] =
            gradOutput_p_k[maxp]; /* update gradient */
      }
    }
  }
}

Tensor& max_unpooling2d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  int dimw = 2;
  int dimh = 1;
  int nbatch = 1;
  int nslices;
  int iheight;
  int iwidth;
  AT_CHECK(
      indices.scalar_type() == at::ScalarType::Long,
      "elements in indices should be type Long");
  AT_CHECK(
      self.sizes() == indices.sizes(), "Input shape must match indices shape");

  AT_CHECK(output_size.size() == 2, "Output size must be 2");

  int owidth = output_size[0];
  int oheight = output_size[1];

  /* get contiguous gradOutput and indices */
  auto gradOutput = grad_output.contiguous();
  auto indicesContiguous = indices.contiguous();

  /* resize */
  grad_input.resize_as_(self);
  grad_input.zero_();

  if (self.ndimension() == 4) {
    nbatch = self.size(0);
    dimw++;
    dimh++;
  }

  /* sizes */
  nslices = self.size(dimh - 1);
  iheight = self.size(dimh);
  iwidth = self.size(dimw);

  if (output_size[0] != gradOutput.size(dimh) ||
      output_size[1] != gradOutput.size(dimw)) {
    AT_ERROR(
        "Inconsistent gradOutput size",
        output_size[0],
        ", output width= ",
        output_size[1],
        ", gradOutput: ",
        gradOutput.size(dimh),
        "x",
        gradOutput.size(dimw));
  }

  int p;
  for (p = 0; p < nbatch; p++) {
    auto inputOffset = p * nslices * iheight * iwidth;
    auto outputOffset = p * nslices * oheight * owidth;
    AT_DISPATCH_FLOATING_TYPES(
        self.type(), "max_unpooling2d_backward_out_cpu_frame", ([&] {
          max_unpooling2d_backward_out_cpu_frame<scalar_t>(
              grad_input.data<scalar_t>() + inputOffset,
              gradOutput.data<scalar_t>() + outputOffset,
              indices.data<int64_t>() + inputOffset,
              nslices,
              iwidth,
              iheight,
              output_size[0],
              output_size[1]);
        }));
  }
  return grad_input;
}

Tensor max_unpooling2d_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size) {
  auto grad_input = at::empty_like(self);
  max_unpooling2d_backward_out_cpu(
      grad_input, grad_output, self, indices, output_size);
  return grad_input;
}

template <typename scalar_t>
static void max_unpooling3d_backward_out_cpu_frame(
    scalar_t* gradInput_p,
    scalar_t* gradOutput_p,
    int64_t* ind_p,
    int64_t nslices,
    int64_t iT,
    int64_t iW,
    int64_t iH,
    int64_t oT,
    int64_t oW,
    int64_t oH) {
  int k;
#pragma omp parallel for private(k)
  for (k = 0; k < nslices; k++) {
    scalar_t* gradInput_p_k = gradInput_p + k * iT * iH * iW;
    scalar_t* gradOutput_p_k = gradOutput_p + k * oT * oH * oW;
    int64_t* ind_p_k = ind_p + k * iT * iH * iW;

    int t, i, j, index;
    int64_t maxp;
    for (t = 0; t < iT; t++) {
      for (i = 0; i < iH; i++) {
        for (j = 0; j < iW; j++) {
          index = t * iH * iW + i * iW + j;
          maxp = ind_p_k[index]; /* retrieve position of max */
          if (maxp < 0 || maxp >= oT * oH * oW) {
            AT_ERROR(
                "invalid max index ",
                maxp,
                ", oT= ",
                oT,
                ", oW= ",
                oW,
                ",oH= ",
                oH);
          }
          gradInput_p_k[index] = gradOutput_p_k[maxp]; /* update gradient */
        }
      }
    }
  }
}
Tensor& max_unpooling3d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  AT_CHECK(
      indices.scalar_type() == at::ScalarType::Long,
      "elements in indices should be type Long");
  AT_CHECK(
      output_size.size() == 3,
      "There should be exactly three elements (depth, height, width) in output_size");
  AT_CHECK(
      stride.size() == 3,
      "There should be exactly three elements (depth, height, width) in stide");
  AT_CHECK(
      padding.size() == 3,
      "There should be exactly three elements (depth, height, width) in padding");
  auto oT = output_size[0];
  auto oH = output_size[1];
  auto oW = output_size[2];
  int dimw = 3;
  int dimh = 2;
  int dimt = 1;
  int nbatch = 1;
  int nslices;
  int iT;
  int iH;
  int iW;

  max_unpooling3d_shape_check(
      self, grad_output, indices, output_size, stride, padding, true);

  // TODO (from THNN): check gradOutput shape
  /* get contiguous gradOutput */
  auto grad_output_contiguous = grad_output.contiguous();
  auto indices_contiguous = indices.contiguous();

  /* resize */
  grad_input.resize_as_(self);
  grad_input.zero_();
  if (self.ndimension() == 5) {
    nbatch = self.size(0);
    dimt++;
    dimw++;
    dimh++;
  }

  /* sizes */
  nslices = self.size(dimt - 1);
  iT = self.size(dimt);
  iH = self.size(dimh);
  iW = self.size(dimw);

  /* backprop */
  int p;
  for (p = 0; p < nbatch; p++) {
    int inputOffset = p * nslices * iT * iH * iW;
    int outputOffset = p * nslices * oT * oT * oW;
    AT_DISPATCH_FLOATING_TYPES(
        self.type(), "max_unpooling3d_backward_out_cpu_frame", ([&] {
          max_unpooling3d_backward_out_cpu_frame<scalar_t>(
              grad_input.data<scalar_t>() + inputOffset,
              grad_output_contiguous.data<scalar_t>() + outputOffset,
              indices_contiguous.data<int64_t>() + inputOffset,
              nslices,
              iT,
              iW,
              iH,
              oT,
              oW,
              oH);
        }));
  }
  return grad_input;
}

Tensor max_unpooling3d_backward_cpu(
    const Tensor& grad_output,
    const Tensor& self,
    const Tensor& indices,
    IntList output_size,
    IntList stride,
    IntList padding) {
  auto grad_input = at::empty_like(self);
  max_unpooling3d_backward_out_cpu(
      grad_input, grad_output, self, indices, output_size, stride, padding);
  return grad_input;
}
} // namespace native
} // namespace at
