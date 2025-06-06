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

#include <common/ValueTests.hpp>
#include <cvcuda/OpRotate.hpp>
#include <cvcuda/cuda_tools/MathWrappers.hpp>
#include <cvcuda/cuda_tools/SaturateCast.hpp>
#include <nvcv/Image.hpp>
#include <nvcv/ImageBatch.hpp>
#include <nvcv/Tensor.hpp>
#include <nvcv/TensorDataAccess.hpp>

#include <cmath>
#include <random>

namespace t    = ::testing;
namespace test = nvcv::test;
namespace cuda = nvcv::cuda;

#define PI 3.1415926535897932384626433832795

// #define DBG_ROTATE 1

static void compute_warpAffine(const double angle, const double xShift, const double yShift, double *aCoeffs)
{
    aCoeffs[0] = cos(angle * PI / 180);
    aCoeffs[1] = sin(angle * PI / 180);
    aCoeffs[2] = xShift;
    aCoeffs[3] = -sin(angle * PI / 180);
    aCoeffs[4] = cos(angle * PI / 180);
    aCoeffs[5] = yShift;
}

static void compute_center_shift(const int center_x, const int center_y, const double angle, double &xShift,
                                 double &yShift)
{
    xShift = (1 - cos(angle * PI / 180)) * center_x - sin(angle * PI / 180) * center_y;
    yShift = sin(angle * PI / 180) * center_x + (1 - cos(angle * PI / 180)) * center_y;
}

static void assignCustomValuesInSrc(std::vector<uint8_t> &srcVec, int srcWidth, int srcHeight, int srcVecRowStride)
{
    int initialValue = 1;
    int pixelBytes   = static_cast<int>(srcVecRowStride / srcWidth);
    for (int i = 0; i < srcHeight; i++)
    {
        for (int j = 0; j < srcVecRowStride; j = j + pixelBytes)
        {
            for (int k = 0; k < pixelBytes; k++)
            {
                srcVec[i * srcVecRowStride + j + k] = initialValue;
            }
            initialValue++;
        }
    }

#if DBG_ROTATE
    std::cout << "\nPrint input " << std::endl;

    for (int i = 0; i < srcHeight; i++)
    {
        for (int j = 0; j < srcVecRowStride; j++)
        {
            std::cout << static_cast<int>(srcVec[i * srcVecRowStride + j]) << ",";
        }
        std::cout << std::endl;
    }
#endif
}

template<typename T>
static void Rotate(std::vector<T> &hDst, int dstRowStride, nvcv::Size2D dstSize, const std::vector<T> &hSrc,
                   int srcRowStride, nvcv::Size2D srcSize, nvcv::ImageFormat fmt, const double angleDeg,
                   const double2 shift, NVCVInterpolationType interpolation)
{
    assert(fmt.numPlanes() == 1);

    int elementsPerPixel = fmt.numChannels();

    T       *dstPtr = hDst.data();
    const T *srcPtr = hSrc.data();

    // calculate coefficients
    double d_aCoeffs[6];
    compute_warpAffine(angleDeg, shift.x, shift.y, d_aCoeffs);

    int width  = dstSize.w;
    int height = dstSize.h;

    for (int dst_y = 0; dst_y < dstSize.h; dst_y++)
    {
        for (int dst_x = 0; dst_x < dstSize.w; dst_x++)
        {
            const double dst_x_shift = dst_x - d_aCoeffs[2];
            const double dst_y_shift = dst_y - d_aCoeffs[5];

            float src_x = (float)(dst_x_shift * d_aCoeffs[0] + dst_y_shift * (-d_aCoeffs[1]));
            float src_y = (float)(dst_x_shift * (-d_aCoeffs[3]) + dst_y_shift * d_aCoeffs[4]);

            if (interpolation == NVCV_INTERP_LINEAR)
            {
                if (src_x > -0.5 && src_x < width && src_y > -0.5 && src_y < height)
                {
                    const int x1 = cuda::round<cuda::RoundMode::DOWN, int>(src_x);
                    const int y1 = cuda::round<cuda::RoundMode::DOWN, int>(src_y);

                    const int x2      = x1 + 1;
                    const int y2      = y1 + 1;
                    const int x1_read = std::max(x1, 0);
                    const int y1_read = std::max(y1, 0);
                    const int x2_read = std::min(x2, width - 1);
                    const int y2_read = std::min(y2, height - 1);

                    for (int k = 0; k < elementsPerPixel; k++)
                    {
                        float out = 0.;

                        T src_reg = srcPtr[y1_read * srcRowStride + x1_read * elementsPerPixel + k];
                        out       = out + src_reg * ((x2 - src_x) * (y2 - src_y));

                        src_reg = srcPtr[y1_read * srcRowStride + x2_read * elementsPerPixel + k];
                        out     = out + src_reg * ((src_x - x1) * (y2 - src_y));

                        src_reg = srcPtr[y2_read * srcRowStride + x1_read * elementsPerPixel + k];
                        out     = out + src_reg * ((x2 - src_x) * (src_y - y1));

                        src_reg = srcPtr[y2_read * srcRowStride + x2_read * elementsPerPixel + k];
                        out     = out + src_reg * ((src_x - x1) * (src_y - y1));

                        dstPtr[dst_y * dstRowStride + dst_x * elementsPerPixel + k] = cuda::SaturateCast<T>(out);
                    }
                }
            }
            else if (interpolation == NVCV_INTERP_NEAREST || interpolation == NVCV_INTERP_CUBIC)
            {
                /*
                    Use this for NVCV_INTERP_CUBIC interpolation only for angles - {90, 180}
                */
                if (src_x > -0.5 && src_x < width && src_y > -0.5 && src_y < height)
                {
                    const int x1 = std::min(cuda::round<cuda::RoundMode::DOWN, int>(src_x + .5f), width - 1);
                    const int y1 = std::min(cuda::round<cuda::RoundMode::DOWN, int>(src_y + .5f), height - 1);

                    for (int k = 0; k < elementsPerPixel; k++)
                    {
                        dstPtr[dst_y * dstRowStride + dst_x * elementsPerPixel + k]
                            = srcPtr[y1 * srcRowStride + x1 * elementsPerPixel + k];
                    }
                }
            }
        }
    }
}

// clang-format off

NVCV_TEST_SUITE_P(OpRotate, test::ValueList<int, int, int, int, NVCVInterpolationType, int, double>
{
    // srcWidth, srcHeight, dstWidth, dstHeight,         interpolation, numberImages, angle
    {         4,         4,        4,         4,    NVCV_INTERP_NEAREST,           1,     90},
    {         4,         4,        4,         4,    NVCV_INTERP_NEAREST,           4,     90},
    {         5,         5,        5,         5,    NVCV_INTERP_LINEAR,            1,     90},
    {         5,         5,        5,         5,    NVCV_INTERP_LINEAR,            4,     90},

    {         4,         4,        4,         4,    NVCV_INTERP_NEAREST,           1,     45},
    {         4,         4,        4,         4,    NVCV_INTERP_NEAREST,           4,     45},
    {         5,         5,        5,         5,    NVCV_INTERP_LINEAR,            1,     45},
    {         5,         5,        5,         5,    NVCV_INTERP_LINEAR,            4,     45},

    {         4,         4,        4,         4,    NVCV_INTERP_CUBIC,             1,     90},
    {         4,         4,        4,         4,    NVCV_INTERP_CUBIC,             4,     90},
    {         5,         5,        5,         5,    NVCV_INTERP_CUBIC,             1,     90},
    {         5,         5,        5,         5,    NVCV_INTERP_CUBIC,             4,     90},

    {         4,         4,        4,         4,    NVCV_INTERP_CUBIC,             1,     180},
    {         4,         4,        4,         4,    NVCV_INTERP_CUBIC,             4,     180},
    {         5,         5,        5,         5,    NVCV_INTERP_CUBIC,             1,     180},
    {         5,         5,        5,         5,    NVCV_INTERP_CUBIC,             4,     180},
});

// clang-format on

TEST_P(OpRotate, tensor_correct_output)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    int srcWidth  = GetParamValue<0>();
    int srcHeight = GetParamValue<1>();
    int dstWidth  = GetParamValue<2>();
    int dstHeight = GetParamValue<3>();

    NVCVInterpolationType interpolation = GetParamValue<4>();

    int numberOfImages = GetParamValue<5>();

    double angleDeg = GetParamValue<6>();
    double shiftX   = -1;
    double shiftY   = -1;

    const nvcv::ImageFormat fmt = nvcv::FMT_RGB8;

    // Generate input
    nvcv::Tensor imgSrc(numberOfImages, {srcWidth, srcHeight}, fmt);

    auto srcData = imgSrc.exportData<nvcv::TensorDataStridedCuda>();

    ASSERT_NE(nullptr, srcData);

    auto srcAccess = nvcv::TensorDataAccessStridedImagePlanar::Create(*srcData);
    ASSERT_TRUE(srcAccess);

    std::vector<std::vector<uint8_t>> srcVec(numberOfImages);
    int                               srcVecRowStride = srcWidth * fmt.planePixelStrideBytes(0);

    for (int i = 0; i < numberOfImages; ++i)
    {
        srcVec[i].resize(srcHeight * srcVecRowStride);
        std::generate(srcVec[i].begin(), srcVec[i].end(), [&]() { return 0; });

        // Assign custom values in input vector
        assignCustomValuesInSrc(srcVec[i], srcWidth, srcHeight, srcVecRowStride);

        // Copy input data to the GPU
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(srcAccess->sampleData(i), srcAccess->rowStride(), srcVec[i].data(), srcVecRowStride,
                               srcVecRowStride, // vec has no padding
                               srcHeight, cudaMemcpyHostToDevice));
    }

    // Generate test result
    nvcv::Tensor imgDst(numberOfImages, {dstWidth, dstHeight}, fmt);

    // Compute shiftX, shiftY using center
    int center_x = (srcWidth - 1) / 2, center_y = (srcHeight - 1) / 2;
    compute_center_shift(center_x, center_y, angleDeg, shiftX, shiftY);

    cvcuda::Rotate RotateOp(0);
    double2        shift = {shiftX, shiftY};
    EXPECT_NO_THROW(RotateOp(stream, imgSrc, imgDst, angleDeg, shift, interpolation));

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
        std::generate(goldVec.begin(), goldVec.end(), [&]() { return 0; });

        // Generate gold result
        Rotate<uint8_t>(goldVec, dstVecRowStride, {dstWidth, dstHeight}, srcVec[i], srcVecRowStride,
                        {srcWidth, srcHeight}, fmt, angleDeg, shift, interpolation);

#if DBG_ROTATE
        std::cout << "\nPrint golden output " << std::endl;

        for (int k = 0; k < dstHeight; k++)
        {
            for (int j = 0; j < dstVecRowStride; j++)
            {
                std::cout << static_cast<int>(goldVec[k * dstVecRowStride + j]) << ",";
            }
            std::cout << std::endl;
        }

        std::cout << "\nPrint rotated output " << std::endl;

        for (int k = 0; k < dstHeight; k++)
        {
            for (int j = 0; j < dstVecRowStride; j++)
            {
                std::cout << static_cast<int>(testVec[k * dstVecRowStride + j]) << ",";
            }
            std::cout << std::endl;
        }
#endif

        EXPECT_EQ(goldVec, testVec);
    }
}

TEST_P(OpRotate, varshape_correct_output)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    int srcWidthBase  = GetParamValue<0>();
    int srcHeightBase = GetParamValue<1>();

    NVCVInterpolationType interpolation = GetParamValue<4>();

    int numberOfImages = GetParamValue<5>();

    double angleDegBase = GetParamValue<6>();

    const nvcv::ImageFormat fmt = nvcv::FMT_RGB8;

    // Create input and output
    std::default_random_engine         randEng;
    std::uniform_int_distribution<int> rndSrcWidth(srcWidthBase * 0.8, srcWidthBase * 1.1);
    std::uniform_int_distribution<int> rndSrcHeight(srcHeightBase * 0.8, srcHeightBase * 1.1);
    std::uniform_int_distribution<int> rndAngle(0, 360);

    nvcv::Tensor angleDegTensor(nvcv::TensorShape({numberOfImages}, "N"), nvcv::TYPE_F64);
    auto         angleDegTensorData = angleDegTensor.exportData<nvcv::TensorDataStridedCuda>();
    ASSERT_NE(nvcv::NullOpt, angleDegTensorData);

    nvcv::Tensor shiftTensor(nvcv::TensorShape({numberOfImages, 2}, nvcv::TENSOR_NW), nvcv::TYPE_F64);
    auto         shiftTensorData = shiftTensor.exportData<nvcv::TensorDataStridedCuda>();
    ASSERT_NE(nvcv::NullOpt, shiftTensorData);

    auto shiftTensorDataAccess = nvcv::TensorDataAccessStrided::Create(*shiftTensorData);
    ASSERT_TRUE(shiftTensorDataAccess);

    std::vector<nvcv::Image> imgSrc, imgDst;
    std::vector<double>      angleDegVecs;
    std::vector<double2>     shiftVecs;

    for (int i = 0; i < numberOfImages; ++i)
    {
        int tmpWidth  = i == 0 ? srcWidthBase : rndSrcWidth(randEng);
        int tmpHeight = i == 0 ? srcHeightBase : rndSrcHeight(randEng);

        imgSrc.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, fmt);

        imgDst.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, fmt);

        double2 shift    = {-1, -1};
        double  angleDeg = i == 0 ? angleDegBase : rndAngle(randEng);
        if (i != 0 && interpolation == NVCV_INTERP_CUBIC)
        {
            // Use the computed angle as rand int and
            // then compute index to pick one of the angles
            std::vector<double> tmpAngleValues = {90, 180, 270};
            size_t              indexToChoose  = static_cast<size_t>(angleDeg) % tmpAngleValues.size();
            angleDeg                           = tmpAngleValues[indexToChoose];
        }

        // Compute shiftX, shiftY using center
        int center_x = (tmpWidth - 1) / 2, center_y = (tmpHeight - 1) / 2;
        compute_center_shift(center_x, center_y, angleDeg, shift.x, shift.y);

        angleDegVecs.push_back(angleDeg);
        shiftVecs.push_back(shift);
    }

    ASSERT_EQ(cudaSuccess, cudaMemcpyAsync(angleDegTensorData->basePtr(), angleDegVecs.data(),
                                           angleDegVecs.size() * sizeof(double), cudaMemcpyHostToDevice, stream));

    ASSERT_EQ(cudaSuccess, cudaMemcpy2DAsync(shiftTensorDataAccess->sampleData(0),
                                             shiftTensorDataAccess->sampleStride(), shiftVecs.data(), sizeof(double2),
                                             sizeof(double2), numberOfImages, cudaMemcpyHostToDevice, stream));

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
        std::generate(srcVec[i].begin(), srcVec[i].end(), [&]() { return 0; });

        // Assign custom values in input vector
        assignCustomValuesInSrc(srcVec[i], srcWidth, srcHeight, srcRowStride);

        // Copy input data to the GPU
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(srcData->plane(0).basePtr, srcData->plane(0).rowStride, srcVec[i].data(), srcRowStride,
                               srcRowStride, // vec has no padding
                               srcHeight, cudaMemcpyHostToDevice));
    }

    // Generate test result
    cvcuda::Rotate rotateOp(numberOfImages);
    EXPECT_NO_THROW(rotateOp(stream, batchSrc, batchDst, angleDegTensor, shiftTensor, interpolation));

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
        int srcRowStride = dstWidth * fmt.planePixelStrideBytes(0);

        std::vector<uint8_t> testVec(dstHeight * dstRowStride);

        // Copy output data to Host
        ASSERT_EQ(cudaSuccess,
                  cudaMemcpy2D(testVec.data(), dstRowStride, dstData->plane(0).basePtr, dstData->plane(0).rowStride,
                               dstRowStride, // vec has no padding
                               dstHeight, cudaMemcpyDeviceToHost));

        std::vector<uint8_t> goldVec(dstHeight * dstRowStride);
        std::generate(goldVec.begin(), goldVec.end(), [&]() { return 0; });

        // Generate gold result
        Rotate<uint8_t>(goldVec, dstRowStride, {dstWidth, dstHeight}, srcVec[i], srcRowStride, {srcWidth, srcHeight},
                        fmt, angleDegVecs[i], shiftVecs[i], interpolation);

        EXPECT_EQ(goldVec, testVec);
    }
}

// clang-format off
NVCV_TEST_SUITE_P(OpRotate_Negative, test::ValueList<nvcv::ImageFormat, nvcv::ImageFormat, NVCVInterpolationType>{
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, NVCV_INTERP_LANCZOS},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8p, NVCV_INTERP_NEAREST},
    {nvcv::FMT_RGB8p, nvcv::FMT_RGB8p, NVCV_INTERP_NEAREST},
    {nvcv::FMT_RGBf16, nvcv::FMT_RGBf16, NVCV_INTERP_NEAREST},
});

NVCV_TEST_SUITE_P(OpRotateVarshape_Negative, test::ValueList<nvcv::ImageFormat, nvcv::ImageFormat, int, int, NVCVInterpolationType, nvcv::DataType, nvcv::DataType>{
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 2, 5, NVCV_INTERP_LANCZOS, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 6, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 2, -1, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8p, 2, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8p, nvcv::FMT_RGB8p, 2, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGBf16, nvcv::FMT_RGBf16, 2, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 2, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F32, nvcv::TYPE_F64},
    {nvcv::FMT_RGB8, nvcv::FMT_RGB8, 2, 5, NVCV_INTERP_NEAREST, nvcv::TYPE_F64, nvcv::TYPE_F32},
});

// clang-format on

TEST_P(OpRotate_Negative, op)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    nvcv::ImageFormat     inputFmt      = GetParamValue<0>();
    nvcv::ImageFormat     outputFmt     = GetParamValue<1>();
    NVCVInterpolationType interpolation = GetParamValue<2>();

    // Generate input
    nvcv::Tensor imgSrc(2, {4, 4}, inputFmt);
    // Generate test result
    nvcv::Tensor imgDst(2, {4, 4}, outputFmt);

    cvcuda::Rotate RotateOp(0);
    double         angleDeg = 90;
    double2        shift    = {-1, -1};
    EXPECT_EQ(NVCV_ERROR_INVALID_ARGUMENT,
              nvcv::ProtectCall([&] { RotateOp(stream, imgSrc, imgDst, angleDeg, shift, interpolation); }));

    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));
}

TEST_P(OpRotateVarshape_Negative, op)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    nvcv::ImageFormat     inputFmt             = GetParamValue<0>();
    nvcv::ImageFormat     outputFmt            = GetParamValue<1>();
    const int             numberOfImages       = GetParamValue<2>();
    const int             maxVarShapeBatchSize = GetParamValue<3>();
    NVCVInterpolationType interpolation        = GetParamValue<4>();
    nvcv::DataType        angleDataType        = GetParamValue<5>();
    nvcv::DataType        shiftDataType        = GetParamValue<6>();

    int srcWidthBase  = 4;
    int srcHeightBase = 4;

    // Create input and output
    std::default_random_engine         randEng;
    std::uniform_int_distribution<int> rndSrcWidth(srcWidthBase * 0.8, srcWidthBase * 1.1);
    std::uniform_int_distribution<int> rndSrcHeight(srcHeightBase * 0.8, srcHeightBase * 1.1);

    nvcv::Tensor angleDegTensor(nvcv::TensorShape({numberOfImages}, "N"), angleDataType);
    nvcv::Tensor shiftTensor(nvcv::TensorShape({numberOfImages, 2}, nvcv::TENSOR_NW), shiftDataType);

    std::vector<nvcv::Image> imgSrc, imgDst;

    for (int i = 0; i < numberOfImages; ++i)
    {
        int tmpWidth  = i == 0 ? srcWidthBase : rndSrcWidth(randEng);
        int tmpHeight = i == 0 ? srcHeightBase : rndSrcHeight(randEng);

        imgSrc.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, inputFmt);
        imgDst.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, outputFmt);
    }

    nvcv::ImageBatchVarShape batchSrc(numberOfImages);
    batchSrc.pushBack(imgSrc.begin(), imgSrc.end());

    nvcv::ImageBatchVarShape batchDst(numberOfImages);
    batchDst.pushBack(imgDst.begin(), imgDst.end());

    // Generate test result
    cvcuda::Rotate rotateOp(maxVarShapeBatchSize);
    EXPECT_EQ(
        NVCV_ERROR_INVALID_ARGUMENT,
        nvcv::ProtectCall([&] { rotateOp(stream, batchSrc, batchDst, angleDegTensor, shiftTensor, interpolation); }));

    // Get test data back
    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));
}

TEST(OpRotate_Negative, varshape_hasDifferentFormat)
{
    cudaStream_t stream;
    EXPECT_EQ(cudaSuccess, cudaStreamCreate(&stream));

    nvcv::ImageFormat     fmt            = nvcv::FMT_RGB8;
    const int             numberOfImages = 5;
    NVCVInterpolationType interpolation  = NVCV_INTERP_NEAREST;

    int srcWidthBase  = 4;
    int srcHeightBase = 4;

    std::vector<std::tuple<nvcv::ImageFormat, nvcv::ImageFormat>> testSet{
        {nvcv::FMT_RGBA8,             fmt},
        {            fmt, nvcv::FMT_RGBA8}
    };
    for (auto testCase : testSet)
    {
        nvcv::ImageFormat inputFmtExtra  = std::get<0>(testCase);
        nvcv::ImageFormat outputFmtExtra = std::get<1>(testCase);

        // Create input and output
        std::default_random_engine         randEng;
        std::uniform_int_distribution<int> rndSrcWidth(srcWidthBase * 0.8, srcWidthBase * 1.1);
        std::uniform_int_distribution<int> rndSrcHeight(srcHeightBase * 0.8, srcHeightBase * 1.1);

        nvcv::Tensor angleDegTensor(nvcv::TensorShape({numberOfImages}, "N"), nvcv::TYPE_F64);
        nvcv::Tensor shiftTensor(nvcv::TensorShape({numberOfImages, 2}, nvcv::TENSOR_NW), nvcv::TYPE_F64);

        std::vector<nvcv::Image> imgSrc, imgDst;

        for (int i = 0; i < numberOfImages - 1; ++i)
        {
            int tmpWidth  = i == 0 ? srcWidthBase : rndSrcWidth(randEng);
            int tmpHeight = i == 0 ? srcHeightBase : rndSrcHeight(randEng);

            imgSrc.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, fmt);
            imgDst.emplace_back(nvcv::Size2D{tmpWidth, tmpHeight}, fmt);
        }
        imgSrc.emplace_back(imgSrc[0].size(), inputFmtExtra);
        imgDst.emplace_back(imgSrc.back().size(), outputFmtExtra);

        nvcv::ImageBatchVarShape batchSrc(numberOfImages);
        batchSrc.pushBack(imgSrc.begin(), imgSrc.end());

        nvcv::ImageBatchVarShape batchDst(numberOfImages);
        batchDst.pushBack(imgDst.begin(), imgDst.end());

        // Generate test result
        cvcuda::Rotate rotateOp(numberOfImages);
        EXPECT_EQ(NVCV_ERROR_INVALID_ARGUMENT,
                  nvcv::ProtectCall(
                      [&] { rotateOp(stream, batchSrc, batchDst, angleDegTensor, shiftTensor, interpolation); }));
    }

    EXPECT_EQ(cudaSuccess, cudaStreamSynchronize(stream));
    EXPECT_EQ(cudaSuccess, cudaStreamDestroy(stream));
}

TEST(OpRotate_Negative, create_null_handle)
{
    EXPECT_EQ(cvcudaRotateCreate(nullptr, 2), NVCV_ERROR_INVALID_ARGUMENT);
}
