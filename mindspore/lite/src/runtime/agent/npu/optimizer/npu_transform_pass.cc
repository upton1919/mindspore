/**
 * Copyright 2020 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "src/runtime/agent/npu/optimizer/npu_transform_pass.h"
#include <vector>
#include "src/lite_kernel.h"
#include "src/runtime/agent/npu/npu_manager.h"
#include "src/runtime/agent/npu/optimizer/npu_pass_utils.h"
namespace mindspore::lite {
using kernel::KERNEL_ARCH::kCPU;
using kernel::KERNEL_ARCH::kNPU;
int NPUTransformPass::UpdateNH2NCTransNodePreKernel(kernel::LiteKernel *kernel, kernel::LiteKernel *trans_kernel,
                                                    kernel::LiteKernel *after_kernel) {
  std::vector<kernel::LiteKernel *> out_kernels;

  for (auto out_kernel : kernel->out_kernels()) {
    if (out_kernel == after_kernel) {
      out_kernels.push_back(trans_kernel);
    } else {
      out_kernels.push_back(out_kernel);
    }
  }
  NPUPassUtils::UpdateKernel(kernel, kernel->in_kernels(), out_kernels, kernel->in_tensors(), kernel->out_tensors());
  return RET_OK;
}

int NPUTransformPass::UpdateNH2NCTransNodeAfterKernel(kernel::LiteKernel *kernel, kernel::LiteKernel *trans_kernel,
                                                      kernel::LiteKernel *before_kernel) {
  std::vector<lite::Tensor *> cur_kernel_in_tensors = {trans_kernel->out_tensors()[0]};
  for (int i = 1; i < kernel->in_tensors().size(); i++) {
    cur_kernel_in_tensors.push_back(kernel->in_tensors()[i]);
  }
  std::vector<kernel::LiteKernel *> cur_in_kernels = {trans_kernel};
  for (int i = 0; i < kernel->in_kernels().size(); i++) {
    auto in_kernel = kernel->in_kernels()[i];
    if (in_kernel != kernel) {
      cur_in_kernels.push_back(in_kernel);
    }
  }
  NPUPassUtils::UpdateKernel(kernel, cur_in_kernels, kernel->out_kernels(), cur_kernel_in_tensors,
                             kernel->out_tensors());
  return RET_OK;
}

int NPUTransformPass::InsertPreNode(const InnerContext *context, std::vector<kernel::LiteKernel *>::iterator it,
                                    std::vector<kernel::LiteKernel *> *all_kernels,
                                    std::vector<Tensor *> *all_tensors) {
  auto kernel = *it;
  bool is_input_kernel = kernel->in_kernels().empty();
  if (is_input_kernel || kernel->in_kernels()[0]->desc().arch != kNPU ||
      npu_trans_nodes.find(kernel->in_kernels()[0]->Type()) == npu_trans_nodes.end()) {
    kernel::LiteKernel *before_kernel = nullptr;
    if (!is_input_kernel) {
      before_kernel = kernel->in_kernels()[0];
    }
    // Create pre transform kernel out tensors.
    std::vector<int> shapes{kernel->in_tensors()[0]->shape()[0], kernel->in_tensors()[0]->shape()[3],
                            kernel->in_tensors()[0]->shape()[1], kernel->in_tensors()[0]->shape()[2]};
    auto tensor = new Tensor(kernel->in_tensors()[0]->data_type(), shapes, schema::Format_NCHW, Tensor::VAR);
    std::vector<Tensor *> pre_trans_out_tensors = {tensor};
    all_tensors->push_back(pre_trans_out_tensors[0]);
    // Replace the output tensor of the previous node
    auto name = kernel->name() + "_pre_trans" + "_Nhwc2Nchw_" + std::to_string(total++);
    auto *pre_trans_kernel =
      NPUPassUtils::CreateNhwc2NchwKernel({kernel->in_tensors()[0]}, pre_trans_out_tensors, context, name);
    // Insert Nhwc2Nchw into the front of the current queue
    all_kernels->push_back(pre_trans_kernel);
    insert_primitive_.push_back(pre_trans_kernel->GetPrimitive());
    // Replace the output kernel of the previous node
    std::vector<kernel::LiteKernel *> pre_trans_in_kernel;
    if (is_input_kernel) {
      pre_trans_in_kernel = {};
    } else {
      pre_trans_in_kernel = {before_kernel};
    }
    NPUPassUtils::UpdateKernel(pre_trans_kernel, pre_trans_in_kernel, {kernel}, {kernel->in_tensors()[0]},
                               pre_trans_out_tensors);

    if (before_kernel != nullptr) {
      UpdateNH2NCTransNodePreKernel(before_kernel, pre_trans_kernel, kernel);
    }
    UpdateNH2NCTransNodeAfterKernel(kernel, pre_trans_kernel, before_kernel);
  }
  return RET_OK;
}

int NPUTransformPass::InsertPostNode(const InnerContext *context, std::vector<kernel::LiteKernel *>::iterator it,
                                     std::vector<kernel::LiteKernel *> *all_kernels,
                                     std::vector<Tensor *> *all_tensors) {
  auto kernel = *it;
  // Model output does not insert operator
  if (kernel->out_kernels().empty()) {
    return RET_OK;
  }
  // Single output multiple references
  for (int i = 0; i < kernel->out_kernels().size(); i++) {
    auto next_kernel = kernel->out_kernels().at(i);
    if (next_kernel->desc().arch == kNPU && npu_trans_nodes.find(next_kernel->Type()) != npu_trans_nodes.end()) {
      continue;
    }
    // Change format the output of the current kernel nhwc->nchw
    auto shapes = {kernel->out_tensors()[0]->shape()[0], kernel->out_tensors()[0]->shape()[1],
                   kernel->out_tensors()[0]->shape()[2], kernel->out_tensors()[0]->shape()[3]};
    auto tensor = new Tensor(kernel->out_tensors()[0]->data_type(), shapes, schema::Format_NHWC, Tensor::VAR);
    std::vector<Tensor *> post_trans_out_tensors = {tensor};
    all_tensors->push_back(post_trans_out_tensors[0]);
    // Use the output tensor of the current node as the input tensor of the post-conversion operator
    auto name = kernel->name() + "_post_trans" + "_Nchw2Nhwc" + std::to_string(total++);
    auto *post_trans_kernel =
      NPUPassUtils::CreateNchw2NhwcKernel(kernel->out_tensors(), post_trans_out_tensors, context, name);
    // Replace the input tensor of the next node
    NPUPassUtils::UpdateKernel(post_trans_kernel, {kernel}, {next_kernel}, kernel->out_tensors(),
                               post_trans_out_tensors);
    insert_primitive_.push_back(post_trans_kernel->GetPrimitive());
    // Directly insert in the back, will not affect the topological sort
    all_kernels->push_back(post_trans_kernel);
    UpdateNC2NHTransNodePreKernel(kernel, post_trans_kernel, next_kernel);
    UpdateNC2NHTransNodeAfterKernel(kernel, post_trans_kernel, next_kernel);
  }
  return RET_OK;
}

int NPUTransformPass::UpdateNC2NHTransNodePreKernel(kernel::LiteKernel *kernel, kernel::LiteKernel *trans_kernel,
                                                    kernel::LiteKernel *next_kernel) {
  std::vector<kernel::LiteKernel *> cur_out_kernels;
  for (auto out_kernel : kernel->out_kernels()) {
    if (out_kernel == next_kernel) {
      cur_out_kernels.push_back(trans_kernel);
    } else {
      cur_out_kernels.push_back(out_kernel);
    }
  }
  auto kernel_out_tensor = kernel->out_tensors()[0];
  // Change format the output of the current kernel nhwc->nchw
  std::vector<int> kernel_out_new_shapes = {kernel_out_tensor->shape()[0], kernel_out_tensor->shape()[3],
                                            kernel_out_tensor->shape()[1], kernel_out_tensor->shape()[2]};
  kernel_out_tensor->set_format(schema::Format_NCHW);
  kernel_out_tensor->set_shape(kernel_out_new_shapes);
  NPUPassUtils::UpdateKernel(kernel, kernel->in_kernels(), cur_out_kernels, kernel->in_tensors(), {kernel_out_tensor});
  return RET_OK;
}

int NPUTransformPass::UpdateNC2NHTransNodeAfterKernel(kernel::LiteKernel *kernel, kernel::LiteKernel *trans_kernel,
                                                      kernel::LiteKernel *next_kernel) {
  std::vector<Tensor *> next_in_tensors;
  for (auto next_in_tensor : next_kernel->in_tensors()) {
    if (next_in_tensor != kernel->out_tensors()[0]) {
      next_in_tensors.push_back(next_in_tensor);
    } else {
      next_in_tensors.push_back(trans_kernel->out_tensors()[0]);
    }
  }
  next_kernel->set_in_tensors(next_in_tensors);
  std::vector<kernel::LiteKernel *> next_in_kernels;
  for (auto in_kernel : next_kernel->in_kernels()) {
    if (in_kernel == kernel) {
      next_in_kernels.push_back(trans_kernel);
    } else {
      next_in_kernels.push_back(in_kernel);
    }
  }
  NPUPassUtils::UpdateKernel(next_kernel, next_in_kernels, next_kernel->out_kernels(), next_in_tensors,
                             next_kernel->out_tensors());

  return RET_OK;
}

int NPUTransformPass::Run() {
  if (context_->IsNpuEnabled()) {
    std::vector<kernel::LiteKernel *> new_kernels;

    for (auto it = all_kernels_->begin(); it != all_kernels_->end(); it++) {
      auto kernel = *it;
      if (kernel->desc().arch != kNPU) {
        new_kernels.push_back(kernel);
        continue;
      }
      if (npu_trans_nodes.find(kernel->Type()) != npu_trans_nodes.end()) {
        InsertPreNode(context_, it, &new_kernels, all_tensors_);
        new_kernels.push_back(kernel);
        InsertPostNode(context_, it, &new_kernels, all_tensors_);
      } else {
        new_kernels.push_back(kernel);
      }
    }
    all_kernels_->clear();
    for (int i = 0; i < new_kernels.size(); i++) {
      all_kernels_->push_back(new_kernels[i]);
    }
  }
  return RET_OK;
}

}  // namespace mindspore::lite
