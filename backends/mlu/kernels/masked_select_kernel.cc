// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "kernels/funcs/mlu_baseop.h"
#include "kernels/funcs/mlu_funcs.h"
#include "kernels/funcs/reduce_op.h"

namespace custom_kernel {

template <typename T, typename Context>
void MaskedSelectKernel(const Context& dev_ctx,
                        const phi::DenseTensor& x,
                        const phi::DenseTensor& mask,
                        phi::DenseTensor* out) {
  auto input_dim = x.dims();
  auto mask_dim = mask.dims();
  PADDLE_ENFORCE_EQ(input_dim,
                    mask_dim,
                    phi::errors::InvalidArgument(
                        "The dim size of input and mask in OP(masked_selected) "
                        "must be equal, but got input dim:(%ld), mask dim: "
                        "(%ld). Please check input "
                        "value.",
                        input_dim,
                        mask_dim));

  Tensor number;
  number.Resize({1});
  void* number_ptr = dev_ctx.template Alloc<int32_t>(&number);
  out->Resize(mask.dims());
  dev_ctx.template Alloc<T>(out);
  MLUCnnlTensorDesc input_desc(x);
  MLUCnnlTensorDesc mask_desc(mask);
  MLUCnnlTensorDesc out_desc(*out);
  MLUCnnl::Mask(dev_ctx,
                CNNL_MASKED_SELECT,
                input_desc.get(),
                GetBasePtr(&x),
                mask_desc.get(),
                GetBasePtr(&mask),
                nullptr, /* value_desc */
                nullptr, /* value */
                nullptr, /* scale */
                out_desc.get(),
                GetBasePtr(out),
                static_cast<uint32_t*>(number_ptr));
}

template <typename T, typename Context>
void MaskedSelectGradKernel(const Context& dev_ctx,
                            const phi::DenseTensor& x,
                            const phi::DenseTensor& mask,
                            const phi::DenseTensor& out_grad,
                            phi::DenseTensor* x_grad) {
  C_Stream stream = static_cast<C_Stream>(dev_ctx.stream());
  Tensor mask_tensor, mask_valid_num_tensor;
  std::vector<int32_t> mask_valid_num_vec;
  mask_tensor.Resize({mask.dims()});
  dev_ctx.template Alloc<int32_t>(&mask_tensor);
  mask_valid_num_tensor.Resize({1});
  dev_ctx.template Alloc<int32_t>(&mask_valid_num_tensor);

  if (mask.dtype() == DataType::BOOL) {  // cast mask to int32 dtype
    MLUCnnlTensorDesc mask_desc(mask);
    MLUCnnlTensorDesc mask_int32_desc(mask_tensor);
    auto cast_type = GetCastDataType(mask.dtype(), DataType::INT32);
    MLUCnnl::Cast(dev_ctx,
                  cast_type,
                  mask_desc.get(),
                  GetBasePtr(&mask),
                  mask_int32_desc.get(),
                  GetBasePtr(&mask_tensor));
  } else {
    mask_tensor = mask;
  }

  // get valid element num from mask tensor (where mask_i == 1)
  std::vector<int64_t> reduce_dims_vec;
  std::string reduce_name = "reduce_sum";
  MLUReduceOp<int32_t, Context>(dev_ctx,
                                mask_tensor,
                                reduce_dims_vec,
                                false, /* keep_dim */
                                true,  /* reduce_all */
                                reduce_name,
                                &mask_valid_num_tensor);

  dev_ctx.Wait();
  TensorToVector<int32_t>(
      dev_ctx, mask_valid_num_tensor, dev_ctx, &mask_valid_num_vec);

  VLOG(3) << "[MaskedSelectGradKernel] valid mask num "
          << mask_valid_num_vec[0];
  VLOG(3) << "[MaskedSelectGradKernel] numel of mask_tensor "
          << mask_tensor.numel();

  // get mask indice
  Tensor topk_v2_out, mask_indices;
  mask_tensor.Resize({mask_tensor.numel()});
  topk_v2_out.Resize({mask_tensor.numel()});
  mask_indices.Resize({mask_tensor.numel()});
  dev_ctx.template Alloc<int32_t>(&topk_v2_out);
  dev_ctx.template Alloc<int32_t>(&mask_indices);

  MLUCnnlTensorDesc topk_v2_out_desc(topk_v2_out);
  MLUCnnlTensorDesc mask_indices_desc(mask_indices);
  MLUCnnlTensorDesc mask_tensor_desc(mask_tensor);

  const int dim = 0;
  MLUCnnl::TopK(dev_ctx,
                mask_tensor.numel(),
                dim,
                true,
                false,
                mask_tensor_desc.get(),
                GetBasePtr(&mask_tensor),
                topk_v2_out_desc.get(),
                GetBasePtr(&topk_v2_out),
                mask_indices_desc.get(),
                GetBasePtr(&mask_indices));

  // copy out valid mask indices
  Tensor valid_mask_indices;
  valid_mask_indices.Resize({mask_valid_num_vec[0]});
  dev_ctx.template Alloc<int32_t>(&valid_mask_indices);
  PADDLE_ENFORCE_GE(mask_tensor.numel(),
                    mask_valid_num_vec[0],
                    phi::errors::InvalidArgument(
                        "Numel of mask tensor should be greater or equal to "
                        "the number of valid mask indices. "
                        "But received: the numel of mask_tensor is [%d], and "
                        "valid_mask_num is [%d]",
                        mask_tensor.numel(),
                        mask_valid_num_vec[0]));
  AsyncMemCpyD2D(nullptr,
                 stream,
                 GetBasePtr(&valid_mask_indices),
                 GetBasePtr(&mask_indices),
                 mask_valid_num_vec[0] * sizeof(int32_t));

  Tensor out_grad_tmp_out;
  out_grad_tmp_out.Resize({mask_valid_num_vec[0]});
  dev_ctx.template Alloc<T>(&out_grad_tmp_out);
  MLUCnnlTensorDesc out_grad_tmp_out_desc(out_grad_tmp_out);
  AsyncMemCpyD2D(nullptr,
                 stream,
                 GetBasePtr(&out_grad_tmp_out),
                 GetBasePtr(&out_grad),
                 mask_valid_num_vec[0] * sizeof(T));
  dev_ctx.Wait();

  Tensor indices_int32_tmp;
  indices_int32_tmp = valid_mask_indices;
  indices_int32_tmp.Resize({mask_valid_num_vec[0], 1});
  MLUCnnlTensorDesc indices_int32_tmp_desc(indices_int32_tmp);

  VLOG(3) << "[MaskedSelectGradKernel] before ScatterNd";

  const cnnlScatterNdMode_t mode = CNNL_SCATTERND_UPDATE;
  x_grad->Resize({x_grad->numel()});
  dev_ctx.template Alloc<T>(x_grad);
  MLUCnnlTensorDesc x_grad_desc(*x_grad);
  MLUCnnl::ScatterNd(dev_ctx,
                     mode,
                     indices_int32_tmp_desc.get(),
                     GetBasePtr(&indices_int32_tmp),
                     out_grad_tmp_out_desc.get(),
                     GetBasePtr(&out_grad_tmp_out),
                     nullptr,
                     nullptr,
                     x_grad_desc.get(),
                     GetBasePtr(x_grad));
  VLOG(3) << "[MaskedSelectGradKernel] after ScatterNd";
  x_grad->Resize(mask.dims());
}

}  // namespace custom_kernel

PD_REGISTER_PLUGIN_KERNEL(masked_select,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::MaskedSelectKernel,
                          phi::dtype::float16,
                          float,
                          int) {}

PD_REGISTER_PLUGIN_KERNEL(masked_select_grad,
                          CustomMLU,
                          ALL_LAYOUT,
                          custom_kernel::MaskedSelectGradKernel,
                          phi::dtype::float16,
                          float,
                          int) {}
