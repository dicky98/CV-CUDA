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
#include "cub/cub.cuh"

using namespace nvcv::legacy::helpers;

using namespace nvcv::legacy::cuda_op;

static __device__ int erase_var_shape_hash(unsigned int x)
{
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = ((x >> 16) ^ x) * 0x45d9f3b;
    x = (x >> 16) ^ x;
    return x;
}

template<typename D>
__global__ void erase(nvcv::cuda::ImageBatchVarShapeWrapNHWC<D> img, nvcv::cuda::Tensor1DWrap<int2> anchorVec,
                      nvcv::cuda::Tensor1DWrap<int3> erasingVec, nvcv::cuda::Tensor1DWrap<float> valuesVec,
                      nvcv::cuda::Tensor1DWrap<int> imgIdxVec, int channels, int random, unsigned int seed)
{
    unsigned int id      = threadIdx.x + blockIdx.x * blockDim.x;
    int          c       = blockIdx.y;
    int          eraseId = blockIdx.z;
    int2         anchor  = anchorVec[eraseId];
    int3         erasing = erasingVec[eraseId];
    float        value   = valuesVec[eraseId * channels + c];
    int          batchId = imgIdxVec[eraseId];
    if (id < erasing.y * erasing.x && (0x1 & (erasing.z >> c)) == 1)
    {
        int x = id % erasing.x;
        int y = id / erasing.x;
        if ((anchor.x + x) < img.width(batchId) && (anchor.y + y) < img.height(batchId))
        {
            if (random)
            {
                unsigned int hashValue = seed + threadIdx.x
                                       + 0x26AD0C9 * blockDim.x * blockDim.y * blockDim.z * (blockIdx.x + 1)
                                             * (blockIdx.y + 1) * (blockIdx.z + 1);
                *img.ptr(batchId, anchor.y + y, anchor.x + x, c)
                    = nvcv::cuda::SaturateCast<D>(erase_var_shape_hash(hashValue) % 256);
            }
            else
            {
                *img.ptr(batchId, anchor.y + y, anchor.x + x, c) = nvcv::cuda::SaturateCast<D>(value);
            }
        }
    }
}

template<typename D>
void eraseCaller(const nvcv::ImageBatchVarShapeDataStridedCuda &imgs, const nvcv::TensorDataStridedCuda &anchor,
                 const nvcv::TensorDataStridedCuda &erasing, const nvcv::TensorDataStridedCuda &imgIdx,
                 const nvcv::TensorDataStridedCuda &values, int max_eh, int max_ew, int num_erasing_area, bool random,
                 unsigned int seed, cudaStream_t stream)
{
    nvcv::cuda::ImageBatchVarShapeWrapNHWC<D> src(imgs, imgs.uniqueFormat().numChannels());

    nvcv::cuda::Tensor1DWrap<int2>  anchorVec(anchor);
    nvcv::cuda::Tensor1DWrap<int3>  erasingVec(erasing);
    nvcv::cuda::Tensor1DWrap<int>   imgIdxVec(imgIdx);
    nvcv::cuda::Tensor1DWrap<float> valuesVec(values);

    int  channel   = imgs.uniqueFormat().numChannels();
    int  blockSize = (max_eh * max_ew < 1024) ? max_eh * max_ew : 1024;
    int  gridSize  = divUp(max_eh * max_ew, 1024);
    dim3 block(blockSize);
    dim3 grid(gridSize, channel, num_erasing_area);
    erase<D><<<grid, block, 0, stream>>>(src, anchorVec, erasingVec, valuesVec, imgIdxVec, channel, random, seed);
}

struct MaxWH
{
    __device__ __forceinline__ int3 operator()(const int3 &a, const int3 &b) const
    {
        return int3{max(a.x, b.x), max(a.y, b.y), 0};
    }
};

namespace nvcv::legacy::cuda_op {

EraseVarShape::EraseVarShape(DataShape max_input_shape, DataShape max_output_shape, int num_erasing_area)
    : CudaBaseOp(max_input_shape, max_output_shape)
    , d_max_values(nullptr)
    , temp_storage(nullptr)
{
    cudaError_t err = cudaMalloc(&d_max_values, sizeof(int3));
    if (err != cudaSuccess)
    {
        LOG_ERROR("CUDA memory allocation error of size: " << sizeof(int3));
        throw std::runtime_error("CUDA memory allocation error!");
    }

    max_num_erasing_area = num_erasing_area;
    if (max_num_erasing_area < 0)
    {
        cudaFree(d_max_values);
        LOG_ERROR("Invalid num of erasing area" << max_num_erasing_area);
        throw nvcv::Exception(nvcv::Status::ERROR_INVALID_ARGUMENT, "max_num_erasing_area must be >= 0");
    }
    temp_storage  = NULL;
    storage_bytes = 0;
    MaxWH mwh;
    int3  init = {0, 0, 0};
    cub::DeviceReduce::Reduce(temp_storage, storage_bytes, (int3 *)nullptr, (int3 *)nullptr, max_num_erasing_area, mwh,
                              init);

    err = cudaMalloc(&temp_storage, storage_bytes);
    if (err != cudaSuccess)
    {
        cudaFree(d_max_values);
        LOG_ERROR("CUDA memory allocation error of size: " << storage_bytes);
        throw std::runtime_error("CUDA memory allocation error!");
    }
}

EraseVarShape::~EraseVarShape()
{
    cudaError_t err0 = cudaFree(d_max_values);
    cudaError_t err1 = cudaFree(temp_storage);
    if (err0 != cudaSuccess || err1 != cudaSuccess)
    {
        LOG_ERROR("CUDA memory free error, possible memory leak!");
    }
    d_max_values = nullptr;
    temp_storage = nullptr;
}

ErrorCode EraseVarShape::infer(const nvcv::ImageBatchVarShape &inbatch, const nvcv::ImageBatchVarShape &outbatch,
                               const TensorDataStridedCuda &anchor, const TensorDataStridedCuda &erasing,
                               const TensorDataStridedCuda &values, const TensorDataStridedCuda &imgIdx, bool random,
                               unsigned int seed, bool inplace, cudaStream_t stream)
{
    auto inData = inbatch.exportData<nvcv::ImageBatchVarShapeDataStridedCuda>(stream);
    if (inData == nullptr)
    {
        LOG_ERROR("Input must be varshape image batch");
    }
    auto outData = outbatch.exportData<nvcv::ImageBatchVarShapeDataStridedCuda>(stream);
    if (outData == nullptr)
    {
        LOG_ERROR("Output must be varshape image batch");
    }

    DataFormat format     = helpers::GetLegacyDataFormat(*inData);
    DataFormat out_format = helpers::GetLegacyDataFormat(*outData);
    if (!(format == kNHWC || format == kHWC))
    {
        LOG_ERROR("Invalid input DataFormat " << format << ", the valid DataFormats are: \"NHWC\", \"HWC\"");
        return ErrorCode::INVALID_DATA_FORMAT;
    }
    if (!(out_format == kNHWC || out_format == kHWC))
    {
        LOG_ERROR("Invalid output DataFormat " << out_format << ", the valid DataFormats are: \"NHWC\", \"HWC\"");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    if (!inData->uniqueFormat())
    {
        LOG_ERROR("Images in input batch must all have the same format ");
        return ErrorCode::INVALID_DATA_FORMAT;
    }
    if (!outData->uniqueFormat())
    {
        LOG_ERROR("Images in output batch must all have the same format ");
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataType data_type     = helpers::GetLegacyDataType(inData->uniqueFormat());
    DataType out_data_type = helpers::GetLegacyDataType(outData->uniqueFormat());
    if (!(data_type == kCV_8U || data_type == kCV_16U || data_type == kCV_16S || data_type == kCV_32S
          || data_type == kCV_32F))
    {
        LOG_ERROR("Invalid DataType " << data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }
    if (data_type != out_data_type)
    {
        LOG_ERROR("DataType of input and output must be equal, but got " << data_type << " and " << out_data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }

    DataType anchor_data_type = GetLegacyDataType(anchor.dtype());
    if (anchor_data_type != kCV_32S)
    {
        LOG_ERROR("Invalid anchor DataType " << anchor_data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }
    int anchor_dim = anchor.layout().rank();
    if (anchor_dim != 1)
    {
        LOG_ERROR("Invalid anchor Dim " << anchor_dim);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    int num_erasing_area = anchor.shape()[0];
    if (num_erasing_area < 0)
    {
        LOG_ERROR("Invalid num of erasing area " << num_erasing_area);
        return ErrorCode::INVALID_PARAMETER;
    }
    if (num_erasing_area > max_num_erasing_area)
    {
        LOG_ERROR("Invalid num of erasing area " << num_erasing_area);
        return ErrorCode::INVALID_PARAMETER;
    }

    DataType erasing_data_type = GetLegacyDataType(erasing.dtype());
    if (erasing_data_type != kCV_32S)
    {
        LOG_ERROR("Invalid erasing DataType " << erasing_data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }
    int erasing_dim = erasing.layout().rank();
    if (erasing_dim != 1)
    {
        LOG_ERROR("Invalid erasing_w Dim " << erasing_dim);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataType imgidx_data_type = GetLegacyDataType(imgIdx.dtype());
    if (imgidx_data_type != kCV_32S)
    {
        LOG_ERROR("Invalid imgIdx DataType " << imgidx_data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }
    int imgidx_dim = imgIdx.layout().rank();
    if (imgidx_dim != 1)
    {
        LOG_ERROR("Invalid imgIdx Dim " << imgidx_dim);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    DataType values_data_type = GetLegacyDataType(values.dtype());
    if (values_data_type != kCV_32F)
    {
        LOG_ERROR("Invalid values DataType " << values_data_type);
        return ErrorCode::INVALID_DATA_TYPE;
    }
    int values_dim = values.layout().rank();
    if (values_dim != 1)
    {
        LOG_ERROR("Invalid values Dim " << values_dim);
        return ErrorCode::INVALID_DATA_FORMAT;
    }

    if (!inplace)
    {
        for (auto init = inbatch.begin(), outit = outbatch.begin(); init != inbatch.end(), outit != outbatch.end();
             ++init, ++outit)
        {
            const Image             &inimg      = *init;
            const Image             &outimg     = *outit;
            auto                     inimgdata  = inimg.exportData<ImageDataStridedCuda>();
            auto                     outimgdata = outimg.exportData<ImageDataStridedCuda>();
            const ImagePlaneStrided &inplane    = inimgdata->plane(0);
            const ImagePlaneStrided &outplane   = outimgdata->plane(0);
            checkCudaErrors(cudaMemcpy2DAsync(outplane.basePtr, outplane.rowStride, inplane.basePtr, inplane.rowStride,
                                              inplane.rowStride, inplane.height, cudaMemcpyDeviceToDevice, stream));
        }
    }

    if (num_erasing_area == 0)
    {
        return SUCCESS;
    }

    int3 *d_erasing = (int3 *)erasing.basePtr();
    int3  h_max_values;
    MaxWH maxwh;
    int3  init = {0, 0, 0};

    cub::DeviceReduce::Reduce(temp_storage, storage_bytes, d_erasing, d_max_values, num_erasing_area, maxwh, init,
                              stream);
    checkCudaErrors(cudaMemcpyAsync(&h_max_values, d_max_values, sizeof(int3), cudaMemcpyDeviceToHost, stream));

    checkCudaErrors(cudaStreamSynchronize(stream));

    int max_ew = h_max_values.x, max_eh = h_max_values.y;

    // All areas as empty? Weird, but valid nonetheless.
    if (max_ew == 0 || max_eh == 0)
    {
        return SUCCESS;
    }

    typedef void (*erase_t)(const ImageBatchVarShapeDataStridedCuda &imgs, const TensorDataStridedCuda &anchor,
                            const TensorDataStridedCuda &erasing, const TensorDataStridedCuda &imgIdx,
                            const TensorDataStridedCuda &values, int max_eh, int max_ew, int num_erasing_area,
                            bool random, unsigned int seed, cudaStream_t stream);

    static const erase_t funcs[6] = {eraseCaller<uchar>, eraseCaller<char>, eraseCaller<ushort>,
                                     eraseCaller<short>, eraseCaller<int>,  eraseCaller<float>};

    if (inplace)
        funcs[data_type](*inData, anchor, erasing, imgIdx, values, max_eh, max_ew, num_erasing_area, random, seed,
                         stream);
    else
        funcs[data_type](*outData, anchor, erasing, imgIdx, values, max_eh, max_ew, num_erasing_area, random, seed,
                         stream);

    return SUCCESS;
}

} // namespace nvcv::legacy::cuda_op
