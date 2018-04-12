/*
 *Copyright (c) 2018 Intel Corporation.
 *
 *Permission is hereby granted, free of charge, to any person obtaining a copy
 *of this software and associated documentation files (the "Software"), to deal
 *in the Software without restriction, including without limitation the rights
 *to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *copies of the Software, and to permit persons to whom the Software is
 *furnished to do so, subject to the following conditions:
 *
 *The above copyright notice and this permission notice shall be included in
 *all copies or substantial portions of the Software.
 *
 *THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *THE SOFTWARE.
 *
 */


#ifndef _BN_PY_H_
#define _BN_PY_H_

#include <vector>
#include <memory>
#include <omp.h>
#include "mdarray.h"
#include "mkl_vml_functions.h"
#include "ideep.hpp"

class batchNormalization {
public:
  using tensor = ideep::tensor;
  using format = ideep::format;
  using alloc = ideep::utils::allocator;
  using scratch_allocator = ideep::utils::scratch_allocator;
  using descriptor = ideep::param::descriptor;
  using batch_normalization_forward_training = ideep::batch_normalization_forward_training;
  using batch_normalization_forward_inference = ideep::batch_normalization_forward_inference;
  using batch_normalization_backward = ideep::batch_normalization_backward;

  static std::vector<mdarray> Forward(mdarray *src,
                                      mdarray *scale,
                                      mdarray *shift,
                                      mdarray *mean,
                                      mdarray *variance,
                                      float eps) {
    std::vector<mdarray> outs;

    if (mean) {
      auto dst_ = batch_normalization_forward_inference::compute<scratch_allocator>(
		    *src->get(), *mean->get(), *variance->get(),
            *scale->get(), *shift->get(), eps);

      outs.push_back(mdarray(dst_));
    } else {
      auto tensors = batch_normalization_forward_training::compute<scratch_allocator>(
		     *src->get(), *scale->get(), *shift->get(), 0, eps);

      auto dst_ = std::get<0>(tensors);
      auto mean_ = std::get<1>(tensors);
      auto variance_ = std::get<2>(tensors);

      tensor inv_;
      inv_.init<scratch_allocator, batch_normalization_forward_training>(
                {variance_.get_dims(), src->get()->get_data_type(),
                descriptor::public_compatible_format(variance_.get_descriptor())});

      batch_normalization_inv((float *)variance_.get_data_handle(), eps,
                              variance_.get_nelems(), (float *)inv_.get_data_handle());

      outs.push_back(mdarray(dst_));
      outs.push_back(mdarray(mean_));
      outs.push_back(mdarray(variance_));
      outs.push_back(mdarray(inv_));
    }

    return outs;
  }

  static std::vector<mdarray> Backward(mdarray *src, mdarray *grady,
                                       mdarray *mean, mdarray *variance,
                                       mdarray *scale, float eps) {
    std::vector<mdarray> outs;

    tensor scale_ = *scale->get();
    // For old framework compatibility
    // deprecated in new framewrok.
    if (scale_.ndims() == 2) {
      // scale is weights (scale + shift).
      // cut half to scale.
      scale_.init({{scale_.get_nelems() / 2}, scale_.get_data_type(), format::x},
                  scale_.get_data_handle());
    }

    auto tensors = batch_normalization_backward::compute<scratch_allocator>(*src->get(),
                       *mean->get(), *variance->get(), *grady->get(), scale_, eps);

    outs.push_back(mdarray(std::get<0>(tensors)));
    outs.push_back(mdarray(std::get<1>(tensors)));

    return outs;
  }

  // Deprecated:
  // use above forward API instead.
  static std::vector<mdarray> Forward(mdarray *src,
                                      mdarray *weights,
                                      mdarray *mean,
                                      mdarray *variance,
                                      float eps) {
    tensor scale;
    tensor shift;
    std::vector<mdarray> outs;
    tensor weights_ = *weights->get();

    scale.init({{weights_.get_nelems() / 2},
                weights_.get_data_type(), format::x},
                weights_.get_data_handle());
    shift.init({{weights_.get_nelems() / 2},
                weights_.get_data_type(), format::x},
                (char *)weights_.get_data_handle() + weights_.get_size() / 2);

    if (mean) {
      auto dst_ = batch_normalization_forward_inference::compute<scratch_allocator>(
		  *src->get(), *mean->get(), *variance->get(), scale, shift, eps);

      outs.push_back(mdarray(dst_));
    } else {
      auto tensors = batch_normalization_forward_training::compute<scratch_allocator>(
		     *src->get(), scale, shift, 0, eps);

      auto dst_ = std::get<0>(tensors);
      auto mean_ = std::get<1>(tensors);
      auto variance_ = std::get<2>(tensors);

      tensor inv_;
      inv_.init<scratch_allocator, batch_normalization_forward_training>(
                {variance_.get_dims(), src->get()->get_data_type(),
                descriptor::public_compatible_format(variance_.get_descriptor())});

      batch_normalization_inv((float *)variance_.get_data_handle(), eps,
                              variance_.get_nelems(), (float *)inv_.get_data_handle());

      outs.push_back(mdarray(dst_));
      outs.push_back(mdarray(mean_));
      outs.push_back(mdarray(variance_));
      outs.push_back(mdarray(inv_));
    }

    return outs;
  }

private:
  static void batch_normalization_inv(float *var, float eps, int size, float *inv) {
    int blk_nthr = omp_get_max_threads(),
      blk_num = blk_nthr,
      blk_len = size / blk_num,
      blk_len_ex = size % blk_num;

    if (!blk_len)
      blk_nthr = size;

    float *var_eps = reinterpret_cast<float *>(alloc::malloc(size * sizeof(float)));

    # pragma omp parallel num_threads(blk_nthr)
    {
      int ithr = omp_get_thread_num();
      int blen = ithr < blk_len_ex ? blk_len + 1 : blk_len;
      int bstart = ithr <= blk_len_ex ? (blk_len + 1) * ithr :
          blk_len_ex * (blk_len + 1) + (ithr - blk_len_ex) * blk_len;
      int bend = bstart + blen;

      for (int b = bstart; b < bend; b++)
        var_eps[b] = var[b] + eps;
    }

    vsPowx(size, var_eps, -0.5, inv);

    alloc::free(reinterpret_cast<char *>(var_eps));
  }
};

#endif // _BN_PY_H_
