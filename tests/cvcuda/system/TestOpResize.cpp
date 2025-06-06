/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
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

#include "Definitions.hpp"
#include "ResizeUtils.hpp"

#include <common/InterpUtils.hpp>
#include <common/TensorDataUtils.hpp>
#include <common/ValueTests.hpp>
#include <cvcuda/OpResize.hpp>
#include <cvcuda/cuda_tools/MathWrappers.hpp>
#include <cvcuda/cuda_tools/SaturateCast.hpp>
#include <nvcv/Image.hpp>
#include <nvcv/ImageBatch.hpp>
#include <nvcv/Tensor.hpp>
#include <nvcv/TensorDataAccess.hpp>

#include <cmath>
#include <random>

namespace cuda = nvcv::cuda;
namespace test = nvcv::test;
namespace t    = ::testing;

// clang-format off

#define NVCV_IMAGE_FORMAT_4U8 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, UNSIGNED, XYZW, ASSOCIATED, X8_Y8_Z8_W8)
#define NVCV_IMAGE_FORMAT_3U16 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, UNSIGNED, XYZ1, ASSOCIATED, X16_Y16_Z16)
#define NVCV_IMAGE_FORMAT_4U16 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, UNSIGNED, XYZW, ASSOCIATED, X16_Y16_Z16_W16)
#define NVCV_IMAGE_FORMAT_3S16 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, SIGNED, XYZ1, ASSOCIATED, X16_Y16_Z16)
#define NVCV_IMAGE_FORMAT_4S16 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, SIGNED, XYZW, ASSOCIATED, X16_Y16_Z16_W16)
#define NVCV_IMAGE_FORMAT_3F32 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, FLOAT, XYZ1, ASSOCIATED, X32_Y32_Z32)
#define NVCV_IMAGE_FORMAT_4F32 NVCV_DETAIL_MAKE_NONCOLOR_FMT1(PL, FLOAT, XYZW, ASSOCIATED, X32_Y32_Z32_W32)

NVCV_TEST_SUITE_P(OpResize, test::ValueList<int, int, int, int, NVCVInterpolationType, int, nvcv::ImageFormat>
{
    // srcWidth, srcHeight, dstWidth, dstHeight,       interpolation, numberImages,    imageFormat
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_RGBA8},
    {        113,       12,       12,        36, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_RGBA8},
    {        421,      148,      223,       124, NVCV_INTERP_NEAREST,           2,     nvcv::FMT_RGBA8},
    {        313,      212,      412,       336, NVCV_INTERP_NEAREST,           3,     nvcv::FMT_RGBA8},
    {         42,       40,       21,        20,  NVCV_INTERP_LINEAR,           1,     nvcv::FMT_RGBA8},
    {         21,       21,       42,        42,  NVCV_INTERP_LINEAR,           1,     nvcv::FMT_RGBA8},
    {        420,      420,      210,       210,  NVCV_INTERP_LINEAR,           4,     nvcv::FMT_RGBA8},
    {        210,      210,      420,       420,  NVCV_INTERP_LINEAR,           5,     nvcv::FMT_RGBA8},
    {         42,       40,       21,        20,   NVCV_INTERP_CUBIC,           1,     nvcv::FMT_RGBA8},
    {         21,       21,       42,        42,   NVCV_INTERP_CUBIC,           6,     nvcv::FMT_RGBA8},
    {        420,      420,      420,       420,   NVCV_INTERP_CUBIC,           2,     nvcv::FMT_RGBA8},
    {        420,      420,      420,       420,   NVCV_INTERP_CUBIC,           1,     nvcv::FMT_RGBA8},
    {        420,      420,       40,        42,   NVCV_INTERP_CUBIC,           1,     nvcv::FMT_RGBA8},
    {       1920,     1080,      640,       320,   NVCV_INTERP_CUBIC,           1,     nvcv::FMT_RGBA8},
    {       1920,     1080,      640,       320,   NVCV_INTERP_CUBIC,           2,     nvcv::FMT_RGBA8},
    {         44,       40,       22,        20,    NVCV_INTERP_AREA,           2,     nvcv::FMT_RGBA8},
    {         30,       30,       20,        20,    NVCV_INTERP_AREA,           2,     nvcv::FMT_RGBA8},
    {         30,       30,       60,        60,    NVCV_INTERP_AREA,           4,     nvcv::FMT_RGBA8},
    {       1080,     1920,      720,      1280,  NVCV_INTERP_LINEAR,           1,     nvcv::FMT_RGBA8},
    {        720,     1280,      480,       854,  NVCV_INTERP_CUBIC,            1,     nvcv::FMT_RGBA8},
    {       1440,     2560,     1080,      1920,  NVCV_INTERP_AREA,             1,     nvcv::FMT_RGBA8},
    {       2160,     3840,     1080,      1920,  NVCV_INTERP_LINEAR,           1,     nvcv::FMT_RGBA8},
    {       1080,     1920,      540,       960,  NVCV_INTERP_CUBIC,            1,     nvcv::FMT_RGBA8},
    {        720,     1280,      360,       640,  NVCV_INTERP_AREA,             1,     nvcv::FMT_RGBA8},
    {       2160,     3840,     1440,      2560,  NVCV_INTERP_LINEAR,           1,     nvcv::FMT_RGBA8},
    {       1080,     1920,      360,       640,  NVCV_INTERP_CUBIC,            1,     nvcv::FMT_RGBA8},
    {       1440,     2560,      720,      1280,  NVCV_INTERP_AREA,             1,     nvcv::FMT_RGBA8},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_U8},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_RGB8},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_4U8}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_U16},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_3U16}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_4U16}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_S16},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_3S16}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_4S16}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::FMT_F32},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_3F32}},
    {         42,       48,       23,        24, NVCV_INTERP_NEAREST,           1,     nvcv::ImageFormat{NVCV_IMAGE_FORMAT_4F32}},
});

#undef NVCV_IMAGE_FORMAT_4U8
#undef NVCV_IMAGE_FORMAT_3U16
#undef NVCV_IMAGE_FORMAT_4U16
#undef NVCV_IMAGE_FORMAT_3S16
#undef NVCV_IMAGE_FORMAT_4S16
#undef NVCV_IMAGE_FORMAT_3F32
#undef NVCV_IMAGE_FORMAT_4F32

// clang-format oon

TEST_P(OpResize, tensor_correct_output)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    int srcWidth  = GetParamValue<0>();
    int srcHeight = GetParamValue<1>();
    int dstWidth  = GetParamValue<2>();
    int dstHeight = GetParamValue<3>();

    NVCVInterpolationType interpolation = GetParamValue<4>();

    int numberOfImages = GetParamValue<5>();

    const nvcv::ImageFormat fmt = GetParamValue<6>();

    // Generate input
    nvcv::Tensor imgSrc = nvcv::util::CreateTensor(numberOfImages, srcWidth, srcHeight, fmt);

    auto srcData = imgSrc.exportData<nvcv::TensorDataStridedCuda>();

    ASSERT_NE(nullptr, srcData);

    auto srcAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(*srcData);
    ASSERT_TRUE(srcAccess);

    std::vector<std::vector<uint8_t>> srcVec(numberOfImages);
    int                               srcVecRowStride = srcWidth * fmt.planePixelStrideBytes(0);

    std::default_random_engine randEng;

    for (int i = 0; i < numberOfImages; ++i)
    {
        std::uniform_int_distribution<uint8_t> rand(0, 255);

        srcVec[i].resize(srcHeight * srcVecRowStride);
        std::generate(srcVec[i].begin(), srcVec[i].end(), [&]() { return rand(randEng); });

        // Copy input data to the GPU
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(srcAccess->sampleData(i), srcAccess->rowStride(), srcVec[i].data(), srcVecRowStride,
                               srcVecRowStride, // vec has no padding
                               srcHeight, cudaMemcpyHostToDevice));
    }

    // Generate test result
    nvcv::Tensor imgDst = nvcv::util::CreateTensor(numberOfImages, dstWidth, dstHeight, fmt);

    cvcuda::Resize resizeOp;
    EXPECT_NO_THROW(resizeOp(stream, imgSrc, imgDst, interpolation));

    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));

    // Check result
    auto dstData = imgDst.exportData<nvcv::TensorDataStridedCuda>();
    ASSERT_NE(nullptr, dstData);

    auto dstAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(*dstData);
    ASSERT_TRUE(dstAccess);

    int dstVecRowStride = dstWidth * fmt.planePixelStrideBytes(0);
    for (int i = 0; i < numberOfImages; ++i)
    {
        SCOPED_TRACE(i);

        std::vector<uint8_t> testVec(dstHeight * dstVecRowStride);

        // Copy output data to Host
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(testVec.data(), dstVecRowStride, dstAccess->sampleData(i), dstAccess->rowStride(),
                               dstVecRowStride, // vec has no padding
                               dstHeight, cudaMemcpyDeviceToHost));

        std::vector<uint8_t> goldVec(dstHeight * dstVecRowStride);

        // Generate gold result
        test::Resize(goldVec, dstVecRowStride, {dstWidth, dstHeight}, srcVec[i], srcVecRowStride, {srcWidth, srcHeight},
                     fmt, interpolation, false);

        std::vector<int> mae(testVec.size());
        for (size_t i = 0; i < mae.size(); ++i)
        {
            mae[i] = abs(static_cast<int>(goldVec[i]) - static_cast<int>(testVec[i]));
        }

        int maeThreshold = 1;

        EXPECT_THAT(mae, t::Each(t::Le(maeThreshold)));
    }
}

TEST_P(OpResize, varshape_correct_output)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    int srcWidthBase  = GetParamValue<0>();
    int srcHeightBase = GetParamValue<1>();
    int dstWidthBase  = GetParamValue<2>();
    int dstHeightBase = GetParamValue<3>();

    NVCVInterpolationType interpolation = GetParamValue<4>();

    int numberOfImages = GetParamValue<5>();

    const nvcv::ImageFormat fmt = GetParamValue<6>();

    // Create input and output
    std::default_random_engine         randEng;
    std::uniform_int_distribution<int> rndSrcWidth(srcWidthBase * 0.8, srcWidthBase * 1.1);
    std::uniform_int_distribution<int> rndSrcHeight(srcHeightBase * 0.8, srcHeightBase * 1.1);

    std::uniform_int_distribution<int> rndDstWidth(dstWidthBase * 0.8, dstWidthBase * 1.1);
    std::uniform_int_distribution<int> rndDstHeight(dstHeightBase * 0.8, dstHeightBase * 1.1);

    std::vector<nvcv::Image> imgSrc, imgDst;
    // The size of the first image is fixed: to cover area fast code path
    imgSrc.emplace_back(nvcv::Size2D{srcWidthBase, srcHeightBase}, fmt);
    imgDst.emplace_back(nvcv::Size2D{dstHeightBase, dstHeightBase}, fmt);
    for (int i = 0; i < numberOfImages - 1; ++i)
    {
        imgSrc.emplace_back(nvcv::Size2D{rndSrcWidth(randEng), rndSrcHeight(randEng)}, fmt);
        imgDst.emplace_back(nvcv::Size2D{rndDstWidth(randEng), rndDstHeight(randEng)}, fmt);
    }

    nvcv::ImageBatchVarShape batchSrc(numberOfImages);
    batchSrc.pushBack(imgSrc.begin(), imgSrc.end());

    nvcv::ImageBatchVarShape batchDst(numberOfImages);
    batchDst.pushBack(imgDst.begin(), imgDst.end());

    std::vector<std::vector<uint8_t>> srcVec(numberOfImages);
    std::vector<int>                  srcVecRowStride(numberOfImages);

    // Populate input
    for (int i = 0; i < numberOfImages; ++i)
    {
        const auto srcData = imgSrc[i].exportData<nvcv::ImageDataStridedCuda>();
        assert(srcData->numPlanes() == 1);

        int srcWidth  = srcData->plane(0).width;
        int srcHeight = srcData->plane(0).height;

        int srcRowStride = srcWidth * fmt.planePixelStrideBytes(0);

        srcVecRowStride[i] = srcRowStride;

        std::uniform_int_distribution<uint8_t> rand(0, 255);

        srcVec[i].resize(srcHeight * srcRowStride);
        std::generate(srcVec[i].begin(), srcVec[i].end(), [&]() { return rand(randEng); });

        // Copy input data to the GPU
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(srcData->plane(0).basePtr, srcData->plane(0).rowStride, srcVec[i].data(), srcRowStride,
                               srcRowStride, // vec has no padding
                               srcHeight, cudaMemcpyHostToDevice));
    }

    // Generate test result
    cvcuda::Resize resizeOp;
    EXPECT_NO_THROW(resizeOp(stream, batchSrc, batchDst, interpolation));

    // Get test data back
    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));

    // Check test data against gold
    for (int i = 0; i < numberOfImages; ++i)
    {
        SCOPED_TRACE(i);

        const auto srcData = imgSrc[i].exportData<nvcv::ImageDataStridedCuda>();
        assert(srcData->numPlanes() == 1);
        int srcWidth  = srcData->plane(0).width;
        int srcHeight = srcData->plane(0).height;

        const auto dstData = imgDst[i].exportData<nvcv::ImageDataStridedCuda>();
        assert(dstData->numPlanes() == 1);

        int dstWidth  = dstData->plane(0).width;
        int dstHeight = dstData->plane(0).height;

        int dstRowStride = dstWidth * fmt.planePixelStrideBytes(0);

        std::vector<uint8_t> testVec(dstHeight * dstRowStride);

        // Copy output data to Host
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(testVec.data(), dstRowStride, dstData->plane(0).basePtr, dstData->plane(0).rowStride,
                               dstRowStride, // vec has no padding
                               dstHeight, cudaMemcpyDeviceToHost));

        std::vector<uint8_t> goldVec(dstHeight * dstRowStride);

        // Generate gold result
        test::Resize(goldVec, dstRowStride, {dstWidth, dstHeight}, srcVec[i], srcVecRowStride[i], {srcWidth, srcHeight},
                     fmt, interpolation, true);

        // maximum absolute error
        std::vector<int> mae(testVec.size());
        for (size_t i = 0; i < mae.size(); ++i)
        {
            mae[i] = abs(static_cast<int>(goldVec[i]) - static_cast<int>(testVec[i]));
        }

        int maeThreshold = 1;

        EXPECT_THAT(mae, t::Each(t::Le(maeThreshold)));
    }
}

// clang-format off
NVCV_TEST_SUITE_P(OpResize_Negative, test::ValueList<nvcv::ImageFormat, nvcv::ImageFormat, int, int, NVCVInterpolationType>{
    {nvcv::FMT_U8, nvcv::FMT_U16, 1, 1, NVCV_INTERP_NEAREST}, // in/out image data type not same
    {nvcv::FMT_U8, nvcv::FMT_RGB8p, 1, 1, NVCV_INTERP_NEAREST}, // in/out image layout not same
    {nvcv::FMT_RGB8p, nvcv::FMT_U8, 1, 1, NVCV_INTERP_NEAREST}, // in/out image layout not NHWC
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 1, 2, NVCV_INTERP_NEAREST}, // in/out image num are different
    {nvcv::FMT_U8, nvcv::FMT_RGB8, 1, 1, NVCV_INTERP_NEAREST}, // in/out image channels are different
    {nvcv::FMT_F16, nvcv::FMT_F16, 1, 1, NVCV_INTERP_NEAREST}, // invalid datatype
    {nvcv::FMT_F16, nvcv::FMT_F16, 1, 1, NVCV_INTERP_HAMMING}, // invalid interpolation
});

// clang-format on

TEST_P(OpResize_Negative, op)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    int srcWidth  = 42;
    int srcHeight = 48;
    int dstWidth  = 23;
    int dstHeight = 24;

    NVCVInterpolationType interpolation = NVCV_INTERP_NEAREST;

    const nvcv::ImageFormat inputFmt        = GetParamValue<0>();
    const nvcv::ImageFormat outputFmt       = GetParamValue<1>();
    int                     numInputImages  = GetParamValue<2>();
    int                     numOutputImages = GetParamValue<3>();

    // Generate input
    nvcv::Tensor imgSrc = nvcv::util::CreateTensor(numInputImages, srcWidth, srcHeight, inputFmt);

    // Generate test result
    nvcv::Tensor imgDst = nvcv::util::CreateTensor(numOutputImages, dstWidth, dstHeight, outputFmt);

    cvcuda::Resize resizeOp;
    EXPECT_EQ(NVCV_ERROR_INVALID_ARGUMENT, nvcv::ProtectCall([&] { resizeOp(stream, imgSrc, imgDst, interpolation); }));

    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));
}
