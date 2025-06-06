/* Copyright (c) 2021-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 *
 * SPDX-FileCopyrightText: NVIDIA CORPORATION & AFFILIATES
 * SPDX-License-Identifier: Apache-2.0
 *
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

#include <cvcuda/OpNormalize.h>             // for CVCUDA_NORMALIZE_SCALE_IS_STDDEV, etc.
#include <cvcuda/cuda_tools/TypeTraits.hpp> // for TypeTraits

using namespace nvcv::legacy::cuda_op;
using namespace nvcv::legacy::helpers;
namespace cuda = nvcv::cuda;

// (float3 - float3) * float3 / (float3 - float) * float3 / (float3 - float3) * float / (float3 - float) * float
template<typename input_type, typename base_type, typename scale_type>
__global__ void normalizeKernel(const input_type src, const base_type base, const scale_type scale, input_type dst,
                                int2 inout_size, int3 base_size, int3 scale_size, float global_scale,
                                float global_shift)
{
    const int src_x     = blockIdx.x * blockDim.x + threadIdx.x;
    const int src_y     = blockIdx.y * blockDim.y + threadIdx.y;
    const int batch_idx = get_batch_idx();

    if (src_x >= inout_size.x || src_y >= inout_size.y)
        return;

    const int base_x         = base_size.x == 1 ? 0 : src_x;
    const int base_y         = base_size.y == 1 ? 0 : src_y;
    const int base_batch_idx = base_size.z == 1 ? 0 : batch_idx;

    const int scale_x         = scale_size.x == 1 ? 0 : src_x;
    const int scale_y         = scale_size.y == 1 ? 0 : src_y;
    const int scale_batch_idx = scale_size.z == 1 ? 0 : batch_idx;

    using input_value_type = typename input_type::ValueType;

    *dst.ptr(batch_idx, src_y, src_x) = nvcv::cuda::SaturateCast<input_value_type>(
        (*src.ptr(batch_idx, src_y, src_x) - *base.ptr(base_batch_idx, base_y, base_x))
            * (*scale.ptr(scale_batch_idx, scale_y, scale_x)) * global_scale
        + global_shift);
}

// (float3 - float3) * float3 / (float3 - float) * float3 / (float3 - float3) * float / (float3 - float) * float
template<typename input_type, typename base_type, typename scale_type>
__global__ void normalizeInvStdDevKernel(const input_type src, const base_type base, const scale_type scale,
                                         input_type dst, int2 inout_size, int3 base_size, int3 scale_size,
                                         float global_scale, float global_shift, float epsilon)
{
    const int src_x     = blockIdx.x * blockDim.x + threadIdx.x;
    const int src_y     = blockIdx.y * blockDim.y + threadIdx.y;
    const int batch_idx = get_batch_idx();

    if (src_x >= inout_size.x || src_y >= inout_size.y)
        return;

    const int base_x         = base_size.x == 1 ? 0 : src_x;
    const int base_y         = base_size.y == 1 ? 0 : src_y;
    const int base_batch_idx = base_size.z == 1 ? 0 : batch_idx;

    const int scale_x         = scale_size.x == 1 ? 0 : src_x;
    const int scale_y         = scale_size.y == 1 ? 0 : src_y;
    const int scale_batch_idx = scale_size.z == 1 ? 0 : batch_idx;

    using input_value_type = typename input_type::ValueType;
    using scale_value_type = typename scale_type::ValueType;

    scale_value_type s   = *scale.ptr(scale_batch_idx, scale_y, scale_x);
    scale_value_type x   = s * s + epsilon;
    scale_value_type mul = 1.0f / nvcv::cuda::sqrt(x);

    *dst.ptr(batch_idx, src_y, src_x) = nvcv::cuda::SaturateCast<input_value_type>(
        (*src.ptr(batch_idx, src_y, src_x) - *base.ptr(base_batch_idx, base_y, base_x)) * mul * global_scale
        + global_shift);
}

template<typename base_type, typename scale_type, typename WrapInput, typename WrapOutput>
void normalizeWrap(WrapInput srcWrap, WrapOutput dstWrap, DataShape input_shape,
                   const nvcv::TensorDataStridedCuda &baseData, const nvcv::TensorDataStridedCuda &scaleData,
                   float global_scale, float shift, cudaStream_t stream)
{
    dim3 block(32, 8);
    dim3 grid(divUp(input_shape.W, block.x), divUp(input_shape.H, block.y), input_shape.N);

    auto baseWrap  = nvcv::cuda::CreateTensorWrapNHW<base_type, int32_t>(baseData);
    auto scaleWrap = nvcv::cuda::CreateTensorWrapNHW<scale_type, int32_t>(scaleData);

    auto baseAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(baseData);
    NVCV_ASSERT(baseAccess);

    auto scaleAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(scaleData);
    NVCV_ASSERT(scaleAccess);

    int2 inout_size = {input_shape.W, input_shape.H};
    int3 base_size  = {static_cast<int>(baseAccess->numCols()), static_cast<int>(baseAccess->numRows()),
                       static_cast<int>(baseAccess->numSamples())};
    int3 scale_size = {static_cast<int>(scaleAccess->numCols()), static_cast<int>(scaleAccess->numRows()),
                       static_cast<int>(scaleAccess->numSamples())};

    normalizeKernel<<<grid, block, 0, stream>>>(srcWrap, baseWrap, scaleWrap, dstWrap, inout_size, base_size,
                                                scale_size, global_scale, shift);
    checkKernelErrors();
}

template<typename base_type, typename scale_type, typename WrapInput, typename WrapOutput>
void normalizeInvStdDevWrap(WrapInput srcWrap, WrapOutput dstWrap, DataShape input_shape,
                            const nvcv::TensorDataStridedCuda &baseData, const nvcv::TensorDataStridedCuda &scaleData,
                            float global_scale, float shift, float epsilon, cudaStream_t stream)
{
    dim3 block(32, 8);
    dim3 grid(divUp(input_shape.W, block.x), divUp(input_shape.H, block.y), input_shape.N);

    auto baseWrap  = nvcv::cuda::CreateTensorWrapNHW<base_type, int32_t>(baseData);
    auto scaleWrap = nvcv::cuda::CreateTensorWrapNHW<scale_type, int32_t>(scaleData);

    auto baseAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(baseData);
    NVCV_ASSERT(baseAccess);

    auto scaleAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(scaleData);
    NVCV_ASSERT(scaleAccess);

    int2 inout_size = {input_shape.W, input_shape.H};
    int3 base_size  = {static_cast<int>(baseAccess->numCols()), static_cast<int>(baseAccess->numRows()),
                       static_cast<int>(baseAccess->numSamples())};
    int3 scale_size = {static_cast<int>(scaleAccess->numCols()), static_cast<int>(scaleAccess->numRows()),
                       static_cast<int>(scaleAccess->numSamples())};

    normalizeInvStdDevKernel<<<grid, block, 0, stream>>>(srcWrap, baseWrap, scaleWrap, dstWrap, inout_size, base_size,
                                                         scale_size, global_scale, shift, epsilon);
    checkKernelErrors();
}

template<typename input_wrapper, typename output_wrapper>
void callNormalizeWrap(const input_wrapper &input, const DataShape &inputShape,
                       const nvcv::TensorDataStridedCuda &baseData, const nvcv::TensorDataStridedCuda &scaleData,
                       const output_wrapper &output, float global_scale, float shift, cudaStream_t stream)
{
    auto baseAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(baseData);
    NVCV_ASSERT(baseAccess);

    auto scaleAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(scaleData);
    NVCV_ASSERT(scaleAccess);

    using input_type = typename input_wrapper::ValueType;
    using work_type  = nvcv::cuda::ConvertBaseTypeTo<float, input_type>;

    if (baseAccess->numChannels() != 1 && scaleAccess->numChannels() != 1)
    {
        using base_type  = work_type;
        using scale_type = work_type;
        normalizeWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale, shift,
                                             stream);
    }
    else if (baseAccess->numChannels() != 1)
    {
        using base_type  = work_type;
        using scale_type = float;
        normalizeWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale, shift,
                                             stream);
    }
    else if (scaleAccess->numChannels() != 1)
    {
        using base_type  = float;
        using scale_type = work_type;
        normalizeWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale, shift,
                                             stream);
    }
    else
    {
        using base_type  = float;
        using scale_type = float;
        normalizeWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale, shift,
                                             stream);
    }
}

template<typename input_type>
ErrorCode normalize(const nvcv::TensorDataStridedCuda &inData, const nvcv::TensorDataStridedCuda &baseData,
                    const nvcv::TensorDataStridedCuda &scaleData, const nvcv::TensorDataStridedCuda &outData,
                    float global_scale, float shift, cudaStream_t stream)
{
    auto inAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(inData);
    NVCV_ASSERT(inAccess);

    auto outAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(outData);
    NVCV_ASSERT(outAccess);

    DataShape inputShape = GetLegacyDataShape(inAccess->infoShape());

    auto inMaxStride  = inAccess->sampleStride() * inAccess->numSamples();
    auto outMaxStride = outAccess->sampleStride() * outAccess->numSamples();
    if (std::max(inMaxStride, outMaxStride) <= cuda::TypeTraits<int32_t>::max)
    {
        auto srcWrap = nvcv::cuda::CreateTensorWrapNHW<input_type, int32_t>(inData);
        auto dstWrap = nvcv::cuda::CreateTensorWrapNHW<input_type, int32_t>(outData);
        callNormalizeWrap(srcWrap, inputShape, baseData, scaleData, dstWrap, global_scale, shift, stream);
    }
    else
    {
        LOG_ERROR("Input or output size exceeds " << cuda::TypeTraits<int32_t>::max << ". Tensor is too large.");
        return ErrorCode::INVALID_PARAMETER;
    }
    return ErrorCode::SUCCESS;
}

template<typename input_wrapper, typename output_wrapper>
void callNormalizeInvStdDevWrap(const input_wrapper &input, const DataShape &inputShape,
                                const nvcv::TensorDataStridedCuda &baseData,
                                const nvcv::TensorDataStridedCuda &scaleData, const output_wrapper &output,
                                float global_scale, float shift, float epsilon, cudaStream_t stream)
{
    auto baseAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(baseData);
    NVCV_ASSERT(baseAccess);

    auto scaleAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(scaleData);
    NVCV_ASSERT(scaleAccess);

    using input_type = typename input_wrapper::ValueType;
    using work_type  = nvcv::cuda::ConvertBaseTypeTo<float, input_type>;

    if (baseAccess->numChannels() != 1 && scaleAccess->numChannels() != 1)
    {
        using base_type  = work_type;
        using scale_type = work_type;
        normalizeInvStdDevWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale,
                                                      shift, epsilon, stream);
    }
    else if (baseAccess->numChannels() != 1)
    {
        using base_type  = work_type;
        using scale_type = float;
        normalizeInvStdDevWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale,
                                                      shift, epsilon, stream);
    }
    else if (scaleAccess->numChannels() != 1)
    {
        using base_type  = float;
        using scale_type = work_type;
        normalizeInvStdDevWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale,
                                                      shift, epsilon, stream);
    }
    else
    {
        using base_type  = float;
        using scale_type = float;
        normalizeInvStdDevWrap<base_type, scale_type>(input, output, inputShape, baseData, scaleData, global_scale,
                                                      shift, epsilon, stream);
    }
}

template<typename input_type>
ErrorCode normalizeInvStdDev(const nvcv::TensorDataStridedCuda &inData, const nvcv::TensorDataStridedCuda &baseData,
                             const nvcv::TensorDataStridedCuda &scaleData, const nvcv::TensorDataStridedCuda &outData,
                             float global_scale, float shift, float epsilon, cudaStream_t stream)
{
    auto inAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(inData);
    NVCV_ASSERT(inAccess);

    auto outAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(outData);
    NVCV_ASSERT(outAccess);

    DataShape inputShape = GetLegacyDataShape(inAccess->infoShape());

    auto inMaxStride  = inAccess->sampleStride() * inAccess->numSamples();
    auto outMaxStride = outAccess->sampleStride() * outAccess->numSamples();
    if (std::max(inMaxStride, outMaxStride) <= cuda::TypeTraits<int32_t>::max)
    {
        auto srcWrap = nvcv::cuda::CreateTensorWrapNHW<input_type, int32_t>(inData);
        auto dstWrap = nvcv::cuda::CreateTensorWrapNHW<input_type, int32_t>(outData);
        callNormalizeInvStdDevWrap(srcWrap, inputShape, baseData, scaleData, dstWrap, global_scale, shift, epsilon,
                                   stream);
    }
    else
    {
        LOG_ERROR("Input or output size exceeds " << cuda::TypeTraits<int32_t>::max << ". Tensor is too large.");
        return ErrorCode::INVALID_PARAMETER;
    }
    return ErrorCode::SUCCESS;
}

namespace nvcv::legacy::cuda_op {

void Normalize::checkParamShape(DataShape input_shape, DataShape param_shape)
{
    NVCV_ASSERT(param_shape.N == input_shape.N || param_shape.N == 1);
    NVCV_ASSERT(param_shape.C == input_shape.C || param_shape.C == 1);
    NVCV_ASSERT(param_shape.H == input_shape.H || param_shape.H == 1);
    NVCV_ASSERT(param_shape.W == input_shape.W || param_shape.W == 1);
}

ErrorCode Normalize::infer(const nvcv::TensorDataStridedCuda &inData, const nvcv::TensorDataStridedCuda &baseData,
                           const nvcv::TensorDataStridedCuda &scaleData, const nvcv::TensorDataStridedCuda &outData,
                           const float global_scale, const float shift, const float epsilon, const uint32_t flags,
                           cudaStream_t stream)
{
    DataFormat format        = GetLegacyDataFormat(inData.layout());
    DataFormat output_format = helpers::GetLegacyDataFormat(outData);
    if (format != output_format)
    {
        LOG_ERROR("Invalid DataFormat between input (" << format << ") and output (" << output_format << ")");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    if (!(format == kNHWC || format == kHWC))
    {
        LOG_ERROR("Invalid input DataFormat " << format << ", the valid DataFormats are: \"NHWC\", \"HWC\"");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    auto inAccess = TensorDataAccessStridedImagePlanar::Create(inData);
    if (!inAccess)
    {
        LOG_ERROR("Invalid DataFormat(in) " << format);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    auto baseAccess = TensorDataAccessStridedImagePlanar::Create(baseData);
    if (!baseAccess)
    {
        LOG_ERROR("Invalid DataFormat(base) " << format);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    auto scaleAccess = TensorDataAccessStridedImagePlanar::Create(scaleData);
    if (!scaleAccess)
    {
        LOG_ERROR("Invalid DataFormat(scale) " << format);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    auto outAccess = TensorDataAccessStridedImagePlanar::Create(outData);
    if (!outAccess)
    {
        LOG_ERROR("Invalid DataFormat(out) " << format);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataType  data_type         = GetLegacyDataType(inData.dtype());
    DataShape input_shape       = GetLegacyDataShape(inAccess->infoShape());
    DataShape base_param_shape  = GetLegacyDataShape(baseAccess->infoShape());
    DataShape scale_param_shape = GetLegacyDataShape(scaleAccess->infoShape());

    int channels = input_shape.C;

    if (channels > 4)
    {
        LOG_ERROR("Invalid channel number " << channels);
        return ErrorCode::INVALID_DATA_SHAPE;
    }

    if (!(data_type == kCV_8U || data_type == kCV_8S || data_type == kCV_16U || data_type == kCV_16S
          || data_type == kCV_32S || data_type == kCV_32F))
    {
        LOG_ERROR("Invalid DataType " << data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }

    checkParamShape(input_shape, base_param_shape);
    checkParamShape(input_shape, scale_param_shape);

    typedef ErrorCode (*normalize_t)(const TensorDataStridedCuda &inData, const TensorDataStridedCuda &baseData,
                                     const TensorDataStridedCuda &scaleData, const TensorDataStridedCuda &outData,
                                     float global_scale, float shift, cudaStream_t stream);

    typedef ErrorCode (*normalizeInvStdDev_t)(
        const TensorDataStridedCuda &inData, const TensorDataStridedCuda &baseData,
        const TensorDataStridedCuda &scaleData, const TensorDataStridedCuda &outData, float global_scale, float shift,
        float epsilon, cudaStream_t stream);

    static const normalize_t funcs_normalize[6][4] = {
        { normalize<uchar>,  0 /*normalize<uchar2>*/,  normalize<uchar3>,  normalize<uchar4>},
        { normalize<schar>,   0 /*normalize<char2>*/,   normalize<char3>,   normalize<char4>},
        {normalize<ushort>, 0 /*normalize<ushort2>*/, normalize<ushort3>, normalize<ushort4>},
        { normalize<short>,  0 /*normalize<short2>*/,  normalize<short3>,  normalize<short4>},
        {   normalize<int>,    0 /*normalize<int2>*/,    normalize<int3>,    normalize<int4>},
        { normalize<float>,  0 /*normalize<float2>*/,  normalize<float3>,  normalize<float4>}
    };

    static const normalizeInvStdDev_t funcs_normalize_stddev[6][4] = {
        { normalizeInvStdDev<uchar>,  0 /*normalizeInvStdDev<uchar2>*/,  normalizeInvStdDev<uchar3>,
         normalizeInvStdDev<uchar4>                                                                                          },
        { normalizeInvStdDev<schar>,   0 /*normalizeInvStdDev<char2>*/,   normalizeInvStdDev<char3>,
         normalizeInvStdDev<char4>                                                                                           },
        {normalizeInvStdDev<ushort>, 0 /*normalizeInvStdDev<ushort2>*/, normalizeInvStdDev<ushort3>,
         normalizeInvStdDev<ushort4>                                                                                         },
        { normalizeInvStdDev<short>,  0 /*normalizeInvStdDev<short2>*/,  normalizeInvStdDev<short3>,
         normalizeInvStdDev<short4>                                                                                          },
        {   normalizeInvStdDev<int>,    0 /*normalizeInvStdDev<int2>*/,    normalizeInvStdDev<int3>, normalizeInvStdDev<int4>},
        { normalizeInvStdDev<float>,  0 /*normalizeInvStdDev<float2>*/,  normalizeInvStdDev<float3>,
         normalizeInvStdDev<float4>                                                                                          }
    };

    if (flags & CVCUDA_NORMALIZE_SCALE_IS_STDDEV)
    {
        return funcs_normalize_stddev[data_type][channels - 1](inData, baseData, scaleData, outData, global_scale,
                                                               shift, epsilon, stream);
    }
    else
    {
        return funcs_normalize[data_type][channels - 1](inData, baseData, scaleData, outData, global_scale, shift,
                                                        stream);
    }
}

} // namespace nvcv::legacy::cuda_op
