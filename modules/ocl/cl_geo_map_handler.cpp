/*
 * cl_geo_map_handler.cpp - CL geometry map handler
 *
 *  Copyright (c) 2016 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Wind Yuan <feng.yuan@intel.com>
 */

#include "xcam_utils.h"
#include "cl_geo_map_handler.h"
#include "cl_device.h"

namespace XCam {

static const XCamKernelInfo kernel_geo_map_info = {
    "kernel_geo_map",
#include "kernel_geo_map.clx"
    , 0,
};

// GEO_MAP_CHANNEL for CL_RGBA channel
#define GEO_MAP_CHANNEL 4  /* only use channel_0, channel_1 */

CLGeoMapKernel::CLGeoMapKernel (
    SmartPtr<CLContext> &context, const SmartPtr<GeoKernelParamCallback> handler)
    : CLImageKernel (context)
    , _handler (handler)
{
    XCAM_ASSERT (handler.ptr ());
    xcam_mem_clear (_geo_scale_size);
    xcam_mem_clear (_out_size);
}

XCamReturn
CLGeoMapKernel::prepare_arguments (
    SmartPtr<DrmBoBuffer> &input, SmartPtr<DrmBoBuffer> &output,
    CLArgument args[], uint32_t &arg_count,
    CLWorkSize &work_size)
{
    XCAM_UNUSED (input);
    XCAM_UNUSED (output);

    SmartPtr<CLImage> input_y = _handler->get_geo_input_image (CLNV12PlaneY);
    SmartPtr<CLImage> input_uv = _handler->get_geo_input_image (CLNV12PlaneUV);
    SmartPtr<CLImage> output_y = _handler->get_geo_output_image (CLNV12PlaneY);
    SmartPtr<CLImage> output_uv = _handler->get_geo_output_image (CLNV12PlaneUV);
    const CLImageDesc &outuv_desc = output_uv->get_image_desc ();
    SmartPtr<CLImage> geo_image = _handler->get_geo_map_table ();
    _handler->get_geo_equivalent_out_size (_geo_scale_size[0], _geo_scale_size[1]);
    _handler->get_geo_pixel_out_size (_out_size[0], _out_size[1]);

    arg_count = 0;
    args[arg_count].arg_adress = &input_y->get_mem_id ();
    args[arg_count].arg_size = sizeof (cl_mem);
    ++arg_count;

    args[arg_count].arg_adress = &input_uv->get_mem_id ();
    args[arg_count].arg_size = sizeof (cl_mem);
    ++arg_count;

    args[arg_count].arg_adress = &geo_image->get_mem_id ();
    args[arg_count].arg_size = sizeof (cl_mem);
    ++arg_count;

    args[arg_count].arg_adress = &_geo_scale_size;
    args[arg_count].arg_size = sizeof (_geo_scale_size);
    ++arg_count;

    args[arg_count].arg_adress = &output_y->get_mem_id ();
    args[arg_count].arg_size = sizeof (cl_mem);
    ++arg_count;

    args[arg_count].arg_adress = &output_uv->get_mem_id ();
    args[arg_count].arg_size = sizeof (cl_mem);
    ++arg_count;

    args[arg_count].arg_adress = &_out_size;
    args[arg_count].arg_size = sizeof (_out_size);
    ++arg_count;

    work_size.dim = XCAM_DEFAULT_IMAGE_DIM;
    work_size.local[0] = 16;
    work_size.local[1] = 4;
    work_size.global[0] = XCAM_ALIGN_UP (outuv_desc.width, work_size.local[0]);
    work_size.global[1] = XCAM_ALIGN_UP (outuv_desc.height, work_size.local[1]);

    return XCAM_RETURN_NO_ERROR;
}

CLGeoMapHandler::CLGeoMapHandler ()
    : CLImageHandler ("CLGeoMapHandler")
    , _output_width (0)
    , _output_height (0)
    , _map_width (0)
    , _map_height (0)
    , _uint_x (0.0f)
    , _uint_y (0.0f)
    , _geo_map_normalized (false)
{
}

void
CLGeoMapHandler::get_geo_equivalent_out_size (float &width, float &height)
{
    width = _map_width * _uint_x;
    height = _map_height * _uint_y;
}

void
CLGeoMapHandler::get_geo_pixel_out_size (float &width, float &height)
{
    width = _output_width;
    height = _output_height;
}

bool
CLGeoMapHandler::set_map_uint (float uint_x, float uint_y)
{
    _uint_x = uint_x;
    _uint_y = uint_y;
    return true;
}

bool
CLGeoMapHandler::set_map_data (GeoPos *data, uint32_t width, uint32_t height)
{
    uint32_t size = width * height * GEO_MAP_CHANNEL * sizeof (float); // 4 for CL_RGBA,
    float *map_ptr = NULL;

    XCAM_FAIL_RETURN (
        ERROR, check_geo_map_buf (width, height), false,
        "CLGeoMapKernel check geo map buffer failed");

    XCamReturn ret = _geo_map->enqueue_map ((void *&)map_ptr, 0, size);
    XCAM_FAIL_RETURN (
        WARNING, ret == XCAM_RETURN_NO_ERROR, false,
        "CLGeoMapKernel map buffer failed");

    for (uint32_t i = 0; i < width * height; ++i) {
        map_ptr [i * GEO_MAP_CHANNEL] = data [i].x;
        map_ptr [i * GEO_MAP_CHANNEL + 1] = data [i].y;
    }
    _geo_map->enqueue_unmap ((void *&)map_ptr);
    _geo_map_normalized = false;
    return true;
}

bool
CLGeoMapHandler::check_geo_map_buf (uint32_t width, uint32_t height)
{
    XCAM_ASSERT (width && height);
    if (width == _map_width && height == _map_height && _geo_map.ptr ()) {
        return true; // geo memory already created
    }

    uint32_t pitch = width * GEO_MAP_CHANNEL * sizeof (float); // 4 channel for CL_RGBA, but only use RG
    uint32_t size = pitch * height;
    SmartPtr<CLContext> context = CLDevice::instance ()->get_context ();
    XCAM_ASSERT (context.ptr ());
    _geo_map = new CLBuffer (context, size);

    if (!_geo_map.ptr () || !_geo_map->is_valid ()) {
        XCAM_LOG_ERROR ("CLGeoMapKernel create geo map buffer failed.");
        _geo_map.release ();
        return false;
    }

    CLImageDesc cl_geo_desc;
    cl_geo_desc.format.image_channel_data_type = CL_FLOAT;
    cl_geo_desc.format.image_channel_order = CL_RGBA; // CL_FLOAT need co-work with CL_RGBA
    cl_geo_desc.width = width;
    cl_geo_desc.height = height;
    cl_geo_desc.row_pitch = pitch;
    _geo_image = new CLImage2D (context, cl_geo_desc, 0, _geo_map);
    if (!_geo_image.ptr () || !_geo_image->is_valid ()) {
        XCAM_LOG_ERROR ("CLGeoMapKernel convert geo map buffer to image2d failed.");
        _geo_map.release ();
        _geo_image.release ();
        return false;
    }

    _map_width = width;
    _map_height = height;
    return true;
}


bool
CLGeoMapHandler::normalize_geo_map (uint32_t image_w, uint32_t image_h)
{
    XCamReturn ret = XCAM_RETURN_NO_ERROR;
    uint32_t size = _map_width * _map_height * GEO_MAP_CHANNEL * sizeof (float);
    float *map_ptr = NULL;

    XCAM_ASSERT (image_w && image_h);
    XCAM_FAIL_RETURN (
        ERROR, _geo_map.ptr () && _geo_map->is_valid (),
        false, "CLGeoMapKernel geo_map was not initialized");

    ret = _geo_map->enqueue_map ((void *&)map_ptr, 0, size);
    XCAM_FAIL_RETURN (WARNING, ret == XCAM_RETURN_NO_ERROR, false, "CLGeoMapKernel map buffer failed");
    for (uint32_t i = 0; i < _map_width * _map_height; ++i) {
        map_ptr [i * GEO_MAP_CHANNEL] /= image_w;      // x
        map_ptr [i * GEO_MAP_CHANNEL + 1] /= image_h;  //y
    }
    _geo_map->enqueue_unmap ((void *&)map_ptr);

    return true;
}



XCamReturn
CLGeoMapHandler::prepare_buffer_pool_video_info (
    const VideoBufferInfo &input, VideoBufferInfo &output)
{
    XCAM_FAIL_RETURN (
        WARNING, input.format == V4L2_PIX_FMT_NV12, XCAM_RETURN_ERROR_PARAM,
        "CLGeoMapHandler(%s) input buffer format(%s) not NV12", get_name (), xcam_fourcc_to_string (input.format));

    if (!_output_width || !_output_height) {
        _output_width = input.width;
        _output_height = input.height;
    }
    output.init (
        input.format, _output_width, _output_height,
        XCAM_ALIGN_UP (_output_width, 16), XCAM_ALIGN_UP (_output_height, 16));
    return XCAM_RETURN_NO_ERROR;
}

XCamReturn
CLGeoMapHandler::prepare_parameters (SmartPtr<DrmBoBuffer> &input, SmartPtr<DrmBoBuffer> &output)
{
    const VideoBufferInfo &in_info = input->get_video_info ();
    const VideoBufferInfo &out_info = output->get_video_info ();
    SmartPtr<CLContext> context = CLDevice::instance ()->get_context ();
    uint32_t input_image_w = XCAM_ALIGN_DOWN (in_info.width, 2);
    uint32_t input_image_h = XCAM_ALIGN_DOWN (in_info.height, 2);

    CLImageDesc cl_desc;
    cl_desc.format.image_channel_data_type = CL_UNORM_INT8;
    cl_desc.format.image_channel_order = CL_R;
    cl_desc.width = input_image_w;
    cl_desc.height = input_image_h;
    cl_desc.row_pitch = in_info.strides[CLNV12PlaneY];
    _input[CLNV12PlaneY] = new CLVaImage (context, input, cl_desc, in_info.offsets[CLNV12PlaneY]);

    cl_desc.format.image_channel_data_type = CL_UNORM_INT8;
    cl_desc.format.image_channel_order = CL_RG;
    cl_desc.width = input_image_w / 2;
    cl_desc.height = input_image_h / 2;
    cl_desc.row_pitch = in_info.strides[CLNV12PlaneUV];
    _input[CLNV12PlaneUV] = new CLVaImage (context, input, cl_desc, in_info.offsets[CLNV12PlaneUV]);

    cl_desc.format.image_channel_data_type = CL_UNSIGNED_INT16;
    cl_desc.format.image_channel_order = CL_RGBA;
    cl_desc.width = XCAM_ALIGN_DOWN (out_info.width, 4) / 8; //CL_RGBA * CL_UNSIGNED_INT16 = 8
    cl_desc.height = XCAM_ALIGN_DOWN (out_info.height, 2);
    cl_desc.row_pitch = out_info.strides[CLNV12PlaneY];
    _output[CLNV12PlaneY] = new CLVaImage (context, output, cl_desc, out_info.offsets[CLNV12PlaneY]);
    cl_desc.height /= 2;
    cl_desc.row_pitch = out_info.strides[CLNV12PlaneUV];
    _output[CLNV12PlaneUV] = new CLVaImage (context, output, cl_desc, out_info.offsets[CLNV12PlaneUV]);

    XCAM_ASSERT (
        _input[CLNV12PlaneY].ptr () && _input[CLNV12PlaneY]->is_valid () &&
        _input[CLNV12PlaneUV].ptr () && _input[CLNV12PlaneUV]->is_valid () &&
        _output[CLNV12PlaneY].ptr () && _output[CLNV12PlaneY]->is_valid () &&
        _output[CLNV12PlaneUV].ptr () && _output[CLNV12PlaneUV]->is_valid ());

    XCAM_FAIL_RETURN (
        ERROR, _geo_map.ptr () && _geo_map->is_valid (),
        XCAM_RETURN_ERROR_PARAM, "CLGeoMapHandler map data was not set");

    //calculate kernel map unit_x, unit_y.
    float uint_x, uint_y;
    get_map_uint (uint_x, uint_y);
    if (uint_x < 1.0f && uint_y < 1.0f) {
        uint_x = out_info.width / (float)_map_width;
        uint_y = out_info.height / (float)_map_height;
        set_map_uint (uint_x, uint_y);
    }

    if (!_geo_map_normalized) {
        XCAM_FAIL_RETURN (
            ERROR, normalize_geo_map (input_image_w, input_image_h),
            XCAM_RETURN_ERROR_PARAM, "CLGeoMapHandler normalized geo map failed");
        _geo_map_normalized = true;
    }

    return CLImageHandler::prepare_parameters (input, output);
}

XCamReturn
CLGeoMapHandler::execute_done (SmartPtr<DrmBoBuffer> &output)
{
    XCAM_UNUSED (output);

    for (int i = 0; i < CLNV12PlaneMax; ++i) {
        _input[i].release ();
        _output[i].release ();
    }

    return XCAM_RETURN_NO_ERROR;
}

SmartPtr<CLImageKernel>
create_geo_map_kernel (SmartPtr<CLContext> &context, SmartPtr<GeoKernelParamCallback> param_cb)
{
    SmartPtr<CLImageKernel> kernel;
    kernel = new CLGeoMapKernel (context, param_cb);
    XCAM_ASSERT (kernel.ptr ());
    XCAM_FAIL_RETURN (
        ERROR, kernel->build_kernel (kernel_geo_map_info, NULL) == XCAM_RETURN_NO_ERROR,
        NULL, "build geo map kernel failed");

    return kernel;
}

SmartPtr<CLImageHandler>
create_geo_map_handler (SmartPtr<CLContext> &context)
{
    SmartPtr<CLGeoMapHandler> handler;
    SmartPtr<CLImageKernel> kernel;

    handler = new CLGeoMapHandler ();
    XCAM_ASSERT (handler.ptr ());

    kernel = create_geo_map_kernel (context, handler);
    XCAM_FAIL_RETURN (
        ERROR, kernel.ptr (), NULL, "CLMapHandler build geo map kernel failed");
    handler->add_kernel (kernel);

    return handler;
}

}
