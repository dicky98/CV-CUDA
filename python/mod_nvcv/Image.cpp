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

#include "Image.hpp"

#include "Cache.hpp"
#include "CastUtils.hpp"
#include "DataType.hpp"
#include "ImageFormat.hpp"
#include "Stream.hpp"

#include <common/Assert.hpp>
#include <common/CheckError.hpp>
#include <common/PyUtil.hpp>
#include <common/String.hpp>
#include <dlpack/dlpack.h>
#include <nvcv/TensorLayout.hpp>
#include <nvcv/TensorShapeInfo.hpp>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace nvcvpy::priv {

bool Image::Key::doIsCompatible(const IKey &ithat) const
{
    auto &that = static_cast<const Key &>(ithat);

    // Wrapper key's all compare equal, are they can't be used
    // and whenever we query the cache for wrappers, we really
    // want to get them all (as long as they aren't being used).
    if (m_isWrapper && that.m_isWrapper)
    {
        return true;
    }
    else if (m_isWrapper || that.m_isWrapper) // xor
    {
        return false;
    }
    else
    {
        return std::tie(m_size, m_format) == std::tie(that.m_size, that.m_format);
    }
}

size_t Image::Key::doGetHash() const
{
    if (m_isWrapper)
    {
        return 0; // all wrappers are equal wrt. the cache
    }
    else
    {
        using util::ComputeHash;
        return ComputeHash(m_size, m_format);
    }
}

namespace {

struct BufferImageInfo
{
    int            numPlanes;
    nvcv::Size2D   size;
    int            numChannels;
    bool           isChannelLast;
    int64_t        planeStride, rowStride;
    nvcv::DataType dtype;
    void          *data;
};

std::vector<BufferImageInfo> ExtractBufferImageInfo(const std::vector<DLPackTensor> &tensorList,
                                                    const nvcv::ImageFormat         &fmt)
{
    std::vector<BufferImageInfo> bufferInfoList;

    int curChannel = 0;

    // For each buffer,
    for (size_t p = 0; p < tensorList.size(); ++p)
    {
        const DLTensor &tensor = *tensorList[p];

        int elemStrideBytes = (tensor.dtype.bits * tensor.dtype.lanes + 7) / 8;

        // Extract 4d shape and layout regardless of rank
        ssize_t            shape[4];
        ssize_t            strides[4];
        nvcv::TensorLayout layout;

        switch (tensor.ndim)
        {
        case 1:
            layout = nvcv::TENSOR_NCHW;

            shape[0] = 1;
            shape[1] = 1;
            shape[2] = 1;
            shape[3] = tensor.shape[0];

            strides[0] = tensor.strides[0] * elemStrideBytes;
            strides[1] = strides[0];
            strides[2] = strides[0];
            strides[3] = strides[0];
            break;

        case 2:
            layout = nvcv::TENSOR_NCHW;

            shape[0] = 1;
            shape[1] = 1;
            shape[2] = tensor.shape[0];
            shape[3] = tensor.shape[1];

            strides[0] = tensor.shape[0] * tensor.strides[0] * elemStrideBytes;
            strides[1] = strides[0];
            strides[2] = tensor.strides[0] * elemStrideBytes;
            strides[3] = tensor.strides[1] * elemStrideBytes;
            break;

        case 3:
        case 4:
            shape[0] = tensor.ndim == 3 ? 1 : tensor.shape[tensor.ndim - 4];
            shape[1] = tensor.shape[tensor.ndim - 3];
            shape[2] = tensor.shape[tensor.ndim - 2];
            shape[3] = tensor.shape[tensor.ndim - 1];

            // User has specified a format?
            if (fmt != nvcv::FMT_NONE)
            {
                // Use it to disambiguate
                if (fmt.planeNumChannels(p) == shape[3])
                {
                    layout = nvcv::TENSOR_NHWC;
                }
                else
                {
                    layout = nvcv::TENSOR_NCHW;
                }
            }
            else
            {
                // Or else,
                if (shape[3] <= 4) // (C<=4)
                {
                    layout = nvcv::TENSOR_NHWC;
                }
                else
                {
                    layout = nvcv::TENSOR_NCHW;
                }
            }

            strides[1] = tensor.strides[tensor.ndim - 3] * elemStrideBytes;
            strides[2] = tensor.strides[tensor.ndim - 2] * elemStrideBytes;
            strides[3] = tensor.strides[tensor.ndim - 1] * elemStrideBytes;

            if (tensor.ndim == 3)
            {
                strides[0] = shape[1] * strides[1];
            }
            else
            {
                strides[0] = tensor.strides[tensor.ndim - 4];
            }
            break;

        default:
            throw std::invalid_argument(
                util::FormatString("Number of buffer dimensions must be between 1 and 4, not %d", tensor.ndim));
        }

        // Validate strides -----------------------

        if (strides[0] <= 0 || strides[1] <= 0 || strides[2] <= 0)
        {
            throw std::invalid_argument("Buffer strides must be all >= 1");
        }

        NVCV_ASSERT(layout.rank() == 4);

        auto infoShape = nvcv::TensorShapeInfoImagePlanar::Create(nvcv::TensorShape(shape, 4, layout));
        NVCV_ASSERT(infoShape);

        const auto *infoLayout = &infoShape->infoLayout();

        if (strides[3] != elemStrideBytes)
        {
            throw std::invalid_argument(util::FormatString(
                "Fastest changing dimension must be packed, i.e., have stride equal to %d byte(s), not %ld",
                elemStrideBytes, strides[2]));
        }

        ssize_t packedRowStride = static_cast<ssize_t>(elemStrideBytes) * infoShape->numCols();
        ssize_t rowStride       = strides[infoLayout->idxHeight()];
        if (!infoLayout->isChannelLast() && rowStride != packedRowStride)
        {
            throw std::invalid_argument(util::FormatString(
                "Image row must packed, i.e., have stride equal to %ld byte(s), not %ld", packedRowStride, rowStride));
        }

        bufferInfoList.emplace_back();

        BufferImageInfo &bufInfo = bufferInfoList.back();
        bufInfo.isChannelLast    = infoLayout->isChannelLast();
        bufInfo.numPlanes        = bufInfo.isChannelLast ? infoShape->numSamples() : infoShape->numChannels();
        bufInfo.numChannels      = infoShape->numChannels();
        bufInfo.size             = infoShape->size();
        bufInfo.planeStride      = strides[bufInfo.isChannelLast ? infoLayout->idxSample() : infoLayout->idxChannel()];
        bufInfo.rowStride        = strides[infoLayout->idxHeight()];
        bufInfo.data             = tensor.data;
        bufInfo.dtype            = ToNVCVDataType(tensor.dtype);

        curChannel += bufInfo.numPlanes * bufInfo.numChannels;
        if (curChannel > 4)
        {
            throw std::invalid_argument("Number of channels specified in a buffers must be <= 4");
        }

        NVCV_ASSERT(bufInfo.numPlanes <= 4);
        NVCV_ASSERT(bufInfo.numChannels <= 4);
    }

    return bufferInfoList;
}

nvcv::DataType MakePackedType(nvcv::DataType dtype, int numChannels)
{
    if (dtype.numChannels() == numChannels)
    {
        return dtype;
    }
    else
    {
        NVCV_ASSERT(2 <= numChannels && numChannels <= 4);

        nvcv::PackingParams pp = GetParams(dtype.packing());

        switch (numChannels)
        {
        case 2:
            pp.swizzle = nvcv::Swizzle::S_XY00;
            break;
        case 3:
            pp.swizzle = nvcv::Swizzle::S_XYZ0;
            break;
        case 4:
            pp.swizzle = nvcv::Swizzle::S_XYZW;
            break;
        }
        pp.byteOrder = nvcv::ByteOrder::MSB;
        for (int i = 1; i < numChannels; ++i)
        {
            pp.bits[i] = pp.bits[0];
        }

        nvcv::Packing newPacking = MakePacking(pp);
        return nvcv::DataType{dtype.dataKind(), newPacking};
    }
}

nvcv::ImageFormat InferImageFormat(const std::vector<nvcv::DataType> &planePixTypes)
{
    if (planePixTypes.empty())
    {
        return nvcv::FMT_NONE;
    }

    static_assert(NVCV_PACKING_0 == 0, "Invalid 0 packing value");
    NVCV_ASSERT(planePixTypes.size() <= 4);

    nvcv::Packing packing[4] = {nvcv::Packing::NONE};

    int numChannels = 0;

    for (size_t p = 0; p < planePixTypes.size(); ++p)
    {
        packing[p] = planePixTypes[p].packing();
        numChannels += planePixTypes[p].numChannels();

        if (planePixTypes[p].dataKind() != planePixTypes[0].dataKind())
        {
            throw std::invalid_argument("Planes must all have the same data type");
        }
    }

    nvcv::DataKind dataKind = planePixTypes[0].dataKind();

    int numPlanes = planePixTypes.size();

    // Planar or packed?
    if (numPlanes == 1 || numChannels == numPlanes)
    {
        nvcv::ImageFormat baseFormatList[4] = {nvcv::FMT_U8, nvcv::FMT_2F32, nvcv::FMT_RGB8, nvcv::FMT_RGBA8};

        NVCV_ASSERT(numChannels <= 4);
        nvcv::ImageFormat baseFormat = baseFormatList[numChannels - 1];

        nvcv::ColorModel model = baseFormat.colorModel();
        switch (model)
        {
        case nvcv::ColorModel::YCbCr:
            return nvcv::ImageFormat(baseFormat.colorSpec(), baseFormat.chromaSubsampling(), baseFormat.memLayout(),
                                     dataKind, baseFormat.swizzle(), packing[0], packing[1], packing[2], packing[3]);

        case nvcv::ColorModel::UNDEFINED:
            return nvcv::ImageFormat(baseFormat.memLayout(), dataKind, baseFormat.swizzle(), packing[0], packing[1],
                                     packing[2], packing[3]);
        case nvcv::ColorModel::RAW:
            return nvcv::ImageFormat(baseFormat.rawPattern(), baseFormat.memLayout(), dataKind, baseFormat.swizzle(),
                                     packing[0], packing[1], packing[2], packing[3]);
        default:
            return nvcv::ImageFormat(model, baseFormat.colorSpec(), baseFormat.memLayout(), dataKind,
                                     baseFormat.swizzle(), packing[0], packing[1], packing[2], packing[3]);
        }
    }
    // semi-planar, NV12-like?
    // TODO: this test is too fragile, must improve
    else if (numPlanes == 2 && numChannels == 3)
    {
        return nvcv::FMT_NV12_ER.dataKind(dataKind).swizzleAndPacking(nvcv::Swizzle::S_XYZ0, packing[0], packing[1],
                                                                      packing[2], packing[3]);
    }
    // Or else, we'll consider it as representing a non-color format
    else
    {
        // clang-format off
        nvcv::Swizzle sw = MakeSwizzle(numChannels >= 1 ? nvcv::Channel::X : nvcv::Channel::NONE,
                                     numChannels >= 2 ? nvcv::Channel::Y : nvcv::Channel::NONE,
                                     numChannels >= 3 ? nvcv::Channel::Z : nvcv::Channel::NONE,
                                     numChannels >= 4 ? nvcv::Channel::W : nvcv::Channel::NONE);
        // clang-format on

        return nvcv::FMT_U8.dataKind(dataKind).swizzleAndPacking(sw, packing[0], packing[1], packing[2], packing[3]);
    }
}

void FillNVCVImageBufferStrided(NVCVImageData &imgData, const std::vector<DLPackTensor> &infos, nvcv::ImageFormat fmt)
{
    // If user passes an image format, we must check if the given buffers are consistent with it.
    // Otherwise, we need to infer the image format from the given buffers.

    // Here's the plan:
    // 1. Loop through all buffers and infer its dimensions, number of channels and data type.
    //    In case of ambiguity in inferring data type for a buffer,
    //    - If available, use given image format for disambiguation
    //    - Otherwise, if number of channels in last dimension is <= 4, treat it as packed, or else it's planar
    // 2. Validate the data collected to see if it represents a real image format
    // 3. If available, compare the given image format with the inferred one, they're data layout must be the same.

    // Let the games begin.

    NVCVImageBufferStrided &dataStrided = imgData.buffer.strided;

    dataStrided = {}; // start anew

    std::vector<BufferImageInfo> bufferInfoList = ExtractBufferImageInfo(infos, fmt);
    std::vector<nvcv::DataType>  planeDataTypes;

    int curPlane = 0;
    for (const BufferImageInfo &b : bufferInfoList)
    {
        for (int p = 0; p < b.numPlanes; ++p, ++curPlane)
        {
            NVCV_ASSERT(curPlane <= 4);

            dataStrided.planes[curPlane].width     = b.size.w;
            dataStrided.planes[curPlane].height    = b.size.h;
            dataStrided.planes[curPlane].rowStride = b.rowStride;
            dataStrided.planes[curPlane].basePtr   = reinterpret_cast<NVCVByte *>(b.data) + b.planeStride * p;

            planeDataTypes.push_back(MakePackedType(b.dtype, b.isChannelLast ? b.numChannels : 1));
        }
    }
    dataStrided.numPlanes = curPlane;

    if (dataStrided.numPlanes == 0)
    {
        throw std::invalid_argument("Number of planes must be >= 1");
    }

    nvcv::ImageFormat inferredFormat = InferImageFormat(planeDataTypes);

    nvcv::ImageFormat finalFormat;

    // User explicitely specifies the image format?
    if (fmt != nvcv::FMT_NONE)
    {
        if (!HasSameDataLayout(fmt, inferredFormat))
        {
            throw std::invalid_argument(
                util::FormatString("Format inferred from buffers %s isn't compatible with given image format %s",
                                   util::ToString(inferredFormat).c_str(), util::ToString(fmt).c_str()));
        }
        finalFormat = fmt;
    }
    else
    {
        finalFormat = inferredFormat;
    }
    imgData.format = finalFormat;

    nvcv::Size2D imgSize = {dataStrided.planes[0].width, dataStrided.planes[0].height};

    // Now do a final check on the expected plane sizes according to the
    // format
    for (int p = 0; p < dataStrided.numPlanes; ++p)
    {
        nvcv::Size2D goldSize = finalFormat.planeSize(imgSize, p);
        nvcv::Size2D plSize{dataStrided.planes[p].width, dataStrided.planes[p].height};

        if (plSize.w != goldSize.w || plSize.h != goldSize.h)
        {
            throw std::invalid_argument(util::FormatString(
                "Plane %d's size %dx%d doesn't correspond to what's expected by %s format %s of image with size %dx%d",
                p, plSize.w, plSize.h, (fmt == nvcv::FMT_NONE ? "inferred" : "given"),
                util::ToString(finalFormat).c_str(), imgSize.w, imgSize.h));
        }
    }
}

nvcv::ImageDataStridedCuda CreateNVCVImageDataCuda(const std::vector<DLPackTensor> &infos, nvcv::ImageFormat fmt)
{
    NVCVImageData imgData;
    FillNVCVImageBufferStrided(imgData, infos, fmt);

    return nvcv::ImageDataStridedCuda(nvcv::ImageFormat{imgData.format}, imgData.buffer.strided);
}

nvcv::ImageDataStridedHost CreateNVCVImageDataHost(const std::vector<DLPackTensor> &infos, nvcv::ImageFormat fmt)
{
    NVCVImageData imgData;
    FillNVCVImageBufferStrided(imgData, infos, fmt);

    return nvcv::ImageDataStridedHost(nvcv::ImageFormat{imgData.format}, imgData.buffer.strided);
}

} // namespace

Image::Image(const Size2D &size, nvcv::ImageFormat fmt, int rowAlign)
{
    nvcv::MemAlignment    bufAlign = rowAlign == 0 ? nvcv::MemAlignment{} : nvcv::MemAlignment{}.rowAddr(rowAlign);
    NVCVImageRequirements reqs;

    nvcvImageCalcRequirements(std::get<0>(size), std::get<1>(size), fmt, bufAlign.baseAddr(), bufAlign.rowAddr(),
                              &reqs);

    m_impl         = nvcv::Image(reqs, nullptr /* allocator */);
    m_key          = Key{size, fmt};
    m_size_inbytes = doComputeSizeInBytes(reqs);
}

Image::Image(std::vector<std::shared_ptr<ExternalBuffer>> bufs, const nvcv::ImageDataStridedCuda &imgData)
    : m_key{} // it's a wrap!
    , m_size_inbytes{doComputeSizeInBytes(NVCVImageRequirements())}
{
    m_wrapData.emplace();

    this->setWrapData(std::move(bufs), imgData);
}

Image::Image(std::vector<py::buffer> bufs, const nvcv::ImageDataStridedHost &hostData, int rowAlign)
{
    // Input buffer is host data.
    // We'll create a regular image and copy the host data into it.

    // Create the image with same size and format as host data
    nvcv::MemAlignment    bufAlign = nvcv::MemAlignment{}.rowAddr(rowAlign);
    NVCVImageRequirements reqs;

    nvcvImageCalcRequirements(hostData.size().w, hostData.size().h, hostData.format(), bufAlign.baseAddr(),
                              bufAlign.rowAddr(), &reqs);

    m_impl         = nvcv::Image(reqs, nullptr /* allocator */);
    m_size_inbytes = doComputeSizeInBytes(reqs);

    auto devData = *m_impl.exportData<nvcv::ImageDataStridedCuda>();
    NVCV_ASSERT(hostData.format() == devData.format());
    NVCV_ASSERT(hostData.numPlanes() == devData.numPlanes());

    // Now copy each plane from host to device
    for (int p = 0; p < devData.numPlanes(); ++p)
    {
        const nvcv::ImagePlaneStrided &devPlane  = devData.plane(p);
        const nvcv::ImagePlaneStrided &hostPlane = hostData.plane(p);

        NVCV_ASSERT(devPlane.width == hostPlane.width);
        NVCV_ASSERT(devPlane.height == hostPlane.height);

        util::CheckThrow(cudaMemcpy2D(devPlane.basePtr, devPlane.rowStride, hostPlane.basePtr, hostPlane.rowStride,
                                      static_cast<size_t>(hostPlane.width) * hostData.format().planePixelStrideBytes(p),
                                      hostPlane.height, cudaMemcpyHostToDevice));
    }

    m_key = Key{
        {m_impl.size().w, m_impl.size().h},
        m_impl.format()
    };
}

int64_t Image::doComputeSizeInBytes(const NVCVImageRequirements &reqs)
{
    int64_t size_inbytes;
    util::CheckThrow(nvcvMemRequirementsCalcTotalSizeBytes(&(reqs.mem.cudaMem), &size_inbytes));
    return size_inbytes;
}

int64_t Image::GetSizeInBytes() const
{
    // m_size_inbytes == -1 indicates failure case and value has not been computed yet
    NVCV_ASSERT(m_size_inbytes != -1 && "Image has m_size_inbytes == -1, ie m_size_inbytes has not been correctly set");
    return m_size_inbytes;
}

std::shared_ptr<Image> Image::shared_from_this()
{
    return std::static_pointer_cast<Image>(Container::shared_from_this());
}

std::shared_ptr<const Image> Image::shared_from_this() const
{
    return std::static_pointer_cast<const Image>(Container::shared_from_this());
}

std::shared_ptr<Image> Image::Create(const Size2D &size, nvcv::ImageFormat fmt, int rowAlign)
{
    std::vector<std::shared_ptr<CacheItem>> vcont = Cache::Instance().fetch(Key{size, fmt});

    // None found?
    if (vcont.empty())
    {
        std::shared_ptr<Image> img(new Image(size, fmt, rowAlign));
        Cache::Instance().add(*img);
        return img;
    }
    else
    {
        // Get the first one
        return std::static_pointer_cast<Image>(vcont[0]);
    }
}

std::shared_ptr<Image> Image::Zeros(const Size2D &size, nvcv::ImageFormat fmt, int rowAlign)
{
    auto img = Image::Create(size, fmt, rowAlign);

    auto data = *img->impl().exportData<nvcv::ImageDataStridedCuda>();

    for (int p = 0; p < data.numPlanes(); ++p)
    {
        const nvcv::ImagePlaneStrided &plane = data.plane(p);

        util::CheckThrow(cudaMemset2D(plane.basePtr, plane.rowStride, 0,
                                      static_cast<size_t>(plane.width) * data.format().planePixelStrideBytes(p),
                                      plane.height));
    }

    return img;
}

std::shared_ptr<Image> Image::WrapExternalBuffer(ExternalBuffer &buffer, nvcv::ImageFormat fmt)
{
    py::object obj = py::cast(buffer.shared_from_this());
    return WrapExternalBufferVector({obj}, fmt);
}

std::vector<std::shared_ptr<Image>> Image::WrapExternalBufferMany(std::vector<std::shared_ptr<ExternalBuffer>> &buffers,
                                                                  nvcv::ImageFormat                             fmt)
{
    // This is the key of an image wrapper.
    // All image wrappers have the same key.
    Image::Key key;

    std::vector<std::shared_ptr<CacheItem>> items = Cache::Instance().fetch(key);

    std::vector<std::shared_ptr<Image>> out;
    out.reserve(buffers.size());

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        std::vector<std::shared_ptr<ExternalBuffer>> spBuffers;
        spBuffers.push_back(buffers[i]);

        if (!spBuffers.back())
            throw std::runtime_error("Input buffer doesn't provide cuda_array_interface or DLPack interfaces");

        std::vector<DLPackTensor> bufinfos;
        bufinfos.emplace_back(spBuffers[0]->dlTensor());
        nvcv::ImageDataStridedCuda imgData = CreateNVCVImageDataCuda(bufinfos, fmt);

        // None found?
        if (items.empty())
        {
            // Need to add wrappers into cache so that they don't get destroyed by
            // the cuda stream when they're last used, and python script isn't
            // holding a reference to them. If we don't do it, things might break.
            std::shared_ptr<Image> img(new Image(std::move(spBuffers), imgData));
            Cache::Instance().add(*img);
            out.push_back(img);
        }
        else
        {
            std::shared_ptr<Image> img = std::static_pointer_cast<Image>(items.back());
            items.pop_back();
            img->setWrapData(std::move(spBuffers), imgData);
            out.push_back(img);
        }
    }

    return out;
}

std::shared_ptr<Image> Image::WrapExternalBufferVector(std::vector<py::object> buffers, nvcv::ImageFormat fmt)
{
    std::vector<std::shared_ptr<ExternalBuffer>> spBuffers;
    for (auto &obj : buffers)
    {
        std::shared_ptr<ExternalBuffer> buffer = cast_py_object_as<ExternalBuffer>(obj);
        if (!buffer)
            throw std::runtime_error("Input buffer doesn't provide cuda_array_interface or DLPack interfaces");
        spBuffers.push_back(std::move(buffer));
    }

    std::vector<DLPackTensor> bufinfos;

    for (size_t i = 0; i < spBuffers.size(); ++i)
    {
        bufinfos.emplace_back(spBuffers[i]->dlTensor());
    }

    nvcv::ImageDataStridedCuda imgData = CreateNVCVImageDataCuda(std::move(bufinfos), fmt);

    // This is the key of an image wrapper.
    // All image wrappers have the same key.
    Image::Key key;

    std::shared_ptr<CacheItem> item = Cache::Instance().fetchOne(key);

    // None found?
    if (!item)
    {
        // Need to add wrappers into cache so that they don't get destroyed by
        // the cuda stream when they're last used, and python script isn't
        // holding a reference to them. If we don't do it, things might break.
        std::shared_ptr<Image> img(new Image(std::move(spBuffers), imgData));
        Cache::Instance().add(*img);
        return img;
    }
    else
    {
        std::shared_ptr<Image> img = std::static_pointer_cast<Image>(item);
        img->setWrapData(std::move(spBuffers), imgData);
        return img;
    }
}

void Image::setWrapData(std::vector<std::shared_ptr<ExternalBuffer>> bufs, const nvcv::ImageDataStridedCuda &imgData)
{
    NVCV_ASSERT(m_wrapData);

    NVCV_ASSERT(bufs.size() >= 1);
    m_wrapData->devType = bufs[0]->dlTensor().device.device_type;

    if (bufs.size() == 1)
    {
        m_wrapData->obj = py::cast(bufs[0]);
    }
    else
    {
        for (size_t i = 1; i < bufs.size(); ++i)
        {
            if (bufs[i]->dlTensor().device.device_type != bufs[0]->dlTensor().device.device_type
                || bufs[i]->dlTensor().device.device_id != bufs[0]->dlTensor().device.device_id)
            {
                throw std::runtime_error("All buffers must belong to the same device, but some don't.");
            }
        }

        m_wrapData->obj = py::cast(std::move(bufs));
    }

    //We recreate the nvcv::Image wrapper (m_impl) because it's cheap.
    //It's not cheap to create nvcvpy::Image as it might have allocated expensive resources (cudaEvent_t in Resource parent).
    m_impl = nvcv::ImageWrapData(imgData);
}

std::shared_ptr<Image> Image::CreateHost(py::buffer buffer, nvcv::ImageFormat fmt, int rowAlign)
{
    return CreateHostVector(std::vector{buffer}, fmt, rowAlign);
}

std::shared_ptr<Image> Image::CreateHostVector(std::vector<py::buffer> buffers, nvcv::ImageFormat fmt, int rowAlign)
{
    std::vector<DLPackTensor> dlTensorList;

    for (size_t i = 0; i < buffers.size(); ++i)
    {
        dlTensorList.emplace_back(buffers[i].request(), DLDevice{kDLCPU, 0});
    }

    nvcv::ImageDataStridedHost imgData = CreateNVCVImageDataHost(std::move(dlTensorList), fmt);

    // We take this opportunity to remove all wrappers from cache.
    // They aren't reusable anyway.
    Image::Key key;
    Cache::Instance().removeAllNotInUseMatching(key);

    std::shared_ptr<Image> img(new Image(std::move(buffers), imgData, rowAlign));
    Cache::Instance().add(*img);
    return img;
}

Size2D Image::size() const
{
    nvcv::Size2D s = m_impl.size();
    return {s.w, s.h};
}

int32_t Image::width() const
{
    return m_impl.size().w;
}

int32_t Image::height() const
{
    return m_impl.size().h;
}

nvcv::ImageFormat Image::format() const
{
    return m_impl.format();
}

std::ostream &operator<<(std::ostream &out, const Image &img)
{
    std::string size_str = std::to_string(img.width()) + 'x' + std::to_string(img.height());

    return out << "<nvcv.Image " << size_str << ' ' << img.format() << '>';
}

namespace {

std::vector<std::pair<py::buffer_info, nvcv::TensorLayout>> ToPyBufferInfo(const nvcv::ImageDataStrided     &imgData,
                                                                           std::optional<nvcv::TensorLayout> userLayout)
{
    if (imgData.numPlanes() < 1)
    {
        return {};
    }

    const nvcv::ImagePlaneStrided &firstPlane = imgData.plane(0);

    std::optional<nvcv::TensorLayoutInfoImage> infoLayout;
    if (userLayout)
    {
        if (auto tmp = nvcv::TensorLayoutInfoImage::Create(*userLayout))
        {
            infoLayout.emplace(std::move(*tmp));
        }
        else
        {
            throw std::runtime_error("Layout can't represent the planar images needed");
        }
    }

    bool singleBuffer = true;

    // Let's check if we can return only one buffer, depending
    // on the planes dimensions, pitch and data type.
    for (int p = 1; p < imgData.numPlanes(); ++p)
    {
        const nvcv::ImagePlaneStrided &plane = imgData.plane(p);

        if (plane.width != firstPlane.width || plane.height != firstPlane.height
            || plane.rowStride != firstPlane.rowStride || imgData.format().planeDataType(0).numChannels() >= 2
            || imgData.format().planeDataType(0) != imgData.format().planeDataType(p))
        {
            singleBuffer = false;
            break;
        }

        // check if using the same plane pitch
        if (p >= 2)
        {
            intptr_t goldPlaneStrided = imgData.plane(1).basePtr - imgData.plane(0).basePtr;
            intptr_t curPlaneStrided  = imgData.plane(p).basePtr - imgData.plane(p - 1).basePtr;
            if (curPlaneStrided != goldPlaneStrided)
            {
                singleBuffer = false;
                break;
            }
        }
    }

    std::vector<std::pair<py::buffer_info, nvcv::TensorLayout>> out;

    // If not using a single buffer, we'll forcibly use one buffer per plane.
    int numBuffers = singleBuffer ? 1 : imgData.numPlanes();

    for (int p = 0; p < numBuffers; ++p)
    {
        int planeWidth       = imgData.plane(p).width;
        int planeHeight      = imgData.plane(p).height;
        int planeNumChannels = imgData.format().planeNumChannels(p);
        // bytes per pixel in the plane
        int planeBPP = imgData.format().planeDataType(p).strideBytes();

        switch (imgData.format().planePacking(p))
        {
        // These (YUYV, UYVY, ...) need some special treatment.
        // Although it's 3 channels in the plane, it's actually
        // two channels per pixel.
        case nvcv::Packing::X8_Y8__X8_Z8:
        case nvcv::Packing::Y8_X8__Z8_X8:
            planeNumChannels = 2;
            break;
        default:
            break;
        }

        // Infer the layout and shape of this buffer
        std::vector<ssize_t> inferredShape;
        std::vector<ssize_t> inferredStrides;
        nvcv::TensorLayout   inferredLayout;

        py::dtype inferredDType;

        if (numBuffers == 1)
        {
            if (imgData.format().numChannels() == 1)
            {
                NVCV_ASSERT(imgData.numPlanes() == 1);
                inferredShape   = {planeHeight, planeWidth};
                inferredStrides = {imgData.plane(p).rowStride, planeBPP};
                inferredLayout  = nvcv::TensorLayout{"HW"};
                inferredDType   = py::cast(imgData.format().planeDataType(p));
            }
            else if (imgData.numPlanes() == 1)
            {
                NVCV_ASSERT(planeNumChannels >= 2);
                inferredShape   = {planeHeight, planeWidth, planeNumChannels};
                inferredStrides = {imgData.plane(p).rowStride, planeBPP, planeBPP / planeNumChannels};
                inferredLayout  = nvcv::TensorLayout{"HWC"};
                inferredDType   = py::cast(imgData.format().planeDataType(p).channelType(0));
            }
            else
            {
                NVCV_ASSERT(planeNumChannels == 1);

                intptr_t planeStride = imgData.plane(1).basePtr - imgData.plane(0).basePtr;
                NVCV_ASSERT(planeStride > 0);

                inferredShape   = {imgData.numPlanes(), planeHeight, planeWidth};
                inferredStrides = {planeStride, imgData.plane(p).rowStride, planeBPP};
                inferredLayout  = nvcv::TensorLayout{"CHW"};
                inferredDType   = py::cast(imgData.format().planeDataType(p));
            }
        }
        else
        {
            NVCV_ASSERT(imgData.numPlanes() >= 2);
            NVCV_ASSERT(imgData.numPlanes() == numBuffers);

            inferredShape = {planeHeight, planeWidth, planeNumChannels};
            inferredStrides
                = {(int64_t)imgData.plane(p).rowStride, (int64_t)planeBPP, (int64_t)planeBPP / planeNumChannels};
            inferredLayout = nvcv::TensorLayout{"HWC"};
            inferredDType  = py::cast(imgData.format().planeDataType(p).channelType(0));
        }

        NVCV_ASSERT((ssize_t)inferredShape.size() == inferredLayout.rank());
        NVCV_ASSERT((ssize_t)inferredStrides.size() == inferredLayout.rank());

        std::vector<ssize_t> shape;
        std::vector<ssize_t> strides;
        nvcv::TensorLayout   layout;

        // Do we have to use the layout user has specified?
        if (userLayout)
        {
            layout = *userLayout;

            // Check if user layout has all required dimensions
            for (int i = 0; i < inferredLayout.rank(); ++i)
            {
                if (inferredShape[i] >= 2 && userLayout->find(inferredLayout[i]) < 0)
                {
                    throw std::runtime_error(util::FormatString("Layout need dimension '%c'", inferredLayout[i]));
                }
            }

            int idxLastInferDim = -1;

            // Fill up the final shape and strides according to the user layout
            for (int i = 0; i < userLayout->rank(); ++i)
            {
                int idxInferDim = inferredLayout.find((*userLayout)[i]);

                if (idxInferDim < 0)
                {
                    shape.push_back(1);
                    // TODO: must do better than this
                    strides.push_back(0);
                }
                else
                {
                    // The order of channels must be the same, despite of
                    // user layout having some other channels in the layout
                    // in between the channels in inferredLayout.
                    if (idxLastInferDim >= idxInferDim)
                    {
                        throw std::runtime_error("Layout not compatible with image to be exported");
                    }
                    idxLastInferDim = idxInferDim;

                    shape.push_back(inferredShape[idxInferDim]);
                    strides.push_back(inferredStrides[idxInferDim]);
                }
            }
        }
        else
        {
            layout  = inferredLayout;
            shape   = inferredShape;
            strides = inferredStrides;
        }

        // There's no direct way to construct a py::buffer_info from data together with a py::dtype.
        // To do that, we first construct a py::array (it accepts py::dtype), and use ".request()"
        // to retrieve the corresponding py::buffer_info.
        // To avoid spurious data copies in py::array ctor, we create this dummy owner.
        py::tuple tmpOwner = py::make_tuple();
        py::array tmp(inferredDType, shape, strides, imgData.plane(p).basePtr, tmpOwner);
        out.emplace_back(tmp.request(), layout);
    }

    return out;
}

std::vector<py::object> ToPython(const nvcv::ImageData &imgData, std::optional<nvcv::TensorLayout> userLayout,
                                 py::object owner)
{
    std::vector<py::object> out;

    auto pitchData = imgData.cast<nvcv::ImageDataStrided>();
    if (!pitchData)
    {
        throw std::runtime_error("Only images with pitch-linear formats can be exported");
    }

    for (const auto &[info, layout] : ToPyBufferInfo(*pitchData, userLayout))
    {
        if (pitchData->cast<nvcv::ImageDataStridedCuda>())
        {
            // TODO: set correct device_type and device_id
            out.emplace_back(ExternalBuffer::Create(
                DLPackTensor{
                    info,
                    {kDLCUDA, 0}
            },
                owner));
        }
        else if (pitchData->cast<nvcv::ImageDataStridedHost>())
        {
            // With no owner, python/pybind11 will make a copy of the data
            out.emplace_back(py::array(info, owner));
        }
        else
        {
            throw std::runtime_error("Buffer type not supported");
        }
    }

    return out;
}

} // namespace

py::object Image::cuda(std::optional<nvcv::TensorLayout> layout) const
{
    // No layout requested and we're wrapping external data?
    if (!layout && m_wrapData)
    {
        if (!IsCudaAccessible(m_wrapData->devType))
        {
            throw std::runtime_error("Image data can't be exported, it's not cuda-accessible");
        }

        // That's what we'll return, as m_impl is wrapping it.
        return m_wrapData->obj;
    }
    else
    {
        auto imgData = m_impl.exportData<nvcv::ImageDataStridedCuda>();
        if (!imgData)
        {
            throw std::runtime_error("Image data can't be exported, it's not cuda-accessible");
        }

        std::vector<py::object> out = ToPython(*imgData, layout, py::cast(*this));

        if (out.size() == 1)
        {
            return std::move(out[0]);
        }
        else
        {
            return py::cast(out);
        }
    }
}

py::object Image::cpu(std::optional<nvcv::TensorLayout> layout) const
{
    auto devStrided = m_impl.exportData<nvcv::ImageDataStridedCuda>();
    if (!devStrided)
    {
        throw std::runtime_error("Only images with pitch-linear formats can be exported to CPU");
    }

    std::vector<std::pair<py::buffer_info, nvcv::TensorLayout>> vDevBufInfo = ToPyBufferInfo(*devStrided, layout);

    std::vector<py::object> out;

    for (const auto &[devBufInfo, bufLayout] : vDevBufInfo)
    {
        std::vector<ssize_t> shape      = devBufInfo.shape;
        std::vector<ssize_t> devStrides = devBufInfo.strides;

        py::array hostData(util::ToDType(devBufInfo), shape);

        py::buffer_info      hostBufInfo = hostData.request();
        std::vector<ssize_t> hostStrides = hostBufInfo.strides;

        auto infoShape
            = nvcv::TensorShapeInfoImagePlanar::Create(nvcv::TensorShape(shape.data(), shape.size(), bufLayout));
        NVCV_ASSERT(infoShape);

        int nplanes = infoShape->numPlanes();
        int ncols   = infoShape->numCols();
        int nrows   = infoShape->numRows();

        ssize_t colStride = devStrides[infoShape->infoLayout().idxWidth()];
        NVCV_ASSERT(colStride == hostStrides[infoShape->infoLayout().idxWidth()]); // both must be packed

        ssize_t hostRowStride, devRowStride;
        if (infoShape->infoLayout().idxHeight() >= 0)
        {
            devRowStride  = devStrides[infoShape->infoLayout().idxHeight()];
            hostRowStride = hostStrides[infoShape->infoLayout().idxHeight()];
        }
        else
        {
            devRowStride  = colStride * ncols;
            hostRowStride = colStride * ncols;
        }

        ssize_t hostPlaneStride = hostRowStride * nrows;
        ssize_t devPlaneStride  = devRowStride * nrows;

        for (int p = 0; p < nplanes; ++p)
        {
            util::CheckThrow(cudaMemcpy2D(reinterpret_cast<std::byte *>(hostBufInfo.ptr) + p * hostPlaneStride,
                                          hostRowStride,
                                          reinterpret_cast<std::byte *>(devBufInfo.ptr) + p * devPlaneStride,
                                          devRowStride, ncols * colStride, nrows, cudaMemcpyDeviceToHost));
        }

        out.push_back(std::move(hostData));
    }

    if (out.size() == 1)
    {
        return std::move(out[0]);
    }
    else
    {
        return py::cast(out);
    }
}

void Image::Export(py::module &m)
{
    using namespace py::literals;

    py::class_<Image, std::shared_ptr<Image>, Container>(m, "Image", "Image")
        .def(py::init(&Image::Create), "size"_a, "format"_a, "rowalign"_a = 0,
             "Constructor that takes a size, format and optional row align of the image")
        .def(py::init(&Image::CreateHost), "buffer"_a, "format"_a = nvcv::FMT_NONE, "rowalign"_a = 0,
             "Constructor that takes a host buffer, format and optional row align")
        .def(py::init(&Image::CreateHostVector), "buffer"_a, "format"_a = nvcv::FMT_NONE, "rowalign"_a = 0,
             "Constructor that takes a host buffer vector, format and optional row align")
        .def_static("zeros", &Image::Zeros, "size"_a, "format"_a, "rowalign"_a = 0,
                    "Create an image filled with zeros with a given size, format and optional row align")
        .def("__repr__", &util::ToString<Image>)
        .def("cuda", &Image::cuda, "layout"_a = std::nullopt, "The image on the CUDA device")
        .def("cpu", &Image::cpu, "layout"_a = std::nullopt, "The image on the CPU")
        .def_property_readonly("size", &Image::size, "Read-only property that returns the size of the image")
        .def_property_readonly("width", &Image::width, "Read-only property that returns the width of the image")
        .def_property_readonly("height", &Image::height, "Read-only property that returns the height of the image")
        .def_property_readonly("format", &Image::format, "Read-only property that returns the format of the image");

    // Make sure buffer lifetime is tied to image's (keep_alive)
    m.def("as_image", &Image::WrapExternalBuffer, "buffer"_a, "format"_a = nvcv::FMT_NONE, py::keep_alive<0, 1>(),
          "Wrap an external buffer as an image and tie the buffer lifetime to the image");
    m.def("as_image", &Image::WrapExternalBufferVector, py::arg_v("buffer", std::vector<py::object>{}),
          "format"_a = nvcv::FMT_NONE, py::keep_alive<0, 1>(),
          "Wrap a vector of external buffers as an image and tie the buffer lifetime to the image");
}

} // namespace nvcvpy::priv
