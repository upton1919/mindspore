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
#include "src/runtime/agent/npu/optimizer/npu_fusion_pass.h"
#include <vector>
#include "src/lite_kernel.h"
#include "nnacl/concat_parameter.h"

namespace mindspore::lite {
bool CheckFusion(kernel::LiteKernel *kernel) {
  auto pre_flag =
    std::all_of(kernel->in_kernels().begin(), kernel->in_kernels().end(), [](const kernel::LiteKernel *kernel) {
      return kernel->Type() == schema::PrimitiveType_Nchw2Nhwc && kernel->out_kernels().size() == 1;
    });
  if (!pre_flag) {
    return false;
  }
  auto post_flag =
    std::all_of(kernel->out_kernels().begin(), kernel->out_kernels().end(), [](const kernel::LiteKernel *kernel) {
      return kernel->Type() == schema::PrimitiveType_Nhwc2Nchw && kernel->in_kernels().size() == 1;
    });
  return post_flag;
}

void NPUFusionPass::UpdatePreKernels(kernel::LiteKernel *cur_kernel) {
  for (auto in_kernel : cur_kernel->in_kernels()) {
    auto pre_kernel = in_kernel->in_kernels()[0];

    auto pre_out_kernels = pre_kernel->out_kernels();
    for (size_t i = 0; i < pre_out_kernels.size(); i++) {
      if (pre_out_kernels[i] == in_kernel) {
        pre_out_kernels[i] = cur_kernel;
        break;
      }
    }
    pre_kernel->set_out_kernels(pre_out_kernels);

    auto cur_in_kernels = cur_kernel->in_kernels();
    for (size_t i = 0; i < cur_in_kernels.size(); i++) {
      if (cur_in_kernels[i] == in_kernel) {
        cur_in_kernels[i] = pre_kernel;
        break;
      }
    }
    cur_kernel->set_in_kernels(cur_in_kernels);
    kernels->erase(find(kernels->begin(), kernels->end(), in_kernel));
  }
}

void NPUFusionPass::UpdatePostKernels(kernel::LiteKernel *cur_kernel) {
  for (auto out_kernel : cur_kernel->out_kernels()) {
    auto post_kernel = out_kernel->out_kernels()[0];

    auto post_in_kernels = post_kernel->in_kernels();
    for (size_t i = 0; i < post_in_kernels.size(); i++) {
      if (post_in_kernels[i] == out_kernel) {
        post_in_kernels[i] = cur_kernel;
        break;
      }
    }
    post_kernel->set_in_kernels(post_in_kernels);

    auto cur_out_kernels = cur_kernel->out_kernels();
    for (size_t i = 0; i < cur_out_kernels.size(); i++) {
      if (cur_out_kernels[i] == out_kernel) {
        cur_out_kernels[i] = post_kernel;
        break;
      }
    }
    cur_kernel->set_out_kernels(cur_out_kernels);
    kernels->erase(find(kernels->begin(), kernels->end(), out_kernel));
  }
}

void UpdatePreTensors(kernel::LiteKernel *cur_kernel) {
  auto tensors_vec = cur_kernel->in_tensors();
  for (auto in_kernel : cur_kernel->in_kernels()) {
    lite::Tensor *cur_tensor = nullptr;
    auto in_tensor = in_kernel->in_tensors()[0];
    auto out_tensor = in_kernel->out_tensors()[0];
    auto pre_kernel = in_kernel->in_kernels()[0];
    for (size_t i = 0; i < pre_kernel->out_tensors().size(); i++) {
      if (pre_kernel->out_tensors()[i] == in_tensor) {
        cur_tensor = pre_kernel->out_tensors()[i];
      }
    }
    for (size_t i = 0; i < tensors_vec.size(); i++) {
      if (tensors_vec[i] == out_tensor) {
        tensors_vec[i] = cur_tensor;
      }
    }
  }
  cur_kernel->set_in_tensors(tensors_vec);
}

void UpdatePostTensors(kernel::LiteKernel *cur_kernel) {
  auto tensor = cur_kernel->out_tensors()[0];
  for (auto out_kernel : cur_kernel->out_kernels()) {
    auto out_tensor = out_kernel->out_tensors()[0];
    for (auto post_kernel : out_kernel->out_kernels()) {
      auto tensors_vec = post_kernel->in_tensors();
      for (int i = 0; i < tensors_vec.size(); i++) {
        if (tensors_vec[i] == out_tensor) {
          tensors_vec[i] = tensor;
        }
      }
      post_kernel->set_in_tensors(tensors_vec);
    }
  }
}

int TransFormAxis(int axis) {
  switch (axis) {
    case 0:
      return 0;
    case 1:
      return 2;
    case 2:
      return 3;
    case 3:
    case -1:
      return 1;
    default:
      return -2;
  }
}

int NPUFusionPass::AddFusion(kernel::LiteKernel *kernel) {
  if (!CheckFusion(kernel)) {
    return RET_OK;
  }
  UpdatePreTensors(kernel);
  UpdatePostTensors(kernel);
  UpdatePreKernels(kernel);
  UpdatePostKernels(kernel);
  return RET_OK;
}

int NPUFusionPass::ConcatFusion(kernel::LiteKernel *kernel) {
  if (!CheckFusion(kernel)) {
    return RET_OK;
  }
  UpdatePreTensors(kernel);
  UpdatePostTensors(kernel);
  UpdatePreKernels(kernel);
  UpdatePostKernels(kernel);
  auto concat_param = reinterpret_cast<ConcatParameter *>(kernel->op_parameter());
  concat_param->axis_ = TransFormAxis(concat_param->axis_);
  return RET_OK;
}

int NPUFusionPass::FormatFusion(kernel::LiteKernel *kernel) {
  if (kernel->out_kernels().empty()) {
    return RET_OK;
  }
  if (!std::all_of(kernel->out_kernels().begin(), kernel->out_kernels().end(), [](const kernel::LiteKernel *kernel) {
        return kernel->Type() == schema::PrimitiveType_Nhwc2Nchw;
      })) {
    return RET_OK;
  }
  auto pre_kernel = kernel->in_kernels()[0];

  auto pre_out_kernels = pre_kernel->out_kernels();
  for (size_t i = 0; i < pre_out_kernels.size(); i++) {
    if (pre_out_kernels[i] == kernel) {
      pre_out_kernels.erase(pre_out_kernels.begin() + i);
      break;
    }
  }
  for (const auto &nc2nh : kernel->out_kernels()) {
    for (const auto &post_kernel : nc2nh->out_kernels()) {
      auto post_in_kernels = post_kernel->in_kernels();
      for (size_t i = 0; i < post_in_kernels.size(); i++) {
        if (post_in_kernels[i] == nc2nh) {
          post_in_kernels[i] = pre_kernel;
          break;
        }
      }
      post_kernel->set_in_kernels(post_in_kernels);
      pre_out_kernels.push_back(post_kernel);
    }
    kernels->erase(find(kernels->begin(), kernels->end(), nc2nh));
  }
  pre_kernel->set_out_kernels(pre_out_kernels);
  kernels->erase(find(kernels->begin(), kernels->end(), kernel));
  return RET_OK;
}

int NPUFusionPass::Run() {
  for (auto kernel : *kernels) {
    switch (kernel->Type()) {
      case schema::PrimitiveType_Concat:
        ConcatFusion(kernel);
        continue;
      case schema::PrimitiveType_Add:
        AddFusion(kernel);
        continue;
      case schema::PrimitiveType_Nchw2Nhwc:
        FormatFusion(kernel);
        continue;
      default:
        continue;
    }
  }
  return RET_OK;
}
}  // namespace mindspore::lite
