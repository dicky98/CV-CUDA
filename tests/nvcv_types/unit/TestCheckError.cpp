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
#include <nvcv/util/CheckError.hpp>

namespace gt   = ::testing;
namespace test = nvcv::test;
namespace priv = nvcv::priv;

// clang-format off
NVCV_TEST_SUITE_P(CheckErrorCudaConversionTests, test::ValueList<cudaError_t, NVCVStatus>
{
     { cudaErrorMemoryAllocation,   NVCV_ERROR_OUT_OF_MEMORY    },
     { cudaErrorNotReady,           NVCV_ERROR_NOT_READY        },
     { cudaErrorInvalidValue,       NVCV_ERROR_INVALID_ARGUMENT },
     { cudaErrorTextureFetchFailed, NVCV_ERROR_INTERNAL         }
});

// clang-format on

TEST_P(CheckErrorCudaConversionTests, check_conversion_to_nvcvstatus)
{
    cudaError_t errCuda = GetParamValue<0>();
    NVCVStatus  gold    = GetParamValue<1>();

    NVCV_EXPECT_STATUS(gold, NVCV_CHECK_THROW(errCuda));
}

TEST(CheckErrorCudaTests, success_no_throw)
{
    EXPECT_NO_THROW(NVCV_CHECK_THROW(cudaSuccess));
}

NVCV_TEST_SUITE_P(CheckStatusMacroTests, test::ValueList{NVCV_SUCCESS, NVCV_ERROR_NOT_READY, NVCV_ERROR_INTERNAL});

TEST_P(CheckStatusMacroTests, return_value)
{
    const NVCVStatus status = GetParam();

    int a = 0; // so that we have a colon in the macro

    NVCV_EXPECT_STATUS(status, [a, status] { return status; })
    NVCV_ASSERT_STATUS(status, [a, status] { return status; })
}

TEST_P(CheckStatusMacroTests, throw_return_void)
{
    const NVCVStatus status = GetParam();

    int a = 0; // so that we have a colon in the macro

    NVCV_EXPECT_STATUS(status, [a, status] { throw priv::Exception(status, "."); })
    NVCV_ASSERT_STATUS(status, [a, status] { throw priv::Exception(status, "."); })
}

TEST_P(CheckStatusMacroTests, throw_return_something_else)
{
    const NVCVStatus status = GetParam();

    int a = 0; // so that we have a colon in the macro

    NVCV_EXPECT_STATUS(status,
                       [a, status]
                       {
                           throw priv::Exception(status, ".");
                           return a;
                       })
    NVCV_ASSERT_STATUS(status,
                       [a, status]
                       {
                           throw priv::Exception(status, ".");
                           return a;
                       })
}

TEST(CheckStatusMacroTests, throw_with_extra_string)
{
    const cudaError_t cudaErrCode = cudaErrorTextureFetchFailed;
    const char       *fmt         = " Extra String: %s";
    const char       *extraString = "abc\n\0";

    NVCV_EXPECT_STATUS(NVCV_ERROR_INTERNAL, NVCV_CHECK_THROW(cudaErrCode, fmt, extraString));
}

TEST(CheckErrorTest, GetFunctionName)
{
    // valid
    EXPECT_EQ(nvcv::util::detail::FormatErrorMessage("customError", "func0()", ""), "(func0:customError)");
    EXPECT_EQ(nvcv::util::detail::FormatErrorMessage("customError", "func1(param1)", ""), "(func1:customError)");
    EXPECT_EQ(nvcv::util::detail::FormatErrorMessage("customError", "func2(param1, param2)", ""),
              "(func2:customError)");

    // invalid
    EXPECT_EQ(nvcv::util::detail::FormatErrorMessage("customError", "()", ""), "(customError)");
    EXPECT_EQ(nvcv::util::detail::FormatErrorMessage("customError", "invalidFuncName", ""), "(customError)");
}

TEST(CheckErrorTest, cudaErr_to_String)
{
    const char *errDesc = nullptr;
    auto        res     = nvcv::util::ToString(cudaErrorInvalidValue, &errDesc);

    EXPECT_STREQ(errDesc, "invalid argument");
    EXPECT_STREQ(res, "cudaErrorInvalidValue");
}
