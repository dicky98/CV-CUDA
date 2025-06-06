# SPDX-FileCopyrightText: Copyright (c) 2022-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import math
import os
import threading
import torch
import numpy as np
import numbers
import nvcv
import copy
import colorsys
from typing_extensions import Callable, Concatenate, ParamSpec

P = ParamSpec("P")

IMG_FORMAT_TO_TYPE = {
    nvcv.Format.U8: nvcv.Type.U8,
    nvcv.Format.U16: nvcv.Type.U16,
    nvcv.Format.U32: nvcv.Type.U32,
    nvcv.Format.S8: nvcv.Type.S8,
    nvcv.Format.S16: nvcv.Type.S16,
    nvcv.Format.S32: nvcv.Type.S32,
    nvcv.Format.F32: nvcv.Type.F32,
    nvcv.Format.F64: nvcv.Type.F64,
}

IMG_FORMAT_TO_NUMPY_DTYPE = {
    nvcv.Format.HSV8: np.uint8,
    nvcv.Format.BGRA8: np.uint8,
    nvcv.Format.RGBA8: np.uint8,
    nvcv.Format.BGR8: np.uint8,
    nvcv.Format.RGB8: np.uint8,
    nvcv.Format.RGBf32: np.float32,
    nvcv.Format.RGBAf32: np.float32,
    nvcv.Format.F32: np.float32,
    nvcv.Format.F64: np.float64,
    nvcv.Format.U8: np.uint8,
    nvcv.Format.S8: np.int8,
    nvcv.Format.Y8: np.uint8,
    nvcv.Format.Y8_ER: np.uint8,
    nvcv.Format.U16: np.uint16,
    nvcv.Format.S16: np.int16,
    nvcv.Format.U32: np.uint32,
    nvcv.Format.S32: np.int32,
}


def dist_odd(x):
    """Add one to x if even to make it odd

    Args:
        x (number): Value to turn odd if even

    Returns:
        number: Odd input value
    """
    return x if x % 2 == 1 else x + 1


def generate_data(shape, dtype, max_random=None, rng=None):
    """Generate data as numpy array

    Args:
        shape (tuple or list): Data shape
        dtype (numpy dtype): Data type (e.g. np.uint8)
        max_random (number or tuple or list): Maximum random value
        rng (numpy random Generator): To fill data with random values

    Returns:
        numpy.array: The generated data
    """
    if rng is None:
        data = np.zeros(shape, dtype=dtype)
    else:
        if max_random is not None and type(max_random) in {tuple, list}:
            assert len(max_random) == shape[-1]
        if issubclass(dtype, numbers.Integral):
            if max_random is None:
                max_random = [np.iinfo(dtype).max for _ in range(shape[-1])]
            data = rng.integers(max_random, size=shape, dtype=dtype)
        elif issubclass(dtype, numbers.Real):
            if max_random is None:
                max_random = [1.0 for _ in range(shape[-1])]
            data = rng.random(size=shape, dtype=dtype) * np.array(max_random)
            data = data.astype(dtype)
    return data


class CudaBuffer:
    __cuda_array_interface__ = None
    obj = None


def to_torch_dtype(data_type):
    """Convert a data type into one supported by torch

    Args:
        data_type (numpy dtype): Original data type

    Returns:
        dtype: A data type supported by torch
    """
    if data_type == np.uint16:
        return np.dtype(np.int16)
    elif data_type == np.uint32:
        return np.dtype(np.int32)
    elif data_type == np.uint64:
        return np.dtype(np.int64)
    else:
        return data_type


def to_cpu_numpy_buffer(cuda_buffer):
    """Convert a CUDA buffer to host (CPU) data

    Args:
        cuda_buffer: CUDA buffer with __cuda_array_interface__

    Returns:
        numpy array: The CUDA buffer copied to the CPU
    """
    torch_dtype = copy.copy(cuda_buffer.dtype)
    torch_dtype = to_torch_dtype(torch_dtype)

    buf = CudaBuffer
    buf.obj = cuda_buffer
    buf.__cuda_array_interface__ = cuda_buffer.__cuda_array_interface__
    buf.__cuda_array_interface__["typestr"] = torch_dtype.str

    return torch.as_tensor(buf).cpu().numpy()


def to_cuda_buffer(host_data):
    """Convert host data to a CUDA buffer

    Args:
        host_data (numpy array): Host data

    Returns:
        CudaBuffer: The converted CUDA buffer
    """
    orig_dtype = copy.copy(host_data.dtype)

    host_data.dtype = to_torch_dtype(host_data.dtype)

    dev = torch.as_tensor(host_data, device="cuda").cuda()
    host_data.dtype = orig_dtype  # restore it

    # The cuda buffer only needs the cuda array interface.
    # We can then set its dtype to whatever we want.
    buf = CudaBuffer()
    buf.__cuda_array_interface__ = dev.__cuda_array_interface__
    buf.__cuda_array_interface__["typestr"] = orig_dtype.str
    buf.obj = dev  # make sure it holds a reference to the torch buffer

    return buf


def to_nvcv_tensor(data, layout):
    """Convert a tensor in host or CUDA data with layout to nvcv.Tensor

    Args:
        data (numpy array or CUDA array): Tensor in host data
        layout (string): Tensor layout (e.g. NC, HWC, NHWC)

    Returns:
        nvcv.Tensor: The converted tensor
    """
    cuda_data = data
    if "__cuda_array_interface__" not in dir(cuda_data):
        cuda_data = to_cuda_buffer(data)
    shape = cuda_data.__cuda_array_interface__["shape"]
    if layout is not None:
        if len(shape) < len(layout):
            shape = (1,) * (len(layout) - len(shape)) + shape
        elif len(shape) > len(layout):
            raise ValueError("Layout smaller than shape of tensor data")
    cuda_data.__cuda_array_interface__["shape"] = shape
    return nvcv.as_tensor(cuda_data, layout=layout)


def create_tensor(shape, dtype, layout, max_random=None, rng=None, transform_dist=None):
    """Create a tensor

    Args:
        shape (tuple or list): Tensor shape
        dtype (numpy dtype): Tensor data type (e.g. np.uint8)
        layout (string): Tensor layout (e.g. NC, HWC, NHWC)
        max_random (number or tuple or list): Maximum random value
        rng (numpy random Generator): To fill tensor with random values
        transform_dist (function): To transform random values (e.g. dist_odd)

    Returns:
        nvcv.Tensor: The created tensor
    """
    h_data = generate_data(shape, dtype, max_random, rng)
    if transform_dist is not None:
        vec_transform_dist = np.vectorize(transform_dist)
        h_data = vec_transform_dist(h_data)
        h_data = h_data.astype(dtype)
    return to_nvcv_tensor(h_data, layout)


def to_nvcv_image(host_data):
    """Convert an image in host data to nvcv.Image

    Args:
        host_data (numpy array): Tensor in host data

    Returns:
        nvcv.Image: The converted image
    """
    return nvcv.as_image(to_cuda_buffer(host_data))


def create_image(size, img_format, max_random=None, rng=None):
    """Create an image

    Args:
        size (tuple or list): Image size (width, height)
        img_format (nvcv.Format): Image format
        max_random (number or tuple or list): Maximum random value
        rng (numpy random Generator): To image with random values

    Returns:
        nvcv.Image: The created image
    """
    shape = (size[1], size[0], img_format.channels)
    dtype = IMG_FORMAT_TO_NUMPY_DTYPE[img_format]
    h_data = generate_data(shape, dtype, max_random, rng)
    return to_nvcv_image(h_data)


def create_image_pattern(
    size, img_format=nvcv.Format.RGB8, bands=10, saturation=0.96, value=0.82
):
    """Create an image with circles-band pattern

    Args:
        size (tuple or list): Image size (width, height)
        img_format (nvcv.Format): Image format, must be RGB(A)(8, f32), BGR(A)8 or HSV8
        bands (number): Number of circle bands in the image pattern
        saturation (number): Saturation of each HSV circle color, hue decreases along image
        value (number): Value of each HSV circle color, hue decreases along image

    Returns:
        np.array: The created image
    """
    shape = (size[1], size[0], img_format.channels)
    dtype = IMG_FORMAT_TO_NUMPY_DTYPE[img_format]
    ci, cj = shape[0] - 1, shape[1] - 1
    max_r = max(ci, cj) * math.sqrt(2)
    image = np.zeros(shape, dtype=dtype)
    if max_r == 0:
        raise ValueError("Invalid image size")
    if img_format in {nvcv.Format.RGBA8, nvcv.Format.RGB8, nvcv.Format.RGBf32}:
        format = "rgb"
    elif img_format in {nvcv.Format.BGRA8, nvcv.Format.BGR8}:
        format = "bgr"
    elif img_format in {nvcv.Format.HSV8}:
        format = "hsv"
    else:
        raise ValueError("Invalid image format")
    for i in range(shape[0]):
        for j in range(shape[1]):
            r = math.sqrt((i - ci) ** 2 + (j - cj) ** 2)
            norm_r = round((r / max_r) * bands) / bands
            hsv = np.array([norm_r, saturation, value])
            if format == "hsv":
                pixel = hsv * 255
            else:
                rgb = np.array(colorsys.hsv_to_rgb(hsv[0], hsv[1], hsv[2]))
                if format == "rgb":
                    pixel = rgb * 255
                elif format == "bgr":
                    pixel = rgb[::-1] * 255
            if img_format.channels == 4:
                pixel = np.concatenate((pixel, np.array([255])))
            image[i, j, :] = pixel.astype(dtype)
    return image


def create_image_batch(
    num_images, img_format, size=(0, 0), max_size=(128, 128), max_random=None, rng=None
):
    """Create an image batch

    Args:
        num_images (number): Number of images in the batch
        img_format (nvcv.ImageFormat): Image format of each image
        size (tuple or list): Image size (width, height) use (0, 0) for random sizes
        max_size (tuple or list): Use random image size from 1 to max_size
        max_random (number or tuple or list): Maximum random value inside each image
        rng (numpy random Generator): To fill each image with random values

    Returns:
        nvcv.ImageBatchVarShape: The created image batch
    """
    if size[0] == 0 or size[1] == 0:
        assert rng is not None
    image_batch = nvcv.ImageBatchVarShape(num_images)
    for i in range(num_images):
        w = (
            rng.integers(1, max_size[0] + 1)
            if rng is not None and size[0] == 0
            else size[0]
        )
        h = (
            rng.integers(1, max_size[1] + 1)
            if rng is not None and size[1] == 0
            else size[1]
        )
        image_batch.pushback(create_image((w, h), img_format, max_random, rng))
    return image_batch


def clone_image_batch(input_image_batch, img_format=None):
    """Clone an image batch

    Args:
        input_image_batch (nvcv.ImageBatchVarShape): Image batch to be cloned
        img_format (nvcv.ImageFormat): Image format of the output

    Returns:
        nvcv.ImageBatchVarShape: The cloned image batch var shape
    """
    output_image_batch = nvcv.ImageBatchVarShape(input_image_batch.capacity)
    for input_image in input_image_batch:
        image = nvcv.Image(
            input_image.size, input_image.format if img_format is None else img_format
        )
        output_image_batch.pushback(image)
    return output_image_batch


def run_parallel(
    target: Callable[Concatenate[int, P], None],
    *args: P.args,
    **kwargs: P.kwargs,
):
    """Run a callable in multiple threads and forward any exception to the main thread.

    Args:
        target (Callable): Callable to be run in multiple threads. The first argument is the thread index
        args: Positional arguments fowarded to the callable
        kwargs: Keyword arguments forwarded to the callable
    """

    def wrapper(thread_no: int):
        nonlocal exception

        barrier.wait()

        try:
            target(thread_no, *args, **kwargs)
        except Exception as exc:
            exception = exc

    nb_threads = len(os.sched_getaffinity(0))
    threads = [
        threading.Thread(target=wrapper, args=(idx,)) for idx in range(nb_threads)
    ]
    barrier = threading.Barrier(nb_threads)
    exception: Exception | None = None

    for thread in threads:
        thread.start()

    for thread in threads:
        thread.join()

    if exception is not None:
        raise exception
