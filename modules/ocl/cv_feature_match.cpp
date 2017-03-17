/*
 * cv_feature_match.cpp - optical flow feature match
 *
 *  Copyright (c) 2016-2017 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: Wind Yuan <feng.yuan@intel.com>
 * Author: Yinhang Liu <yinhangx.liu@intel.com>
 */

#include "cv_feature_match.h"

using namespace std;
using namespace cv;
using namespace XCam;

#define XCAM_OF_DEBUG 0
#define XCAM_OF_DRAW_SCALE 2

static const int sitch_min_width = 56;

static const int min_corners = 8;
static const float offset_factor = 0.8f;

static const int delta_count = 4;  // cur_count - last_count
static const float delta_mean_offset = 1.0f; //0.1f;  // cur_mean_offset - last_mean_offset
static const float delta_offset = 12.0f;  // cur_mean_offset - last_offset

void
init_opencv_ocl (SmartPtr<CLContext> context)
{
    static bool is_ocl_inited = false;

    if (!is_ocl_inited) {
        cl_platform_id platform_id = CLDevice::instance()->get_platform_id ();
        char *platform_name = CLDevice::instance()->get_platform_name ();
        cl_device_id device_id = CLDevice::instance()->get_device_id ();
        cl_context context_id = context->get_context_id ();
        ocl::attachContext (platform_name, platform_id, context_id, device_id);

        is_ocl_inited = true;
    }
}

bool
convert_to_mat (SmartPtr<CLContext> context, SmartPtr<DrmBoBuffer> buffer, Mat &image)
{
    SmartPtr<CLBuffer> cl_buffer = new CLVaBuffer (context, buffer);
    VideoBufferInfo info = buffer->get_video_info ();
    cl_mem cl_mem_id = cl_buffer->get_mem_id ();

    UMat umat;
    ocl::convertFromBuffer (cl_mem_id, info.strides[0], info.height * 3 / 2, info.width, CV_8U, umat);
    if (umat.empty ()) {
        XCAM_LOG_ERROR ("convert bo buffer to UMat failed");
        return false;
    }

    Mat mat;
    umat.copyTo (mat);
    if (mat.empty ()) {
        XCAM_LOG_ERROR ("copy UMat to Mat failed");
        return false;
    }

    cvtColor (mat, image, COLOR_YUV2BGR_NV12);
    return true;
}

static void
add_detected_data (Mat image, Ptr<Feature2D> detector, vector<Point2f> &corners)
{
    vector<KeyPoint> keypoints;
    detector->detect (image, keypoints);
    corners.reserve (corners.size () + keypoints.size ());
    for (size_t i = 0; i < keypoints.size (); ++i) {
        KeyPoint &kp = keypoints[i];
        corners.push_back (kp.pt);
    }
}

static void
get_valid_offsets (
    Mat out_image, Size img0_size,
    vector<Point2f> corner0, vector<Point2f> corner1,
    vector<uchar> status, vector<float> error,
    vector<float> &offsets, float &sum, int &count)
{
    count = 0;
    sum = 0.0f;
    for (uint32_t i = 0; i < status.size (); ++i) {
#if XCAM_OF_DEBUG
        Point start = Point(corner0[i]) * XCAM_OF_DRAW_SCALE;
        circle (out_image, start, 4, Scalar(255, 0, 0), XCAM_OF_DRAW_SCALE);
#endif
        if (!status[i] || error[i] > 16)
            continue;
        if (fabs(corner0[i].y - corner1[i].y) >= 4)
            continue;

        float offset = corner1[i].x - corner0[i].x;

        sum += offset;
        ++count;
        offsets.push_back (offset);

#if XCAM_OF_DEBUG
        Point end = (Point(corner1[i]) + Point (img0_size.width, 0)) * XCAM_OF_DRAW_SCALE;
        line (out_image, start, end, Scalar(0, 0, 255), XCAM_OF_DRAW_SCALE);
#else
        XCAM_UNUSED (out_image);
        XCAM_UNUSED (img0_size);
#endif
    }
}

static bool
get_mean_offset (vector<float> offsets, float sum, int &count, float &mean_offset)
{
    if (count < min_corners)
        return false;

    mean_offset = sum / count;

    XCAM_LOG_INFO (
        "X-axis mean offset:%.2f, pre_mean_offset:%.2f (%d times, count:%d)",
        mean_offset, 0.0f, 0, count);

    bool ret = true;
    float delta = 20.0f;//mean_offset;
    float pre_mean_offset = mean_offset;
    for (int try_times = 1; try_times < 4; ++try_times) {
        int recur_count = 0;
        sum = 0.0f;
        for (size_t i = 0; i < offsets.size (); ++i) {
            if (fabs (offsets[i] - mean_offset) >= 4.0f)
                continue;
            sum += offsets[i];
            ++recur_count;
        }

        if (recur_count < min_corners) {
            ret = false;
            break;
        }

        mean_offset = sum / recur_count;
        XCAM_LOG_INFO (
            "X-axis mean offset:%.2f, pre_mean_offset:%.2f (%d times, count:%d)",
            mean_offset, pre_mean_offset, try_times, recur_count);
        if (fabs (mean_offset - pre_mean_offset) > fabs (delta) * 1.2f) {
            ret = false;
            break;
        }

        delta = mean_offset - pre_mean_offset;
        pre_mean_offset = mean_offset;
        count = recur_count;
    }

    return ret;
}

static Mat
calc_match_optical_flow (
    Mat image0, Mat image1,
    vector<Point2f> corner0, vector<Point2f> corner1,
    vector<uchar> &status, vector<float> &error,
    int &last_count, float &last_mean_offset, float &out_x_offset)
{
    Mat out_image;
    Size img0_size = image0.size ();
    Size img1_size = image1.size ();
    XCAM_ASSERT (img0_size.height == img1_size.height);
    Size size (img0_size.width + img1_size.width, img0_size.height);

    out_image.create (size, image0.type ());
    image0.copyTo (out_image (Rect(0, 0, img0_size.width, img0_size.height)));
    image1.copyTo (out_image (Rect(img0_size.width, 0, img1_size.width, img1_size.height)));

#if XCAM_OF_DEBUG
    Size scale_size = size * XCAM_OF_DRAW_SCALE;
    resize (out_image, out_image, scale_size, 0, 0);
#endif

    vector<float> offsets;
    float offset_sum = 0.0f;
    int count = 0;
    float mean_offset = 0.0f;
    offsets.reserve (corner0.size ());
    get_valid_offsets (out_image, img0_size, corner0, corner1, status, error,
                       offsets, offset_sum, count);

    bool ret = get_mean_offset (offsets, offset_sum, count, mean_offset);
    if (ret) {
        if (fabs (mean_offset - last_mean_offset) < delta_mean_offset ||
                fabs (mean_offset - out_x_offset) < delta_offset) {
            out_x_offset = out_x_offset * offset_factor + mean_offset * (1.0f - offset_factor);
        }
    } else
        out_x_offset = 0.0f;

    last_count = count;
    last_mean_offset = mean_offset;

    return out_image;
}

static void
adjust_stitch_area (int dst_width, Rect &stitch0, Rect &stitch1)
{
    int final_overlap_width = stitch1.x + stitch1.width + (dst_width - (stitch0.x + stitch0.width));
    final_overlap_width = XCAM_ALIGN_AROUND (final_overlap_width, 8);
    XCAM_ASSERT (final_overlap_width >= sitch_min_width);
    int center = final_overlap_width / 2;
    XCAM_ASSERT (center > sitch_min_width / 2);

    stitch1.x = XCAM_ALIGN_AROUND (center - sitch_min_width / 2, 8);
    stitch1.width = sitch_min_width;
    stitch0.x = dst_width - final_overlap_width + stitch1.x;
    stitch0.width = sitch_min_width;
}

void
optical_flow_feature_match (
    SmartPtr<CLContext> context, int dst_width,
    SmartPtr<DrmBoBuffer> buf0, SmartPtr<DrmBoBuffer> buf1,
    Rect &image0_crop_left, Rect &image0_crop_right,
    Rect &image1_crop_left, Rect &image1_crop_right,
    const char *input_name, int frame_num)
{
    Mat image0, image1;
    Mat image0_left, image0_right, image1_left, image1_right;
    Mat image0_left_rgb, image0_right_rgb, image1_left_rgb, image1_right_rgb;
    vector<Point2f> corner0_left, corner0_right, corner1_left, corner1_right;
    Mat out_image0, out_image1;
    static float x_offset0 = 0.0f, x_offset1 = 0.0f;
    static int valid_count0 = 0, valid_count1 = 0;
    static float mean_offset0 = 0.0f, mean_offset1 = 0.0f;

    if (!convert_to_mat (context, buf0, image0) || !convert_to_mat (context, buf1, image1))
        return;

    image0_left_rgb = image0 (image0_crop_left);
    cvtColor (image0_left_rgb, image0_left, COLOR_BGR2GRAY);
    image0_right_rgb = image0 (image0_crop_right);
    cvtColor (image0_right_rgb, image0_right, COLOR_BGR2GRAY);

    image1_left_rgb = image1 (image1_crop_left);
    cvtColor (image1_left_rgb, image1_left, COLOR_BGR2GRAY);
    image1_right_rgb = image1 (image1_crop_right);
    cvtColor (image1_right_rgb, image1_right, COLOR_BGR2GRAY);

    Ptr<Feature2D> gft_detector, orb_detector;
    gft_detector = GFTTDetector::create (300, 0.01, 5, 5, false);
    orb_detector = ORB::create (200, 1.5, 2, 9);

    add_detected_data (image0_left, gft_detector, corner0_left);
    add_detected_data (image0_left, orb_detector, corner0_left);
    add_detected_data (image0_right, gft_detector, corner0_right);
    add_detected_data (image0_right, orb_detector, corner0_right);

    vector<float> err0, err1;
    vector<uchar> status0, status1;
    calcOpticalFlowPyrLK (
        image0_left, image1_right, corner0_left, corner1_right,
        status0, err0, Size(5, 5), 3,
        TermCriteria (TermCriteria::COUNT + TermCriteria::EPS, 10, 0.01));
    calcOpticalFlowPyrLK (
        image0_right, image1_left, corner0_right, corner1_left,
        status1, err1, Size(5, 5), 3,
        TermCriteria (TermCriteria::COUNT + TermCriteria::EPS, 10, 0.01));

    Rect tmp_stitch0 = image1_crop_right;
    Rect tmp_stitch1 = image0_crop_left;
    out_image0 = calc_match_optical_flow (image0_left_rgb, image1_right_rgb, corner0_left, corner1_right,
                                          status0, err0, valid_count0, mean_offset0, x_offset0);
    image1_crop_right.x += x_offset0;
    adjust_stitch_area (dst_width, image1_crop_right, image0_crop_left);
    if (image1_crop_right.x != tmp_stitch0.x || image1_crop_right.width != tmp_stitch0.width ||
            image0_crop_left.x != tmp_stitch1.x || image0_crop_left.width != tmp_stitch1.width)
        x_offset0 = 0.0f;
    XCAM_LOG_INFO (
        "Stiching area 0: image0_left_area(x:%d, width:%d), image1_right_area(x:%d, width:%d)",
        image0_crop_left.x, image0_crop_left.width, image1_crop_right.x, image1_crop_right.width);

    tmp_stitch0 = image0_crop_right;
    tmp_stitch1 = image1_crop_left;
    out_image1 = calc_match_optical_flow (image0_right_rgb, image1_left_rgb, corner0_right, corner1_left,
                                          status1, err1, valid_count1, mean_offset1, x_offset1);
    image0_crop_right.x -= x_offset1;
    adjust_stitch_area (dst_width, image0_crop_right, image1_crop_left);
    if (image0_crop_right.x != tmp_stitch0.x || image0_crop_right.width != tmp_stitch0.width ||
            image1_crop_left.x != tmp_stitch1.x || image1_crop_left.width != tmp_stitch1.width)
        x_offset1 = 0.0f;
    XCAM_LOG_INFO (
        "Stiching area 1: image0_right_area(x:%d, width:%d), image1_left_area(x:%d, width:%d)",
        image0_crop_right.x, image0_crop_right.width, image1_crop_left.x, image1_crop_left.width);

#if XCAM_OF_DEBUG
    char file_name[1024];
    char *tmp_name = strdup (input_name);
    char *prefix = strtok (tmp_name, ".");
    snprintf (file_name, 1024, "%s_%d_OF_stitching_0.jpg", prefix, frame_num);
    imwrite (file_name, out_image0);
    XCAM_LOG_INFO ("write feature match: %s", file_name);

    snprintf (file_name, 1024, "%s_%d_OF_stitching_1.jpg", prefix, frame_num);
    imwrite (file_name, out_image1);
    XCAM_LOG_INFO ("write feature match: %s", file_name);
    free (tmp_name);
#else
    XCAM_UNUSED (input_name);
    XCAM_UNUSED (frame_num);
#endif
}

