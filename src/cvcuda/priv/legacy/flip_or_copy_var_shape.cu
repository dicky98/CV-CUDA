/* Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved.
 * Copyright (C) 2021-2022, Bytedance Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CvCudaLegacy.h"
#include "CvCudaLegacyHelpers.hpp"

#include "CvCudaUtils.cuh"

using namespace nvcv::legacy::cuda_op;
using namespace nvcv::legacy::helpers;

namespace nvcv::legacy::cuda_op {

template<typename T>
__global__ void flip_kernel(const cuda::ImageBatchVarShapeWrap<T> src, cuda::ImageBatchVarShapeWrap<T> dst,
                            const cuda::Tensor1DWrap<int> flipCode)
{
    const int x         = blockIdx.x * blockDim.x + threadIdx.x;
    const int y         = blockIdx.y * blockDim.y + threadIdx.y;
    const int batch_idx = get_batch_idx();

    int out_height = dst.height(batch_idx), out_width = dst.width(batch_idx);
    if (x >= out_width || y >= out_height)
        return;
    int flip_code = flipCode[batch_idx];

    if (flip_code == 1) // flip_code = 1, horizontal flip
    {
        *dst.ptr(batch_idx, y, x) = *src.ptr(batch_idx, y, (out_width - 1 - x));
    }
    else if (flip_code == 0) // flip_code = 0, vertical flip
    {
        *dst.ptr(batch_idx, y, x) = *src.ptr(batch_idx, (out_height - 1 - y), x);
    }
    else if (flip_code == -1) // flip_code = -1, horizontal and vertical flip
    {
        *dst.ptr(batch_idx, y, x) = *src.ptr(batch_idx, (out_height - 1 - y), (out_width - 1 - x));
    }
    else // just copy
    {
        *dst.ptr(batch_idx, y, x) = *src.ptr(batch_idx, y, x);
    }
}

template<typename T>
void flip(const ImageBatchVarShapeDataStridedCuda &input, const ImageBatchVarShapeDataStridedCuda &output,
          const TensorDataStridedCuda &flipCode, cudaStream_t stream)
{
    constexpr uint32_t BLOCK = 32;

    dim3 blockSize(BLOCK, BLOCK / 4, 1);
    dim3 gridSize(divUp(input.maxSize().w, blockSize.x), divUp(input.maxSize().h, blockSize.y), output.numImages());

    cuda::ImageBatchVarShapeWrap<T> src(input);
    cuda::ImageBatchVarShapeWrap<T> dst(output);
    cuda::Tensor1DWrap<int>         flip_code(flipCode);

#ifdef CUDA_DEBUG_LOG
    checkCudaErrors(cudaStreamSynchronize(stream));
    checkCudaErrors(cudaGetLastError());
#endif // CUDA_DEBUG_LOG

    flip_kernel<T><<<gridSize, blockSize, 0, stream>>>(src, dst, flip_code);
    checkKernelErrors();
#ifdef CUDA_DEBUG_LOG
    checkCudaErrors(cudaStreamSynchronize(stream));
    checkCudaErrors(cudaGetLastError());
#endif // CUDA_DEBUG_LOG
}

ErrorCode FlipOrCopyVarShape::infer(const ImageBatchVarShapeDataStridedCuda &input,
                                    const ImageBatchVarShapeDataStridedCuda &output,
                                    const TensorDataStridedCuda &flipCode, cudaStream_t stream)
{
    if (!input.uniqueFormat())
    {
        LOG_ERROR("Images in the input batch must all have the same format");
        return ErrorCode::INVALID_DATA_FORMAT;
    }
    if (!output.uniqueFormat())
    {
        LOG_ERROR("Images in the output batch must all have the same format");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataFormat inputFormat  = helpers::GetLegacyDataFormat(input);
    DataFormat outputFormat = helpers::GetLegacyDataFormat(output);

    if (inputFormat != outputFormat)
    {
        LOG_ERROR("Invalid DataFormat between input (" << inputFormat << ") and output (" << outputFormat << ")");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataFormat format = inputFormat;
    if (!(format == kNHWC || format == kHWC))
    {
        LOG_ERROR("Invalid input DataFormat " << format << ", the valid DataFormats are: \"NHWC\", \"HWC\"");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataType dataType    = helpers::GetLegacyDataType(input.uniqueFormat());
    DataType outDataType = helpers::GetLegacyDataType(output.uniqueFormat());

    if (dataType != outDataType)
    {
        LOG_ERROR("DataType of input and output must be equal, but got " << dataType << " and " << outDataType);
        return ErrorCode::INVALID_DATA_TYPE;
    }

    if (!(dataType == kCV_8U || dataType == kCV_16U || dataType == kCV_16S || dataType == kCV_32S
          || dataType == kCV_32F))
    {
        LOG_ERROR("Invalid DataType " << dataType);
        return ErrorCode::INVALID_DATA_TYPE;
    }

    const int channels = input.uniqueFormat().numChannels();
    if (channels > 4)
    {
        LOG_ERROR("Invalid channel number " << channels);
        return ErrorCode::INVALID_DATA_SHAPE;
    }

    // using flip_t = void(const ImageBatchVarShapeDataStridedCuda & input,
    //                     const ImageBatchVarShapeDataStridedCuda & output,
    //                     const TensorDataStridedCuda & flipCode,
    //                     cudaStream_t stream);
    typedef void (*flip_t)(const ImageBatchVarShapeDataStridedCuda &input,
                           const ImageBatchVarShapeDataStridedCuda &output, const TensorDataStridedCuda &flipCode,
                           cudaStream_t stream);

    static const flip_t funcs[6][4] = {
        { flip<uchar>, 0,  flip<uchar3>,  flip<uchar4>},
        {           0, 0,             0,             0},
        {flip<ushort>, 0, flip<ushort3>, flip<ushort4>},
        { flip<short>, 0,  flip<short3>,  flip<short4>},
        {   flip<int>, 0,    flip<int3>,    flip<int4>},
        { flip<float>, 0,  flip<float3>,  flip<float4>}
    };

    funcs[dataType][channels - 1](input, output, flipCode, stream);

    return ErrorCode::SUCCESS;
}

} // namespace nvcv::legacy::cuda_op
