/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*
* Copyright (c) 2016, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

#include <stdlib.h>

#include "EbPictureControlSet.h"
#include "EbReferenceObject.h"

#include "EbInterPrediction.h"
#include "EbSvtAv1.h"
#include "EbDefinitions.h"
#include "EbAdaptiveMotionVectorPrediction.h"

#include "EbModeDecisionProcess.h"
#include <smmintrin.h> /* SSE4.1 */

#include "convolve.h"
#include "aom_dsp_rtcd.h"

#if COMP_DIFF
#include "EbRateDistortionCost.h"
#endif
#define MVBOUNDLOW    36    //  (80-71)<<2 // 80 = ReferencePadding ; minus 71 is derived from the expression -64 + 1 - 8, and plus 7 is derived from expression -1 + 8
#define MVBOUNDHIGH   348   //  (80+7)<<2
#define REFPADD_QPEL  320   //  (16+64)<<2

#define UNUSED_VARIABLE(x) (void)(x)

#define AOM_INTERP_EXTEND 4

#define SCALE_SUBPEL_BITS 10
#define SCALE_SUBPEL_SHIFTS (1 << SCALE_SUBPEL_BITS)
#define SCALE_SUBPEL_MASK (SCALE_SUBPEL_SHIFTS - 1)
#define SCALE_EXTRA_BITS (SCALE_SUBPEL_BITS - SUBPEL_BITS)
#define SCALE_EXTRA_OFF ((1 << SCALE_EXTRA_BITS) / 2)

#define RS_SUBPEL_BITS 6
#define RS_SUBPEL_MASK ((1 << RS_SUBPEL_BITS) - 1)
#define RS_SCALE_SUBPEL_BITS 14
#define RS_SCALE_SUBPEL_MASK ((1 << RS_SCALE_SUBPEL_BITS) - 1)
#define RS_SCALE_EXTRA_BITS (RS_SCALE_SUBPEL_BITS - RS_SUBPEL_BITS)

#define BIL_SUBPEL_BITS 3
#define BIL_SUBPEL_SHIFTS (1 << BIL_SUBPEL_BITS)

#define ROUND0_BITS 3
#define COMPOUND_ROUND1_BITS 7

typedef struct SubpelParams {
    int32_t xs;
    int32_t ys;
    int32_t subpel_x;
    int32_t subpel_y;
} SubpelParams;

//extern INLINE void clamp_mv(MV *mv, int32_t min_col, int32_t max_col, int32_t min_row,int32_t max_row);

static INLINE void clamp_mv(MV *mv, int32_t min_col, int32_t max_col, int32_t min_row,
    int32_t max_row) {
    mv->col = (int16_t)clamp(mv->col, min_col, max_col);
    mv->row = (int16_t)clamp(mv->row, min_row, max_row);
}

extern void av1_set_ref_frame(MvReferenceFrame *rf,
    int8_t ref_frame_type);

static INLINE MV clamp_mv_to_umv_border_sb(const MacroBlockD *xd,
    const MV *src_mv, int32_t bw, int32_t bh,
    int32_t ss_x, int32_t ss_y) {
    // If the MV points so far into the UMV border that no visible pixels
    // are used for reconstruction, the subpel part of the MV can be
    // discarded and the MV limited to 16 pixels with equivalent results.
    const int32_t spel_left = (AOM_INTERP_EXTEND + bw) << SUBPEL_BITS;
    const int32_t spel_right = spel_left - SUBPEL_SHIFTS;
    const int32_t spel_top = (AOM_INTERP_EXTEND + bh) << SUBPEL_BITS;
    const int32_t spel_bottom = spel_top - SUBPEL_SHIFTS;
    MV clamped_mv = { (int16_t)(src_mv->row * (1 << (1 - ss_y))),
        (int16_t)(src_mv->col * (1 << (1 - ss_x))) };
    assert(ss_x <= 1);
    assert(ss_y <= 1);

    clamp_mv(&clamped_mv,
        xd->mb_to_left_edge   * (1 << (1 - ss_x)) - spel_left,
        xd->mb_to_right_edge  * (1 << (1 - ss_x)) + spel_right,
        xd->mb_to_top_edge    * (1 << (1 - ss_y)) - spel_top,
        xd->mb_to_bottom_edge * (1 << (1 - ss_y)) + spel_bottom);

    return clamped_mv;
}

DECLARE_ALIGNED(256, const InterpKernel,
sub_pel_filters_8[SUBPEL_SHIFTS]) = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },{ 0, 2, -6, 126, 8, -2, 0, 0 },
    { 0, 2, -10, 122, 18, -4, 0, 0 },{ 0, 2, -12, 116, 28, -8, 2, 0 },
    { 0, 2, -14, 110, 38, -10, 2, 0 },{ 0, 2, -14, 102, 48, -12, 2, 0 },
    { 0, 2, -16, 94, 58, -12, 2, 0 },{ 0, 2, -14, 84, 66, -12, 2, 0 },
    { 0, 2, -14, 76, 76, -14, 2, 0 },{ 0, 2, -12, 66, 84, -14, 2, 0 },
    { 0, 2, -12, 58, 94, -16, 2, 0 },{ 0, 2, -12, 48, 102, -14, 2, 0 },
    { 0, 2, -10, 38, 110, -14, 2, 0 },{ 0, 2, -8, 28, 116, -12, 2, 0 },
    { 0, 0, -4, 18, 122, -10, 2, 0 },{ 0, 0, -2, 8, 126, -6, 2, 0 }
};
DECLARE_ALIGNED(256, const InterpKernel,
sub_pel_filters_4[SUBPEL_SHIFTS]) = {
    { 0, 0, 0, 128, 0, 0, 0, 0 },{ 0, 0, -4, 126, 8, -2, 0, 0 },
    { 0, 0, -8, 122, 18, -4, 0, 0 },{ 0, 0, -10, 116, 28, -6, 0, 0 },
    { 0, 0, -12, 110, 38, -8, 0, 0 },{ 0, 0, -12, 102, 48, -10, 0, 0 },
    { 0, 0, -14, 94, 58, -10, 0, 0 },{ 0, 0, -12, 84, 66, -10, 0, 0 },
    { 0, 0, -12, 76, 76, -12, 0, 0 },{ 0, 0, -10, 66, 84, -12, 0, 0 },
    { 0, 0, -10, 58, 94, -14, 0, 0 },{ 0, 0, -10, 48, 102, -12, 0, 0 },
    { 0, 0, -8, 38, 110, -12, 0, 0 },{ 0, 0, -6, 28, 116, -10, 0, 0 },
    { 0, 0, -4, 18, 122, -8, 0, 0 },{ 0, 0, -2, 8, 126, -4, 0, 0 }
};

#define MAX_FILTER_TAP 8

#if COMP_MODE
int get_relative_dist_enc(SeqHeader *seq_header, int ref_hint, int order_hint)
{
    int diff, m;
    if (!seq_header->order_hint_info.enable_order_hint)
        return 0;
    diff = ref_hint - order_hint;
    m = 1 << (seq_header->order_hint_info.order_hint_bits - 1);
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

static const int quant_dist_weight[4][2] = {
  { 2, 3 }, { 2, 5 }, { 2, 7 }, { 1, MAX_FRAME_DISTANCE }
};
static const int quant_dist_lookup_table[2][4][2] = {
  { { 9, 7 }, { 11, 5 }, { 12, 4 }, { 13, 3 } },
  { { 7, 9 }, { 5, 11 }, { 4, 12 }, { 3, 13 } },
};

void av1_dist_wtd_comp_weight_assign(
    //const AV1_COMMON *cm,
    //const MB_MODE_INFO *mbmi,
    PictureControlSet  *picture_control_set_ptr,
    int cur_frame_index ,
    int bck_frame_index ,
    int fwd_frame_index,
    int compound_idx,
    int order_idx,
    int *fwd_offset, int *bck_offset,
    int *use_dist_wtd_comp_avg,
    int is_compound) {


    assert(fwd_offset != NULL && bck_offset != NULL);
    if (!is_compound || /*mbmi->*/compound_idx) {
        *use_dist_wtd_comp_avg = 0;
        return;
    }

    *use_dist_wtd_comp_avg = 1;
    //const RefCntBuffer *const bck_buf = get_ref_frame_buf(cm, mbmi->ref_frame[0]);
    //const RefCntBuffer *const fwd_buf = get_ref_frame_buf(cm, mbmi->ref_frame[1]);
    //const int cur_frame_index = cm->cur_frame->order_hint;
    //int bck_frame_index = 0, fwd_frame_index = 0;

    //if (bck_buf != NULL) bck_frame_index = bck_buf->order_hint;
    //if (fwd_buf != NULL) fwd_frame_index = fwd_buf->order_hint;

    int d0 = clamp(abs(get_relative_dist_enc(&picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->seq_header, //&cm->seq_params.order_hint_info,
        fwd_frame_index, cur_frame_index)),
        0, MAX_FRAME_DISTANCE);
    int d1 = clamp(abs(get_relative_dist_enc(&picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->seq_header, //&cm->seq_params.order_hint_info,
        cur_frame_index, bck_frame_index)),
        0, MAX_FRAME_DISTANCE);

    const int order = d0 <= d1;

    if (d0 == 0 || d1 == 0) {
        *fwd_offset = quant_dist_lookup_table[order_idx][3][order];
        *bck_offset = quant_dist_lookup_table[order_idx][3][1 - order];
        return;
    }

    int i;
    for (i = 0; i < 3; ++i) {
        int c0 = quant_dist_weight[i][order];
        int c1 = quant_dist_weight[i][!order];
        int d0_c0 = d0 * c0;
        int d1_c1 = d1 * c1;
        if ((d0 > d1 && d0_c0 < d1_c1) || (d0 <= d1 && d0_c0 > d1_c1)) break;
    }

    *fwd_offset = quant_dist_lookup_table[order_idx][i][order];
    *bck_offset = quant_dist_lookup_table[order_idx][i][1 - order];
}
#endif
void av1_convolve_2d_sr_c(const uint8_t *src, int32_t src_stride, uint8_t *dst,
    int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    int16_t im_block[(MAX_SB_SIZE + MAX_FILTER_TAP - 1) * MAX_SB_SIZE];
    int32_t im_h = h + filter_params_y->taps - 1;
    int32_t im_stride = w;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bd = 8;
    const int32_t bits =
        FILTER_BITS * 2 - conv_params->round_0 - conv_params->round_1;

    // horizontal filter
    const uint8_t *src_horiz = src - fo_vert * src_stride;
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < im_h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = (1 << (bd + FILTER_BITS - 1));
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                sum += x_filter[k] * src_horiz[y * src_stride + x - fo_horiz + k];
            assert(0 <= sum && sum < (1 << (bd + FILTER_BITS + 1)));
            im_block[y * im_stride + x] =
                (int16_t)ROUND_POWER_OF_TWO(sum, conv_params->round_0);
        }
    }

    // vertical filter
    int16_t *src_vert = im_block + fo_vert * im_stride;
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = 1 << offset_bits;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                sum += y_filter[k] * src_vert[(y - fo_vert + k) * im_stride + x];
            assert(0 <= sum && sum < (1 << (offset_bits + 2)));
            int16_t res = (ConvBufType)(ROUND_POWER_OF_TWO(sum, conv_params->round_1) -
                ((1 << (offset_bits - conv_params->round_1)) +
                (1 << (offset_bits - conv_params->round_1 - 1))));
            dst[y * dst_stride + x] = (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(res, bits), 8);
        }
    }
}

void av1_convolve_y_sr_c(const uint8_t *src, int32_t src_stride, uint8_t *dst,
    int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    assert(filter_params_y != NULL);
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    (void)filter_params_x;
    (void)subpel_x_q4;
    (void)conv_params;

    assert(conv_params->round_0 <= FILTER_BITS);
    assert(((conv_params->round_0 + conv_params->round_1) <= (FILTER_BITS + 1)) ||
        ((conv_params->round_0 + conv_params->round_1) == (2 * FILTER_BITS)));

    // vertical filter
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                res += y_filter[k] * src[(y - fo_vert + k) * src_stride + x];
            dst[y * dst_stride + x] =
                (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(res, FILTER_BITS), 8);
        }
    }
}

void av1_convolve_x_sr_c(const uint8_t *src, int32_t src_stride, uint8_t *dst,
    int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_0;
    (void)filter_params_y;
    (void)subpel_y_q4;
    (void)conv_params;

    assert(bits >= 0);
    assert((FILTER_BITS - conv_params->round_1) >= 0 ||
        ((conv_params->round_0 + conv_params->round_1) == 2 * FILTER_BITS));

    // horizontal filter
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                res += x_filter[k] * src[y * src_stride + x - fo_horiz + k];
            res = ROUND_POWER_OF_TWO(res, conv_params->round_0);
            dst[y * dst_stride + x] = (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(res, bits), 8);
        }
    }
}

void av1_convolve_2d_copy_sr_c(const uint8_t *src, int32_t src_stride, uint8_t *dst,
    int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params) {
    (void)filter_params_x;
    (void)filter_params_y;
    (void)subpel_x_q4;
    (void)subpel_y_q4;
    (void)conv_params;

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x)
            dst[y * dst_stride + x] = src[y * src_stride + x];
    }
}

void av1_jnt_convolve_2d_c(const uint8_t *src, int32_t src_stride, uint8_t *dst8,
    int32_t dst8_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    int16_t im_block[(MAX_SB_SIZE + MAX_FILTER_TAP - 1) * MAX_SB_SIZE];
    int32_t im_h = h + filter_params_y->taps - 1;
    int32_t im_stride = w;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bd = 8;
    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;

    // horizontal filter
    const uint8_t *src_horiz = src - fo_vert * src_stride;
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < im_h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = (1 << (bd + FILTER_BITS - 1));
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                sum += x_filter[k] * src_horiz[y * src_stride + x - fo_horiz + k];
            assert(0 <= sum && sum < (1 << (bd + FILTER_BITS + 1)));
            im_block[y * im_stride + x] =
                (int16_t)ROUND_POWER_OF_TWO(sum, conv_params->round_0);
        }
    }

    // vertical filter
    int16_t *src_vert = im_block + fo_vert * im_stride;
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = 1 << offset_bits;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                sum += y_filter[k] * src_vert[(y - fo_vert + k) * im_stride + x];
            assert(0 <= sum && sum < (1 << (offset_bits + 2)));
            ConvBufType res = (ConvBufType)ROUND_POWER_OF_TWO(sum, conv_params->round_1);
            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                    printf("here");
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= (1 << (offset_bits - conv_params->round_1)) +
                    (1 << (offset_bits - conv_params->round_1 - 1));
                dst8[y * dst8_stride + x] =
                    (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), 8);
            }
            else
                dst[y * dst_stride + x] = res;
        }
    }
}

void av1_jnt_convolve_y_c(const uint8_t *src, int32_t src_stride, uint8_t *dst8,
    int32_t dst8_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_0;
    const int32_t bd = 8;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;
    (void)filter_params_x;
    (void)subpel_x_q4;

    // vertical filter
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                res += y_filter[k] * src[(y - fo_vert + k) * src_stride + x];
            res *= (1 << bits);
            res = ROUND_POWER_OF_TWO(res, conv_params->round_1) + round_offset;

            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst8[y * dst8_stride + x] =
                    (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), 8);
            }
            else
                dst[y * dst_stride + x] = (ConvBufType)res;
        }
    }
}

void av1_jnt_convolve_x_c(const uint8_t *src, int32_t src_stride, uint8_t *dst8,
    int32_t dst8_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_1;
    const int32_t bd = 8;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;
    (void)filter_params_y;
    (void)subpel_y_q4;

    // horizontal filter
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                res += x_filter[k] * src[y * src_stride + x - fo_horiz + k];
            res = (1 << bits) * ROUND_POWER_OF_TWO(res, conv_params->round_0);
            res += round_offset;

            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst8[y * dst8_stride + x] =
                    (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), 8);
            }
            else
                dst[y * dst_stride + x] = (ConvBufType)res;
        }
    }
}

void av1_jnt_convolve_2d_copy_c(const uint8_t *src, int32_t src_stride,
    uint8_t *dst8, int32_t dst8_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x,
    InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params)
{
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t bits =
        FILTER_BITS * 2 - conv_params->round_1 - conv_params->round_0;
    const int32_t bd = 8;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    (void)filter_params_x;
    (void)filter_params_y;
    (void)subpel_x_q4;
    (void)subpel_y_q4;

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            ConvBufType res = src[y * src_stride + x] << bits;
            res += (ConvBufType)round_offset;

            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst8[y * dst8_stride + x] = (uint8_t)clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, bits), 8);
            }
            else
                dst[y * dst_stride + x] = res;
        }
    }
}

void av1_highbd_convolve_2d_copy_sr_c(
    const uint16_t *src, int32_t src_stride, uint16_t *dst, int32_t dst_stride, int32_t w,
    int32_t h, const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y, const int32_t subpel_x_q4,
    const int32_t subpel_y_q4, ConvolveParams *conv_params, int32_t bd) {
    (void)filter_params_x;
    (void)filter_params_y;
    (void)subpel_x_q4;
    (void)subpel_y_q4;
    (void)conv_params;
    (void)bd;

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x)
            dst[y * dst_stride + x] = src[y * src_stride + x];
    }
}

void av1_highbd_convolve_x_sr_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst, int32_t dst_stride, int32_t w, int32_t h,
    const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd) {
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_0;
    (void)filter_params_y;
    (void)subpel_y_q4;

    assert(bits >= 0);
    assert((FILTER_BITS - conv_params->round_1) >= 0 ||
        ((conv_params->round_0 + conv_params->round_1) == 2 * FILTER_BITS));

    // horizontal filter
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                res += x_filter[k] * src[y * src_stride + x - fo_horiz + k];
            res = ROUND_POWER_OF_TWO(res, conv_params->round_0);
            dst[y * dst_stride + x] =
                clip_pixel_highbd(ROUND_POWER_OF_TWO(res, bits), bd);
        }
    }
}

void av1_highbd_convolve_y_sr_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst, int32_t dst_stride, int32_t w, int32_t h,
    const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd) {
    assert(filter_params_y != NULL);
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    (void)filter_params_x;
    (void)subpel_x_q4;
    (void)conv_params;

    assert(conv_params->round_0 <= FILTER_BITS);
    assert(((conv_params->round_0 + conv_params->round_1) <= (FILTER_BITS + 1)) ||
        ((conv_params->round_0 + conv_params->round_1) == (2 * FILTER_BITS)));
    // vertical filter
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                res += y_filter[k] * src[(y - fo_vert + k) * src_stride + x];
            dst[y * dst_stride + x] =
                clip_pixel_highbd(ROUND_POWER_OF_TWO(res, FILTER_BITS), bd);
        }
    }
}

void av1_highbd_convolve_2d_sr_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst, int32_t dst_stride, int32_t w, int32_t h,
    const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd) {
    int16_t im_block[(MAX_SB_SIZE + MAX_FILTER_TAP - 1) * MAX_SB_SIZE];
    int32_t im_h = h + filter_params_y->taps - 1;
    int32_t im_stride = w;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bits =
        FILTER_BITS * 2 - conv_params->round_0 - conv_params->round_1;
    assert(bits >= 0);

    // horizontal filter
    const uint16_t *src_horiz = src - fo_vert * src_stride;
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < im_h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = (1 << (bd + FILTER_BITS - 1));
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                sum += x_filter[k] * src_horiz[y * src_stride + x - fo_horiz + k];
            assert(0 <= sum && sum < (1 << (bd + FILTER_BITS + 1)));
            im_block[y * im_stride + x] = (ConvBufType)
                ROUND_POWER_OF_TWO(sum, conv_params->round_0);
        }
    }

    // vertical filter
    int16_t *src_vert = im_block + fo_vert * im_stride;
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t sum = 1 << offset_bits;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                sum += y_filter[k] * src_vert[(y - fo_vert + k) * im_stride + x];
            assert(0 <= sum && sum < (1 << (offset_bits + 2)));
            int32_t res = ROUND_POWER_OF_TWO(sum, conv_params->round_1) -
                ((1 << (offset_bits - conv_params->round_1)) +
                (1 << (offset_bits - conv_params->round_1 - 1)));
            dst[y * dst_stride + x] =
                clip_pixel_highbd(ROUND_POWER_OF_TWO(res, bits), bd);
        }
    }
}

void av1_highbd_jnt_convolve_x_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst16, int32_t dst16_stride, int32_t w,
    int32_t h, const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd) {
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_1;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;
    assert(round_bits >= 0);
    (void)filter_params_y;
    (void)subpel_y_q4;
    assert(bits >= 0);
    // horizontal filter
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_x->taps; ++k)
                res += x_filter[k] * src[y * src_stride + x - fo_horiz + k];
            res = (1 << bits) * ROUND_POWER_OF_TWO(res, conv_params->round_0);
            res += round_offset;

            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst16[y * dst16_stride + x] =
                    clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), bd);
            }
            else
                dst[y * dst_stride + x] = (ConvBufType)res;
        }
    }
}

void av1_highbd_jnt_convolve_y_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst16, int32_t dst16_stride, int32_t w,
    int32_t h, const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd) {
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t bits = FILTER_BITS - conv_params->round_0;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;
    assert(round_bits >= 0);
    (void)filter_params_x;
    (void)subpel_x_q4;
    assert(bits >= 0);
    // vertical filter
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            int32_t res = 0;
            for (int32_t k = 0; k < filter_params_y->taps; ++k)
                res += y_filter[k] * src[(y - fo_vert + k) * src_stride + x];
            res *= (1 << bits);
            res = ROUND_POWER_OF_TWO(res, conv_params->round_1) + round_offset;

            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst16[y * dst16_stride + x] =
                    clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), bd);
            }
            else
                dst[y * dst_stride + x] = (ConvBufType)res;
        }
    }
}

void av1_highbd_jnt_convolve_2d_copy_c(
    const uint16_t *src, int32_t src_stride, uint16_t *dst16, int32_t dst16_stride,
    int32_t w, int32_t h, const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y, const int32_t subpel_x_q4,
    const int32_t subpel_y_q4, ConvolveParams *conv_params, int32_t bd) {
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    const int32_t bits =
        FILTER_BITS * 2 - conv_params->round_1 - conv_params->round_0;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int32_t round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    assert(bits >= 0);
    (void)filter_params_x;
    (void)filter_params_y;
    (void)subpel_x_q4;
    (void)subpel_y_q4;

    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            ConvBufType res = src[y * src_stride + x] << bits;
            res += (ConvBufType)round_offset;
            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= round_offset;
                dst16[y * dst16_stride + x] =
                    clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, bits), bd);
            }
            else
                dst[y * dst_stride + x] = res;
        }
    }
}

void av1_highbd_jnt_convolve_2d_c(const uint16_t *src, int32_t src_stride,
    uint16_t *dst16, int32_t dst16_stride, int32_t w,
    int32_t h, const InterpFilterParams *filter_params_x,
    const InterpFilterParams *filter_params_y,
    const int32_t subpel_x_q4, const int32_t subpel_y_q4,
    ConvolveParams *conv_params, int32_t bd)

{
    int32_t x, y, k;
    int16_t im_block[(MAX_SB_SIZE + MAX_FILTER_TAP - 1) * MAX_SB_SIZE];
    ConvBufType *dst = conv_params->dst;
    int32_t dst_stride = conv_params->dst_stride;
    int32_t im_h = h + filter_params_y->taps - 1;
    int32_t im_stride = w;
    const int32_t fo_vert = filter_params_y->taps / 2 - 1;
    const int32_t fo_horiz = filter_params_x->taps / 2 - 1;

    const int32_t round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;
    assert(round_bits >= 0);

    // horizontal filter
    const uint16_t *src_horiz = src - fo_vert * src_stride;
    const int16_t *x_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_x, subpel_x_q4 & SUBPEL_MASK);
    for (y = 0; y < im_h; ++y) {
        for (x = 0; x < w; ++x) {
            int32_t sum = (1 << (bd + FILTER_BITS - 1));
            for (k = 0; k < filter_params_x->taps; ++k)
                sum += x_filter[k] * src_horiz[y * src_stride + x - fo_horiz + k];
            assert(0 <= sum && sum < (1 << (bd + FILTER_BITS + 1)));
            (void)bd;
            im_block[y * im_stride + x] =
                (int16_t)ROUND_POWER_OF_TWO(sum, conv_params->round_0);
        }
    }

    // vertical filter
    int16_t *src_vert = im_block + fo_vert * im_stride;
    const int32_t offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int16_t *y_filter = av1_get_interp_filter_subpel_kernel(
        *filter_params_y, subpel_y_q4 & SUBPEL_MASK);
    for (y = 0; y < h; ++y) {
        for (x = 0; x < w; ++x) {
            int32_t sum = 1 << offset_bits;
            for (k = 0; k < filter_params_y->taps; ++k)
                sum += y_filter[k] * src_vert[(y - fo_vert + k) * im_stride + x];
            assert(0 <= sum && sum < (1 << (offset_bits + 2)));
            ConvBufType res = (ConvBufType)ROUND_POWER_OF_TWO(sum, conv_params->round_1);
            if (conv_params->do_average) {
                int32_t tmp = dst[y * dst_stride + x];
                if (conv_params->use_jnt_comp_avg) {
                    tmp = tmp * conv_params->fwd_offset + res * conv_params->bck_offset;
                    tmp = tmp >> DIST_PRECISION_BITS;
                }
                else {
                    tmp += res;
                    tmp = tmp >> 1;
                }
                tmp -= (1 << (offset_bits - conv_params->round_1)) +
                    (1 << (offset_bits - conv_params->round_1 - 1));
                dst16[y * dst16_stride + x] =
                    clip_pixel_highbd(ROUND_POWER_OF_TWO(tmp, round_bits), bd);
            }
            else
                dst[y * dst_stride + x] = res;
        }
    }
}

aom_highbd_convolve_fn_t convolveHbd[/*subX*/2][/*subY*/2][/*bi*/2];
void asmSetConvolveHbdAsmTable(void)
{
    convolveHbd[0][0][0] = av1_highbd_convolve_2d_copy_sr;
    convolveHbd[0][0][1] = av1_highbd_jnt_convolve_2d_copy;

    convolveHbd[0][1][0] = av1_highbd_convolve_y_sr;
    convolveHbd[0][1][1] = av1_highbd_jnt_convolve_y;

    convolveHbd[1][0][0] = av1_highbd_convolve_x_sr;
    convolveHbd[1][0][1] = av1_highbd_jnt_convolve_x;

    convolveHbd[1][1][0] = av1_highbd_convolve_2d_sr;
    convolveHbd[1][1][1] = av1_highbd_jnt_convolve_2d;
}

aom_convolve_fn_t convolve[/*subX*/2][/*subY*/2][/*bi*/2];
void asmSetConvolveAsmTable(void)
{
    convolve[0][0][0] = av1_convolve_2d_copy_sr;
    convolve[0][0][1] = av1_jnt_convolve_2d_copy;

    convolve[0][1][0] = av1_convolve_y_sr;
    convolve[0][1][1] = av1_jnt_convolve_y;

    convolve[1][0][0] = av1_convolve_x_sr;
    convolve[1][0][1] = av1_jnt_convolve_x;

    convolve[1][1][0] = av1_convolve_2d_sr;
    convolve[1][1][1] = av1_jnt_convolve_2d;
}

InterpFilterParams av1RegularFilter = { (const int16_t *)sub_pel_filters_8, SUBPEL_TAPS, SUBPEL_SHIFTS, EIGHTTAP_REGULAR };
InterpFilterParams av1RegularFilterW4 = { (const int16_t *)sub_pel_filters_4, SUBPEL_TAPS, SUBPEL_SHIFTS, EIGHTTAP_REGULAR };

DECLARE_ALIGNED(256, const InterpKernel,
sub_pel_filters_8sharp[SUBPEL_SHIFTS]) = {
{ 0, 0, 0, 128, 0, 0, 0, 0 },         { -2, 2, -6, 126, 8, -2, 2, 0 },
{ -2, 6, -12, 124, 16, -6, 4, -2 },   { -2, 8, -18, 120, 26, -10, 6, -2 },
{ -4, 10, -22, 116, 38, -14, 6, -2 }, { -4, 10, -22, 108, 48, -18, 8, -2 },
{ -4, 10, -24, 100, 60, -20, 8, -2 }, { -4, 10, -24, 90, 70, -22, 10, -2 },
{ -4, 12, -24, 80, 80, -24, 12, -4 }, { -2, 10, -22, 70, 90, -24, 10, -4 },
{ -2, 8, -20, 60, 100, -24, 10, -4 }, { -2, 8, -18, 48, 108, -22, 10, -4 },
{ -2, 6, -14, 38, 116, -22, 10, -4 }, { -2, 6, -10, 26, 120, -18, 8, -2 },
{ -2, 4, -6, 16, 124, -12, 6, -2 },   { 0, 2, -2, 8, 126, -6, 2, -2 }
};

DECLARE_ALIGNED(256, const InterpKernel,
sub_pel_filters_8smooth[SUBPEL_SHIFTS]) = {
{ 0, 0, 0, 128, 0, 0, 0, 0 },     { 0, 2, 28, 62, 34, 2, 0, 0 },
{ 0, 0, 26, 62, 36, 4, 0, 0 },    { 0, 0, 22, 62, 40, 4, 0, 0 },
{ 0, 0, 20, 60, 42, 6, 0, 0 },    { 0, 0, 18, 58, 44, 8, 0, 0 },
{ 0, 0, 16, 56, 46, 10, 0, 0 },   { 0, -2, 16, 54, 48, 12, 0, 0 },
{ 0, -2, 14, 52, 52, 14, -2, 0 }, { 0, 0, 12, 48, 54, 16, -2, 0 },
{ 0, 0, 10, 46, 56, 16, 0, 0 },   { 0, 0, 8, 44, 58, 18, 0, 0 },
{ 0, 0, 6, 42, 60, 20, 0, 0 },    { 0, 0, 4, 40, 62, 22, 0, 0 },
{ 0, 0, 4, 36, 62, 26, 0, 0 },    { 0, 0, 2, 34, 62, 28, 2, 0 }
};
DECLARE_ALIGNED(256, const InterpKernel,
bilinear_filters[SUBPEL_SHIFTS]) = {
{ 0, 0, 0, 128, 0, 0, 0, 0 },  { 0, 0, 0, 120, 8, 0, 0, 0 },
{ 0, 0, 0, 112, 16, 0, 0, 0 }, { 0, 0, 0, 104, 24, 0, 0, 0 },
{ 0, 0, 0, 96, 32, 0, 0, 0 },  { 0, 0, 0, 88, 40, 0, 0, 0 },
{ 0, 0, 0, 80, 48, 0, 0, 0 },  { 0, 0, 0, 72, 56, 0, 0, 0 },
{ 0, 0, 0, 64, 64, 0, 0, 0 },  { 0, 0, 0, 56, 72, 0, 0, 0 },
{ 0, 0, 0, 48, 80, 0, 0, 0 },  { 0, 0, 0, 40, 88, 0, 0, 0 },
{ 0, 0, 0, 32, 96, 0, 0, 0 },  { 0, 0, 0, 24, 104, 0, 0, 0 },
{ 0, 0, 0, 16, 112, 0, 0, 0 }, { 0, 0, 0, 8, 120, 0, 0, 0 }
};
DECLARE_ALIGNED(256, const InterpKernel,
sub_pel_filters_4smooth[SUBPEL_SHIFTS]) = {
{ 0, 0, 0, 128, 0, 0, 0, 0 },   { 0, 0, 30, 62, 34, 2, 0, 0 },
{ 0, 0, 26, 62, 36, 4, 0, 0 },  { 0, 0, 22, 62, 40, 4, 0, 0 },
{ 0, 0, 20, 60, 42, 6, 0, 0 },  { 0, 0, 18, 58, 44, 8, 0, 0 },
{ 0, 0, 16, 56, 46, 10, 0, 0 }, { 0, 0, 14, 54, 48, 12, 0, 0 },
{ 0, 0, 12, 52, 52, 12, 0, 0 }, { 0, 0, 12, 48, 54, 14, 0, 0 },
{ 0, 0, 10, 46, 56, 16, 0, 0 }, { 0, 0, 8, 44, 58, 18, 0, 0 },
{ 0, 0, 6, 42, 60, 20, 0, 0 },  { 0, 0, 4, 40, 62, 22, 0, 0 },
{ 0, 0, 4, 36, 62, 26, 0, 0 },  { 0, 0, 2, 34, 62, 30, 0, 0 }
};
static const InterpFilterParams
av1_interp_filter_params_list[SWITCHABLE_FILTERS + 1] = {
  { (const int16_t *)sub_pel_filters_8, SUBPEL_TAPS, SUBPEL_SHIFTS,
    EIGHTTAP_REGULAR },
  { (const int16_t *)sub_pel_filters_8smooth, SUBPEL_TAPS, SUBPEL_SHIFTS,
    EIGHTTAP_SMOOTH },
  { (const int16_t *)sub_pel_filters_8sharp, SUBPEL_TAPS, SUBPEL_SHIFTS,
    MULTITAP_SHARP },
  { (const int16_t *)bilinear_filters, SUBPEL_TAPS, SUBPEL_SHIFTS,
    BILINEAR }
};
static const InterpFilterParams av1_interp_4tap[2] = {
  { (const int16_t *)sub_pel_filters_4, SUBPEL_TAPS, SUBPEL_SHIFTS,
    EIGHTTAP_REGULAR },
  { (const int16_t *)sub_pel_filters_4smooth, SUBPEL_TAPS, SUBPEL_SHIFTS,
    EIGHTTAP_SMOOTH },
};
InterpFilterParams av1_get_interp_filter_params_with_block_size(
    const InterpFilter interp_filter, const int32_t w) {
    if (w <= 4 &&
        (interp_filter == MULTITAP_SHARP || interp_filter == EIGHTTAP_REGULAR))
        return av1_interp_4tap[0];
    else if (w <= 4 && interp_filter == EIGHTTAP_SMOOTH)
        return av1_interp_4tap[1];

    return av1_interp_filter_params_list[interp_filter];
}

static void av1_get_convolve_filter_params( uint32_t interp_filters,
    InterpFilterParams *params_x, InterpFilterParams *params_y,
    int32_t w, int32_t h)
{
    InterpFilter filter_x = av1_extract_interp_filter(interp_filters, 1);
    InterpFilter filter_y = av1_extract_interp_filter(interp_filters, 0);
    *params_x = av1_get_interp_filter_params_with_block_size(filter_x, w);
    *params_y = av1_get_interp_filter_params_with_block_size(filter_y, h);
}

int32_t is_inter_block(const MbModeInfo *mbmi);
BlockSize scale_chroma_bsize(BlockSize bsize, int32_t subsampling_x,
    int32_t subsampling_y);

// A special 2-tap bilinear filter for IntraBC chroma. IntraBC uses full pixel
// MV for luma. If sub-sampling exists, chroma may possibly use half-pel MV.
DECLARE_ALIGNED(256, static const int16_t, av1_intrabc_bilinear_filter[2]) = {
  64,
  64,
};

static const InterpFilterParams av1_intrabc_filter_params = {
  av1_intrabc_bilinear_filter, 2, 0, BILINEAR
};
static void convolve_2d_for_intrabc(const uint8_t *src, int src_stride,
    uint8_t *dst, int dst_stride, int w, int h,
    int subpel_x_q4, int subpel_y_q4,
    ConvolveParams *conv_params)
{
    const InterpFilterParams *filter_params_x =
        subpel_x_q4 ? &av1_intrabc_filter_params : NULL;
    const InterpFilterParams *filter_params_y =
        subpel_y_q4 ? &av1_intrabc_filter_params : NULL;
    if (subpel_x_q4 != 0 && subpel_y_q4 != 0) {
        av1_convolve_2d_sr_c(src, src_stride, dst, dst_stride, w, h,
            (InterpFilterParams *)filter_params_x, (InterpFilterParams *)filter_params_y, 0, 0, conv_params);
    }
    else if (subpel_x_q4 != 0) {
        av1_convolve_x_sr_c(src, src_stride, dst, dst_stride, w, h, (InterpFilterParams *)filter_params_x,
            (InterpFilterParams *)filter_params_y, 0, 0, conv_params);
    }
    else {
        av1_convolve_y_sr_c(src, src_stride, dst, dst_stride, w, h, (InterpFilterParams *)filter_params_x,
            (InterpFilterParams *)filter_params_y, 0, 0, conv_params);
    }
}
static void highbd_convolve_2d_for_intrabc(const uint16_t *src, int src_stride,
    uint16_t *dst, int dst_stride, int w,
    int h, int subpel_x_q4,
    int subpel_y_q4,
    ConvolveParams *conv_params,
    int bd) {
    const InterpFilterParams *filter_params_x =
        subpel_x_q4 ? &av1_intrabc_filter_params : NULL;
    const InterpFilterParams *filter_params_y =
        subpel_y_q4 ? &av1_intrabc_filter_params : NULL;
    if (subpel_x_q4 != 0 && subpel_y_q4 != 0) {
        av1_highbd_convolve_2d_sr_c(src, src_stride, dst, dst_stride, w, h,
            filter_params_x, filter_params_y, 0, 0,
            conv_params, bd);
    }
    else if (subpel_x_q4 != 0) {
        av1_highbd_convolve_x_sr_c(src, src_stride, dst, dst_stride, w, h,
            filter_params_x, filter_params_y, 0, 0,
            conv_params, bd);
    }
    else {
        av1_highbd_convolve_y_sr_c(src, src_stride, dst, dst_stride, w, h,
            filter_params_x, filter_params_y, 0, 0,
            conv_params, bd);
    }
}
#if COMP_DIFF

#define USE_PRECOMPUTED_WEDGE_SIGN 1
#define USE_PRECOMPUTED_WEDGE_MASK 1

#if USE_PRECOMPUTED_WEDGE_MASK
static const uint8_t wedge_master_oblique_odd[MASK_MASTER_SIZE] = {
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  6,  18,
  37, 53, 60, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};
static const uint8_t wedge_master_oblique_even[MASK_MASTER_SIZE] = {
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  4,  11, 27,
  46, 58, 62, 63, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};
static const uint8_t wedge_master_vertical[MASK_MASTER_SIZE] = {
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  7,  21,
  43, 57, 62, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
  64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};


void aom_convolve_copy_c(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst,
    ptrdiff_t dst_stride, const int16_t *filter_x,
    int filter_x_stride, const int16_t *filter_y,
    int filter_y_stride, int w, int h) {
    int r;

    (void)filter_x;
    (void)filter_x_stride;
    (void)filter_y;
    (void)filter_y_stride;

    for (r = h; r > 0; --r) {
        memcpy(dst, src, w);
        src += src_stride;
        dst += dst_stride;
    }
}

static void shift_copy(const uint8_t *src, uint8_t *dst, int shift, int width) {
    if (shift >= 0) {
        memcpy(dst + shift, src, width - shift);
        memset(dst, src[0], shift);
    }
    else {
        shift = -shift;
        memcpy(dst, src + shift, width - shift);
        memset(dst + width - shift, src[width - 1], shift);
    }
}
#endif  // USE_PRECOMPUTED_WEDGE_MASK



// [negative][direction]
DECLARE_ALIGNED(
16, static uint8_t,
wedge_mask_obl[2][WEDGE_DIRECTIONS][MASK_MASTER_SIZE * MASK_MASTER_SIZE]);

// 4 * MAX_WEDGE_SQUARE is an easy to compute and fairly tight upper bound
// on the sum of all mask sizes up to an including MAX_WEDGE_SQUARE.
DECLARE_ALIGNED(16, static uint8_t,
wedge_mask_buf[2 * MAX_WEDGE_TYPES * 4 * MAX_WEDGE_SQUARE]);

static void init_wedge_master_masks() {
    int i, j;
    const int w = MASK_MASTER_SIZE;
    const int h = MASK_MASTER_SIZE;
    const int stride = MASK_MASTER_STRIDE;
    // Note: index [0] stores the masters, and [1] its complement.
#if USE_PRECOMPUTED_WEDGE_MASK
  // Generate prototype by shifting the masters
    int shift = h / 4;
    for (i = 0; i < h; i += 2) {
        shift_copy(wedge_master_oblique_even,
            &wedge_mask_obl[0][WEDGE_OBLIQUE63][i * stride], shift,
            MASK_MASTER_SIZE);
        shift--;
        shift_copy(wedge_master_oblique_odd,
            &wedge_mask_obl[0][WEDGE_OBLIQUE63][(i + 1) * stride], shift,
            MASK_MASTER_SIZE);
        memcpy(&wedge_mask_obl[0][WEDGE_VERTICAL][i * stride],
            wedge_master_vertical,
            MASK_MASTER_SIZE * sizeof(wedge_master_vertical[0]));
        memcpy(&wedge_mask_obl[0][WEDGE_VERTICAL][(i + 1) * stride],
            wedge_master_vertical,
            MASK_MASTER_SIZE * sizeof(wedge_master_vertical[0]));
    }
#else
    static const double smoother_param = 2.85;
    const int a[2] = { 2, 1 };
    const double asqrt = sqrt(a[0] * a[0] + a[1] * a[1]);
    for (i = 0; i < h; i++) {
        for (j = 0; j < w; ++j) {
            int x = (2 * j + 1 - w);
            int y = (2 * i + 1 - h);
            double d = (a[0] * x + a[1] * y) / asqrt;
            const int msk = (int)rint((1.0 + tanh(d / smoother_param)) * 32);
            wedge_mask_obl[0][WEDGE_OBLIQUE63][i * stride + j] = msk;
            const int mskx = (int)rint((1.0 + tanh(x / smoother_param)) * 32);
            wedge_mask_obl[0][WEDGE_VERTICAL][i * stride + j] = mskx;
        }
    }
#endif  // USE_PRECOMPUTED_WEDGE_MASK
    for (i = 0; i < h; ++i) {
        for (j = 0; j < w; ++j) {
            const int msk = wedge_mask_obl[0][WEDGE_OBLIQUE63][i * stride + j];
            wedge_mask_obl[0][WEDGE_OBLIQUE27][j * stride + i] = msk;
            wedge_mask_obl[0][WEDGE_OBLIQUE117][i * stride + w - 1 - j] =
                wedge_mask_obl[0][WEDGE_OBLIQUE153][(w - 1 - j) * stride + i] =
                (1 << WEDGE_WEIGHT_BITS) - msk;
            wedge_mask_obl[1][WEDGE_OBLIQUE63][i * stride + j] =
                wedge_mask_obl[1][WEDGE_OBLIQUE27][j * stride + i] =
                (1 << WEDGE_WEIGHT_BITS) - msk;
            wedge_mask_obl[1][WEDGE_OBLIQUE117][i * stride + w - 1 - j] =
                wedge_mask_obl[1][WEDGE_OBLIQUE153][(w - 1 - j) * stride + i] = msk;
            const int mskx = wedge_mask_obl[0][WEDGE_VERTICAL][i * stride + j];
            wedge_mask_obl[0][WEDGE_HORIZONTAL][j * stride + i] = mskx;
            wedge_mask_obl[1][WEDGE_VERTICAL][i * stride + j] =
                wedge_mask_obl[1][WEDGE_HORIZONTAL][j * stride + i] =
                (1 << WEDGE_WEIGHT_BITS) - mskx;
        }
    }
}

#if !USE_PRECOMPUTED_WEDGE_SIGN
// If the signs for the wedges for various blocksizes are
// inconsistent flip the sign flag. Do it only once for every
// wedge codebook.
static void init_wedge_signs() {
    BLOCK_SIZE sb_type;
    memset(wedge_signflip_lookup, 0, sizeof(wedge_signflip_lookup));
    for (sb_type = BLOCK_4X4; sb_type < BLOCK_SIZES_ALL; ++sb_type) {
        const int bw = block_size_wide[sb_type];
        const int bh = block_size_high[sb_type];
        const wedge_params_type wedge_params = wedge_params_lookup[sb_type];
        const int wbits = wedge_params.bits;
        const int wtypes = 1 << wbits;
        int i, w;
        if (wbits) {
            for (w = 0; w < wtypes; ++w) {
                // Get the mask master, i.e. index [0]
                const uint8_t *mask = get_wedge_mask_inplace(w, 0, sb_type);
                int avg = 0;
                for (i = 0; i < bw; ++i) avg += mask[i];
                for (i = 1; i < bh; ++i) avg += mask[i * MASK_MASTER_STRIDE];
                avg = (avg + (bw + bh - 1) / 2) / (bw + bh - 1);
                // Default sign of this wedge is 1 if the average < 32, 0 otherwise.
                // If default sign is 1:
                //   If sign requested is 0, we need to flip the sign and return
                //   the complement i.e. index [1] instead. If sign requested is 1
                //   we need to flip the sign and return index [0] instead.
                // If default sign is 0:
                //   If sign requested is 0, we need to return index [0] the master
                //   if sign requested is 1, we need to return the complement index [1]
                //   instead.
                wedge_params.signflip[w] = (avg < 32);
            }
        }
    }
}
#endif  // !USE_PRECOMPUTED_WEDGE_SIGN
static INLINE int get_wedge_bits_lookup(BLOCK_SIZE sb_type) {
    return wedge_params_lookup[sb_type].bits;
}
static const uint8_t *get_wedge_mask_inplace(int wedge_index, int neg,
    BLOCK_SIZE sb_type) {
    const uint8_t *master;
    const int bh = block_size_high[sb_type];
    const int bw = block_size_wide[sb_type];
    const wedge_code_type *a =
        wedge_params_lookup[sb_type].codebook + wedge_index;
    int woff, hoff;
    const uint8_t wsignflip = wedge_params_lookup[sb_type].signflip[wedge_index];

    assert(wedge_index >= 0 &&
        wedge_index < (1 << get_wedge_bits_lookup(sb_type)));
    woff = (a->x_offset * bw) >> 3;
    hoff = (a->y_offset * bh) >> 3;
    master = wedge_mask_obl[neg ^ wsignflip][a->direction] +
        MASK_MASTER_STRIDE * (MASK_MASTER_SIZE / 2 - hoff) +
        MASK_MASTER_SIZE / 2 - woff;
    return master;
}
static void init_wedge_masks() {
    uint8_t *dst = wedge_mask_buf;
    BLOCK_SIZE bsize;
    memset(wedge_masks, 0, sizeof(wedge_masks));
    for (bsize = BLOCK_4X4; bsize < BlockSizeS_ALL; ++bsize) {
        const uint8_t *mask;
        const int bw = block_size_wide[bsize];
        const int bh = block_size_high[bsize];
        const wedge_params_type *wedge_params = &wedge_params_lookup[bsize];
        const int wbits = wedge_params->bits;
        const int wtypes = 1 << wbits;
        int w;
        if (wbits == 0) continue;
        for (w = 0; w < wtypes; ++w) {
            mask = get_wedge_mask_inplace(w, 0, bsize);
            aom_convolve_copy_c(mask, MASK_MASTER_STRIDE, dst, bw, NULL, 0, NULL, 0, bw,
                bh);
            wedge_params->masks[0][w] = dst;
            dst += bw * bh;

            mask = get_wedge_mask_inplace(w, 1, bsize);
            aom_convolve_copy_c(mask, MASK_MASTER_STRIDE, dst, bw, NULL, 0, NULL, 0, bw,
                bh);
            wedge_params->masks[1][w] = dst;
            dst += bw * bh;
        }
        assert(sizeof(wedge_mask_buf) >= (size_t)(dst - wedge_mask_buf));
    }
}

// Equation of line: f(x, y) = a[0]*(x - a[2]*w/8) + a[1]*(y - a[3]*h/8) = 0
void av1_init_wedge_masks() {
    init_wedge_master_masks();
#if !USE_PRECOMPUTED_WEDGE_SIGN
    init_wedge_signs();
#endif  // !USE_PRECOMPUTED_WEDGE_SIGN
    init_wedge_masks();
}
static void diffwtd_mask_d16(uint8_t *mask, int which_inverse, int mask_base,
    const CONV_BUF_TYPE *src0, int src0_stride,
    const CONV_BUF_TYPE *src1, int src1_stride, int h,
    int w, ConvolveParams *conv_params, int bd) {
    int round =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1 + (bd - 8);
    int i, j, m, diff;
    for (i = 0; i < h; ++i) {
        for (j = 0; j < w; ++j) {
            diff = abs(src0[i * src0_stride + j] - src1[i * src1_stride + j]);
            diff = ROUND_POWER_OF_TWO(diff, round);
            m = clamp(mask_base + (diff / DIFF_FACTOR), 0, AOM_BLEND_A64_MAX_ALPHA);
            mask[i * w + j] = which_inverse ? AOM_BLEND_A64_MAX_ALPHA - m : m;
        }
    }
}

void av1_build_compound_diffwtd_mask_d16_c(
    uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const CONV_BUF_TYPE *src0,
    int src0_stride, const CONV_BUF_TYPE *src1, int src1_stride, int h, int w,
    ConvolveParams *conv_params, int bd) {
    switch (mask_type) {
    case DIFFWTD_38:
        diffwtd_mask_d16(mask, 0, 38, src0, src0_stride, src1, src1_stride, h, w,
            conv_params, bd);
        break;
    case DIFFWTD_38_INV:
        diffwtd_mask_d16(mask, 1, 38, src0, src0_stride, src1, src1_stride, h, w,
            conv_params, bd);
        break;
    default: assert(0);
    }
}
// Blending with alpha mask. Mask values come from the range [0, 64],
// as described for AOM_BLEND_A64 in aom_dsp/blend.h. src0 or src1 can
// be the same as dst, or dst can be different from both sources.

// NOTE(david.barker): The input and output of aom_blend_a64_d16_mask_c() are
// in a higher intermediate precision, and will later be rounded down to pixel
// precision.
// Thus, in order to avoid double-rounding, we want to use normal right shifts
// within this function, not ROUND_POWER_OF_TWO.
// This works because of the identity:
// ROUND_POWER_OF_TWO(x >> y, z) == ROUND_POWER_OF_TWO(x, y+z)
//
// In contrast, the output of the non-d16 functions will not be further rounded,
// so we *should* use ROUND_POWER_OF_TWO there.

void aom_lowbd_blend_a64_d16_mask_c(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int w, int h, int subw, int subh,
    ConvolveParams *conv_params) {
    int i, j;
    const int bd = 8;
    const int offset_bits = bd + 2 * FILTER_BITS - conv_params->round_0;
    const int round_offset = (1 << (offset_bits - conv_params->round_1)) +
        (1 << (offset_bits - conv_params->round_1 - 1));
    const int round_bits =
        2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;

    assert(IMPLIES((void *)src0 == dst, src0_stride == dst_stride));
    assert(IMPLIES((void *)src1 == dst, src1_stride == dst_stride));

    assert(h >= 4);
    assert(w >= 4);
    //assert(IS_POWER_OF_TWO(h));
    //assert(IS_POWER_OF_TWO(w));

    if (subw == 0 && subh == 0) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                int32_t res;
                const int m = mask[i * mask_stride + j];
                res = ((m * (int32_t)src0[i * src0_stride + j] +
                    (AOM_BLEND_A64_MAX_ALPHA - m) *
                    (int32_t)src1[i * src1_stride + j]) >>
                    AOM_BLEND_A64_ROUND_BITS);
                res -= round_offset;
                dst[i * dst_stride + j] =
                    clip_pixel(ROUND_POWER_OF_TWO(res, round_bits));
            }
        }
    }
    else if (subw == 1 && subh == 1) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                int32_t res;
                const int m = ROUND_POWER_OF_TWO(
                    mask[(2 * i) * mask_stride + (2 * j)] +
                    mask[(2 * i + 1) * mask_stride + (2 * j)] +
                    mask[(2 * i) * mask_stride + (2 * j + 1)] +
                    mask[(2 * i + 1) * mask_stride + (2 * j + 1)],
                    2);
                res = ((m * (int32_t)src0[i * src0_stride + j] +
                    (AOM_BLEND_A64_MAX_ALPHA - m) *
                    (int32_t)src1[i * src1_stride + j]) >>
                    AOM_BLEND_A64_ROUND_BITS);
                res -= round_offset;
                dst[i * dst_stride + j] =
                    clip_pixel(ROUND_POWER_OF_TWO(res, round_bits));
            }
        }
    }
    else if (subw == 1 && subh == 0) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                int32_t res;
                const int m = AOM_BLEND_AVG(mask[i * mask_stride + (2 * j)],
                    mask[i * mask_stride + (2 * j + 1)]);
                res = ((m * (int32_t)src0[i * src0_stride + j] +
                    (AOM_BLEND_A64_MAX_ALPHA - m) *
                    (int32_t)src1[i * src1_stride + j]) >>
                    AOM_BLEND_A64_ROUND_BITS);
                res -= round_offset;
                dst[i * dst_stride + j] =
                    clip_pixel(ROUND_POWER_OF_TWO(res, round_bits));
            }
        }
    }
    else {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                int32_t res;
                const int m = AOM_BLEND_AVG(mask[(2 * i) * mask_stride + j],
                    mask[(2 * i + 1) * mask_stride + j]);
                res = ((int32_t)(m * (int32_t)src0[i * src0_stride + j] +
                    (AOM_BLEND_A64_MAX_ALPHA - m) *
                    (int32_t)src1[i * src1_stride + j]) >>
                    AOM_BLEND_A64_ROUND_BITS);
                res -= round_offset;
                dst[i * dst_stride + j] =
                    clip_pixel(ROUND_POWER_OF_TWO(res, round_bits));
            }
        }
    }
}
int is_masked_compound_type(COMPOUND_TYPE type);
#if II_COMP
/* clang-format off */
static const uint8_t ii_weights1d[MAX_SB_SIZE] = {
  60, 58, 56, 54, 52, 50, 48, 47, 45, 44, 42, 41, 39, 38, 37, 35, 34, 33, 32,
  31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 22, 21, 20, 19, 19, 18, 18, 17, 16,
  16, 15, 15, 14, 14, 13, 13, 12, 12, 12, 11, 11, 10, 10, 10,  9,  9,  9,  8,
  8,  8,  8,  7,  7,  7,  7,  6,  6,  6,  6,  6,  5,  5,  5,  5,  5,  4,  4,
  4,  4,  4,  4,  4,  4,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  2,  2,
  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  1,  1,
  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1
};
static uint8_t ii_size_scales[BlockSizeS_ALL] = {
    32, 16, 16, 16, 8, 8, 8, 4,
    4,  4,  2,  2,  2, 1, 1, 1,
    8,  8,  4,  4,  2, 2
};
/* clang-format on */

static void build_smooth_interintra_mask(uint8_t *mask, int stride,
                                         BLOCK_SIZE plane_bsize,
                                         INTERINTRA_MODE mode) {
  int i, j;
  const int bw = block_size_wide[plane_bsize];
  const int bh = block_size_high[plane_bsize];
  const int size_scale = ii_size_scales[plane_bsize];

  switch (mode) {
    case II_V_PRED:
      for (i = 0; i < bh; ++i) {
        memset(mask, ii_weights1d[i * size_scale], bw * sizeof(mask[0]));
        mask += stride;
      }
      break;

    case II_H_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j) mask[j] = ii_weights1d[j * size_scale];
        mask += stride;
      }
      break;

    case II_SMOOTH_PRED:
      for (i = 0; i < bh; ++i) {
        for (j = 0; j < bw; ++j)
          mask[j] = ii_weights1d[(i < j ? i : j) * size_scale];
        mask += stride;
      }
      break;

    case II_DC_PRED:
    default:
      for (i = 0; i < bh; ++i) {
        memset(mask, 32, bw * sizeof(mask[0]));
        mask += stride;
      }
      break;
  }
}
#endif
static INLINE const uint8_t *av1_get_contiguous_soft_mask(int wedge_index,
    int wedge_sign,
    BLOCK_SIZE sb_type) {
    return wedge_params_lookup[sb_type].masks[wedge_sign][wedge_index];
}
const uint8_t *av1_get_compound_type_mask(
    const INTERINTER_COMPOUND_DATA *const comp_data, BLOCK_SIZE sb_type) {
    assert(is_masked_compound_type(comp_data->type));
    (void)sb_type;
    switch (comp_data->type) {
    case COMPOUND_WEDGE:
        return av1_get_contiguous_soft_mask(comp_data->wedge_index,
            comp_data->wedge_sign, sb_type);
    case COMPOUND_DIFFWTD: return comp_data->seg_mask;
    default: assert(0); return NULL;
    }
}
static void build_masked_compound_no_round(
    uint8_t *dst, int dst_stride, const CONV_BUF_TYPE *src0, int src0_stride,
    const CONV_BUF_TYPE *src1, int src1_stride,
    const INTERINTER_COMPOUND_DATA *const comp_data, BLOCK_SIZE sb_type, int h,
    int w, ConvolveParams *conv_params) {
    // Derive subsampling from h and w passed in. May be refactored to
    // pass in subsampling factors directly.
    const int subh = (2 << mi_size_high_log2[sb_type]) == h;
    const int subw = (2 << mi_size_wide_log2[sb_type]) == w;
    const uint8_t *mask = av1_get_compound_type_mask(comp_data, sb_type);
/*    if (is_cur_buf_hbd(xd)) {
        aom_highbd_blend_a64_d16_mask(dst, dst_stride, src0, src0_stride, src1,
            src1_stride, mask, block_size_wide[sb_type],
            w, h, subw, subh, conv_params, xd->bd);
    }
    else */{
#if COMP_AVX
        aom_lowbd_blend_a64_d16_mask(dst, dst_stride, src0, src0_stride, src1,
            src1_stride, mask, block_size_wide[sb_type], w,
            h, subw, subh, conv_params);
#else
        aom_lowbd_blend_a64_d16_mask_c(dst, dst_stride, src0, src0_stride, src1,
            src1_stride, mask, block_size_wide[sb_type], w,
            h, subw, subh, conv_params);
#endif
    }
}
void av1_make_masked_inter_predictor(
    uint8_t                   *src_ptr,
    uint32_t                   src_stride,
    uint8_t                   *dst_ptr,
    uint32_t                   dst_stride,
    const BlockGeom           *blk_geom,
    uint8_t                    bwidth,
    uint8_t                    bheight,
    InterpFilterParams        *filter_params_x,
    InterpFilterParams        *filter_params_y,
    int32_t                    subpel_x,
    int32_t                    subpel_y,
    ConvolveParams            *conv_params,
    INTERINTER_COMPOUND_DATA  *comp_data,
    uint8_t                    bitdepth,
    uint8_t                    plane
    )
{

//We come here when we have a prediction done using regular path for the ref0 stored in conv_param.dst.
//use regular path to generate a prediction for ref1 into  a temporary buffer,
//then  blend that temporary buffer with that from  the first reference.

    DECLARE_ALIGNED(16, uint8_t, seg_mask[2 * MAX_SB_SQUARE]);
    comp_data->seg_mask = seg_mask;


#define INTER_PRED_BYTES_PER_PIXEL 2
    DECLARE_ALIGNED(32, uint8_t,
    tmp_buf[INTER_PRED_BYTES_PER_PIXEL * MAX_SB_SQUARE]);
#undef INTER_PRED_BYTES_PER_PIXEL
    //uint8_t *tmp_dst =  tmp_buf;
    const int tmp_buf_stride = MAX_SB_SIZE;

    CONV_BUF_TYPE *org_dst = conv_params->dst;//save the ref0 prediction pointer
    int org_dst_stride = conv_params->dst_stride;
    CONV_BUF_TYPE *tmp_buf16 = (CONV_BUF_TYPE *)tmp_buf;
    conv_params->dst = tmp_buf16;
    conv_params->dst_stride = tmp_buf_stride;
    assert(conv_params->do_average == 0);

    convolve[subpel_x != 0][subpel_y != 0][1/* ????  is_compound*/](
        src_ptr,
        src_stride,
        dst_ptr,
        dst_stride,
        bwidth,
        bheight,
        filter_params_x,
        filter_params_y,
        subpel_x,
        subpel_y,
        conv_params);

    if (!plane && comp_data->type == COMPOUND_DIFFWTD) {
        //CHKN  for DIFF: need to compute the mask  comp_data->seg_mask is the output computed from the two preds org_dst and tmp_buf16
        //for WEDGE the mask is fixed from the table based on wedge_sign/index
#if COMP_AVX
        av1_build_compound_diffwtd_mask_d16(
            comp_data->seg_mask, comp_data->mask_type, org_dst, org_dst_stride,
            tmp_buf16, tmp_buf_stride, bheight, bwidth, conv_params, bitdepth);

#else
        av1_build_compound_diffwtd_mask_d16_c(
            comp_data->seg_mask, comp_data->mask_type, org_dst, org_dst_stride,
            tmp_buf16, tmp_buf_stride, bheight, bwidth, conv_params, bitdepth);
#endif
    }

    build_masked_compound_no_round(dst_ptr, dst_stride, org_dst, org_dst_stride,
        tmp_buf16, tmp_buf_stride, comp_data,
        blk_geom->bsize, bheight, bwidth, conv_params);


}

void aom_subtract_block_c(int rows, int cols, int16_t *diff,
    ptrdiff_t diff_stride, const uint8_t *src,
    ptrdiff_t src_stride, const uint8_t *pred,
    ptrdiff_t pred_stride) {
    int r, c;

    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) diff[c] = src[c] - pred[c];

        diff += diff_stride;
        pred += pred_stride;
        src += src_stride;
    }
}
static void diffwtd_mask(uint8_t *mask, int which_inverse, int mask_base,
    const uint8_t *src0, int src0_stride,
    const uint8_t *src1, int src1_stride, int h, int w) {
    int i, j, m, diff;
    for (i = 0; i < h; ++i) {
        for (j = 0; j < w; ++j) {
            diff =
                abs((int)src0[i * src0_stride + j] - (int)src1[i * src1_stride + j]);
            m = clamp(mask_base + (diff / DIFF_FACTOR), 0, AOM_BLEND_A64_MAX_ALPHA);
            mask[i * w + j] = which_inverse ? AOM_BLEND_A64_MAX_ALPHA - m : m;
        }
    }
}
void av1_build_compound_diffwtd_mask_c(uint8_t *mask,
    DIFFWTD_MASK_TYPE mask_type,
    const uint8_t *src0, int src0_stride,
    const uint8_t *src1, int src1_stride,
    int h, int w) {
    switch (mask_type) {
    case DIFFWTD_38:
        diffwtd_mask(mask, 0, 38, src0, src0_stride, src1, src1_stride, h, w);
        break;
    case DIFFWTD_38_INV:
        diffwtd_mask(mask, 1, 38, src0, src0_stride, src1, src1_stride, h, w);
        break;
    default: assert(0);
    }
}
#define MAX_MASK_VALUE (1 << WEDGE_WEIGHT_BITS)

/**
 * Computes SSE of a compound predictor constructed from 2 fundamental
 * predictors p0 and p1 using blending with mask.
 *
 * r1:  Residuals of p1.
 *      (source - p1)
 * d:   Difference of p1 and p0.
 *      (p1 - p0)
 * m:   The blending mask
 * N:   Number of pixels
 *
 * 'r1', 'd', and 'm' are contiguous.
 *
 * Computes:
 *  Sum((MAX_MASK_VALUE*r1 + mask*d)**2), which is equivalent to:
 *  Sum((mask*r0 + (MAX_MASK_VALUE-mask)*r1)**2),
 *    where r0 is (source - p0), and r1 is (source - p1), which is in turn
 *    is equivalent to:
 *  Sum((source*MAX_MASK_VALUE - (mask*p0 + (MAX_MASK_VALUE-mask)*p1))**2),
 *    which is the SSE of the residuals of the compound predictor scaled up by
 *    MAX_MASK_VALUE**2.
 *
 * Note that we clamp the partial term in the loop to 16 bits signed. This is
 * to facilitate equivalent SIMD implementation. It should have no effect if
 * residuals are within 16 - WEDGE_WEIGHT_BITS (=10) signed, which always
 * holds for 8 bit input, and on real input, it should hold practically always,
 * as residuals are expected to be small.
 */
uint64_t av1_wedge_sse_from_residuals_c(const int16_t *r1, const int16_t *d,
    const uint8_t *m, int N) {
    uint64_t csse = 0;
    int i;

    for (i = 0; i < N; i++) {
        int32_t t = MAX_MASK_VALUE * r1[i] + m[i] * d[i];
        t = clamp(t, INT16_MIN, INT16_MAX);
        csse += t * t;
    }
    return ROUND_POWER_OF_TWO(csse, 2 * WEDGE_WEIGHT_BITS);
}
static const uint8_t bsize_curvfit_model_cat_lookup[BlockSizeS_ALL] = {
  0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 3, 3, 3, 0, 0, 1, 1, 2, 2
};
static int sse_norm_curvfit_model_cat_lookup(double sse_norm) {
    return (sse_norm > 16.0);
}
static const double interp_rgrid_curv[4][65] = {
  {
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    23.801499,   28.387688,   33.388795,   42.298282,
      41.525408,   51.597692,   49.566271,   54.632979,   60.321507,
      67.730678,   75.766165,   85.324032,   96.600012,   120.839562,
      173.917577,  255.974908,  354.107573,  458.063476,  562.345966,
      668.568424,  772.072881,  878.598490,  982.202274,  1082.708946,
      1188.037853, 1287.702240, 1395.588773, 1490.825830, 1584.231230,
      1691.386090, 1766.822555, 1869.630904, 1926.743565, 2002.949495,
      2047.431137, 2138.486068, 2154.743767, 2209.242472, 2277.593051,
      2290.996432, 2307.452938, 2343.567091, 2397.654644, 2469.425868,
      2558.591037, 2664.860422, 2787.944296, 2927.552932, 3083.396602,
      3255.185579, 3442.630134, 3645.440541, 3863.327072, 4096.000000,
  },
  {
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    8.998436,    9.439592,    9.731837,    10.865931,
      11.561347,   12.578139,   14.205101,   16.770584,   19.094853,
      21.330863,   23.298907,   26.901921,   34.501017,   57.891733,
      112.234763,  194.853189,  288.302032,  380.499422,  472.625309,
      560.226809,  647.928463,  734.155122,  817.489721,  906.265783,
      999.260562,  1094.489206, 1197.062998, 1293.296825, 1378.926484,
      1472.760990, 1552.663779, 1635.196884, 1692.451951, 1759.741063,
      1822.162720, 1916.515921, 1966.686071, 2031.647506, 2033.700134,
      2087.847688, 2161.688858, 2242.536028, 2334.023491, 2436.337802,
      2549.665519, 2674.193198, 2810.107395, 2957.594666, 3116.841567,
      3288.034655, 3471.360486, 3667.005616, 3875.156602, 4096.000000,
  },
  {
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    2.377584,    2.557185,    2.732445,    2.851114,
      3.281800,    3.765589,    4.342578,    5.145582,    5.611038,
      6.642238,    7.945977,    11.800522,   17.346624,   37.501413,
      87.216800,   165.860942,  253.865564,  332.039345,  408.518863,
      478.120452,  547.268590,  616.067676,  680.022540,  753.863541,
      834.529973,  919.489191,  1008.264989, 1092.230318, 1173.971886,
      1249.514122, 1330.510941, 1399.523249, 1466.923387, 1530.533471,
      1586.515722, 1695.197774, 1746.648696, 1837.136959, 1909.075485,
      1975.074651, 2060.159200, 2155.335095, 2259.762505, 2373.710437,
      2497.447898, 2631.243895, 2775.367434, 2930.087523, 3095.673170,
      3272.393380, 3460.517161, 3660.313520, 3872.051464, 4096.000000,
  },
  {
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    0.000000,    0.000000,    0.000000,    0.000000,
      0.000000,    0.296997,    0.342545,    0.403097,    0.472889,
      0.614483,    0.842937,    1.050824,    1.326663,    1.717750,
      2.530591,    3.582302,    6.995373,    9.973335,    24.042464,
      56.598240,   113.680735,  180.018689,  231.050567,  266.101082,
      294.957934,  323.326511,  349.434429,  380.443211,  408.171987,
      441.214916,  475.716772,  512.900000,  551.186939,  592.364455,
      624.527378,  661.940693,  679.185473,  724.800679,  764.781792,
      873.050019,  950.299001,  939.292954,  1052.406153, 1033.893184,
      1112.182406, 1219.174326, 1337.296681, 1471.648357, 1622.492809,
      1790.093491, 1974.713858, 2176.617364, 2396.067465, 2633.327614,
      2888.661266, 3162.331876, 3454.602899, 3765.737789, 4096.000000,
  },
};

static const double interp_dgrid_curv[2][65] = {
  {
      16.000000, 15.962891, 15.925174, 15.886888, 15.848074, 15.808770,
      15.769015, 15.728850, 15.688313, 15.647445, 15.606284, 15.564870,
      15.525918, 15.483820, 15.373330, 15.126844, 14.637442, 14.184387,
      13.560070, 12.880717, 12.165995, 11.378144, 10.438769, 9.130790,
      7.487633,  5.688649,  4.267515,  3.196300,  2.434201,  1.834064,
      1.369920,  1.035921,  0.775279,  0.574895,  0.427232,  0.314123,
      0.233236,  0.171440,  0.128188,  0.092762,  0.067569,  0.049324,
      0.036330,  0.027008,  0.019853,  0.015539,  0.011093,  0.008733,
      0.007624,  0.008105,  0.005427,  0.004065,  0.003427,  0.002848,
      0.002328,  0.001865,  0.001457,  0.001103,  0.000801,  0.000550,
      0.000348,  0.000193,  0.000085,  0.000021,  0.000000,
  },
  {
      16.000000, 15.996116, 15.984769, 15.966413, 15.941505, 15.910501,
      15.873856, 15.832026, 15.785466, 15.734633, 15.679981, 15.621967,
      15.560961, 15.460157, 15.288367, 15.052462, 14.466922, 13.921212,
      13.073692, 12.222005, 11.237799, 9.985848,  8.898823,  7.423519,
      5.995325,  4.773152,  3.744032,  2.938217,  2.294526,  1.762412,
      1.327145,  1.020728,  0.765535,  0.570548,  0.425833,  0.313825,
      0.232959,  0.171324,  0.128174,  0.092750,  0.067558,  0.049319,
      0.036330,  0.027008,  0.019853,  0.015539,  0.011093,  0.008733,
      0.007624,  0.008105,  0.005427,  0.004065,  0.003427,  0.002848,
      0.002328,  0.001865,  0.001457,  0.001103,  0.000801,  0.000550,
      0.000348,  0.000193,  0.000085,  0.000021,  -0.000000,
  },
};

/*
  Precalucation factors to interp_cubic()
    interp_cubic() OUT is: p[1] + 0.5 * x * (p[2] - p[0] +
                      x * (2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3] +
                      x * (3.0 * (p[1] - p[2]) + p[3] - p[0])));
  Precalucation:
    interp_cubic() OUT is: D + x * (C + x * (B + x * A))
    For precalculated factors:
    double A = 0.5 *(3.0 * (p[1] - p[2]) + p[3] - p[0]);
    double B = 0.5 *(2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3]);
    double C = 0.5 * (p[2] - p[0]);
    double D = p[1];

    Precalculated values of array factors:
    A is: (0 to sizeof(ARRAY[])-1)
    B is: (0 to sizeof(ARRAY[A][])-4)
    PRECALC[A][B][0] = 0.5 *(3.0 * (ARRAY[A][B+1] - ARRAY[A][B+2]) + ARRAY[A][B+3] - ARRAY[A][B])
    PRECALC[A][B][1] = 0.5 *(2.0 * p[0] - 5.0 * ARRAY[A][B+1] + 4.0 * ARRAY[A][B+2]) - ARRAY[A][B+3]);
    PRECALC[A][B][2] = 0.5 * (ARRAY[A][B+2] - ARRAY[A][B]);
    PRECALC[A][B][3] = ARRAY[A][B+1]
*/
static const double interp_rgrid_curv_precalc[4][62][4] = {
  {
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {11.900749500000, -11.900749500000, 0.000000000000, 0.000000000000},
    {-21.508404500000, 33.409154000000, 11.900749500000, 0.000000000000},
    {9.815114000000, -19.422769000000, 14.193844000000, 23.801499000000},
    {1.746731000000, -1.539272000000, 4.793648000000, 28.387688000000},
    {-6.795370500000, 8.749560500000, 6.955297000000, 33.388795000000},
    {10.263759500000, -15.104940000000, 4.068306500000, 42.298282000000},
    {-11.474431500000, 16.897010500000, 4.649705000000, 41.525408000000},
    {9.600917000000, -15.652769500000, 4.020431500000, 51.597692000000},
    {-3.238154500000, 6.787219000000, 1.517643500000, 49.566271000000},
    {0.549411500000, -0.238501500000, 5.377618000000, 54.632979000000},
    {-0.547163500000, 1.407485000000, 6.548849500000, 60.321507000000},
    {0.448032000000, -0.134874000000, 7.722329000000, 67.730678000000},
    {0.097866500000, 0.663323500000, 8.796677000000, 75.766165000000},
    {5.622728500000, -4.763672000000, 10.416923500000, 85.324032000000},
    {7.937447500000, -1.455662500000, 17.757765000000, 96.600012000000},
    {0.070425500000, 14.348807000000, 38.658782500000, 120.839562000000},
    {-6.451991000000, 20.941649000000, 67.567673000000, 173.917577000000},
    {-5.126048000000, 13.163715000000, 90.094998000000, 255.974908000000},
    {-2.748325500000, 5.659944500000, 101.044284000000, 354.107573000000},
    {0.806690500000, -0.643397000000, 104.119196500000, 458.063476000000},
    {-2.328984500000, 3.298968500000, 105.252474000000, 562.345966000000},
    {2.869576500000, -4.228577000000, 104.863457500000, 668.568424000000},
    {-2.971488500000, 4.482064500000, 105.015033000000, 772.072881000000},
    {-0.087643500000, -1.373269000000, 105.064696500000, 878.598490000000},
    {3.959673500000, -5.508229500000, 102.055228000000, 982.202274000000},
    {-5.243377500000, 7.654495000000, 102.917789500000, 1082.708946000000},
    {6.943333000000, -9.775593000000, 102.496647000000, 1188.037853000000},
    {-10.435811000000, 14.546884000000, 103.775460000000, 1287.702240000000},
    {5.408909500000, -11.733647500000, 101.561795000000, 1395.588773000000},
    {7.790558500000, -8.706387000000, 94.321228500000, 1490.825830000000},
    {-22.733927500000, 29.608657499999, 100.280130000000, 1584.231230000000},
    {29.545139500000, -45.404337000000, 91.295662500000, 1691.386090000000},
    {-36.533786000000, 50.219728000000, 89.122407000000, 1766.822555000000},
    {32.394478500000, -55.242322500000, 79.960505000000, 1869.630904000000},
    {-25.408778500000, 34.955413000000, 66.659295500000, 1926.743565000000},
    {39.148788500000, -55.010932500000, 60.343786000000, 2002.949495000000},
    {-60.685260500000, 83.971905000000, 67.768286500000, 2047.431137000000},
    {56.519119000000, -93.917735000000, 53.656315000000, 2138.486068000000},
    {-12.194566000000, 31.315069000000, 35.378202000000, 2154.743767000000},
    {-34.399536000000, 41.325473000000, 61.424642000000, 2209.242472000000},
    {29.000161500000, -56.473760500000, 40.876980000000, 2277.593051000000},
    {8.302261000000, -6.775698500000, 14.929943500000, 2290.996432000000},
    {-0.842123500000, 10.670947000000, 26.285329500000, 2307.452938000000},
    {-0.144864500000, 9.131564500000, 45.100853000000, 2343.567091000000},
    {-0.144862999999, 8.986698499999, 62.929388500000, 2397.654644000000},
    {-0.144864500000, 8.841837000001, 80.468196500000, 2469.425868000000},
    {-0.144863500000, 8.696971500000, 97.717277000000, 2558.591037000000},
    {-0.144863500000, 8.552108000000, 114.676629500000, 2664.860422000000},
    {-0.144864000000, 8.407245000000, 131.346255000000, 2787.944296000000},
    {-0.144863500000, 8.262380499999, 147.726153000000, 2927.552932000000},
    {-0.144864500000, 8.117518000000, 163.816323500000, 3083.396602000000},
    {-0.144863000000, 7.972651999999, 179.616766000000, 3255.185579000000},
    {-0.144864000000, 7.827790000000, 195.127481000000, 3442.630134000000},
    {-0.144863500000, 7.682925500000, 210.348469000000, 3645.440541000000},
  },
  {
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {4.499218000000, -4.499218000000, 0.000000000000, 0.000000000000},
    {-8.777858000000, 13.277076000000, 4.499218000000, 0.000000000000},
    {4.204184500000, -8.482824500000, 4.719796000000, 8.998436000000},
    {0.495380000000, -0.569835500000, 0.366700500000, 9.439592000000},
    {-0.640263500000, 1.061188000000, 0.713169500000, 9.731837000000},
    {0.380027000000, -0.599366000000, 0.914755000000, 10.865931000000},
    {0.144397000000, 0.016291000000, 0.856104000000, 11.561347000000},
    {0.164175500000, 0.140909500000, 1.321877000000, 12.578139000000},
    {-0.589867500000, 1.059128000000, 2.096222500000, 14.205101000000},
    {0.076477500000, -0.197084500000, 2.444876000000, 16.770584000000},
    {-0.089853500000, 0.045724000000, 2.280139500000, 19.094853000000},
    {0.951468000000, -1.085451000000, 2.102027000000, 21.330863000000},
    {1.180556000000, -0.363071000000, 2.785529000000, 23.298907000000},
    {5.897769000000, -3.899728000000, 5.601055000000, 26.901921000000},
    {7.580347000000, 0.315463000000, 15.494906000000, 34.501017000000},
    {-1.338459000000, 16.814616000000, 38.866873000000, 57.891733000000},
    {-8.722489500000, 22.860187500000, 68.480728000000, 112.234763000000},
    {-6.040935000000, 11.456143500000, 88.033634500000, 194.853189000000},
    {0.589975000000, -1.215701500000, 92.823116500000, 288.302032000000},
    {-2.226442000000, 2.190690500000, 92.161638500000, 380.499422000000},
    {2.312270500000, -4.574464000000, 89.863693500000, 472.625309000000},
    {-0.787574500000, 0.837651500000, 87.651577000000, 560.226809000000},
    {-0.708532500000, -0.028965000000, 86.964156500000, 647.928463000000},
    {4.166761500000, -5.612791500000, 84.780629000000, 734.155122000000},
    {-0.611373000000, 3.332104500000, 86.055330500000, 817.489721000000},
    {-0.992426000000, 3.101784500000, 90.885420500000, 906.265783000000},
    {2.555641500000, -1.438709000000, 94.111711500000, 999.260562000000},
    {-6.842556500000, 10.515130500000, 98.901218000000, 1094.489206000000},
    {-2.132101500000, -1.037881000001, 99.403809500000, 1197.062998000000},
    {9.404507500000, -14.706591499999, 90.931743000000, 1293.296825000000},
    {-11.068282000000, 15.170705500000, 89.732082500000, 1378.926484000000},
    {8.281016500000, -15.246875000000, 86.868647500000, 1472.760990000000},
    {-13.954177000000, 15.269335000000, 81.217947000000, 1552.663779000000},
    {17.656041500000, -30.295060500000, 69.894086000000, 1635.196884000000},
    {-7.450750000000, 12.467772500000, 62.272089500000, 1692.451951000000},
    {18.399499500000, -20.833227000000, 64.855384500000, 1759.741063000000},
    {-38.057297500000, 54.023069500000, 78.387429000000, 1822.162720000000},
    {29.487168000000, -51.578693499999, 72.261675500000, 1916.515921000000},
    {-38.850046000000, 46.245688500000, 57.565792500000, 1966.686071000000},
    {57.501866500000, -88.956270000000, 33.507031500000, 2031.647506000000},
    {-16.200655000000, 42.248118000000, 28.100091000000, 2033.700134000000},
    {-6.343808000000, 16.190616000000, 63.994362000000, 2087.847688000000},
    {1.817146500000, 1.685853500000, 77.344170000000, 2161.688858000000},
    {0.093277500000, 5.226869000000, 86.167316500000, 2242.536028000000},
    {0.093279000000, 5.320145000000, 96.900887000000, 2334.023491000000},
    {0.093278000000, 5.413425000000, 107.821014000000, 2436.337802000000},
    {0.093278000000, 5.506703000000, 118.927698000000, 2549.665519000000},
    {0.093278000000, 5.599981000000, 130.220938000000, 2674.193198000000},
    {0.093278000000, 5.693259000000, 141.700734000000, 2810.107395000000},
    {0.093278500000, 5.786536500000, 153.367086000000, 2957.594666000000},
    {0.093278000000, 5.879815500000, 165.219994500000, 3116.841567000000},
    {0.093278000000, 5.973093499999, 177.259459500000, 3288.034655000000},
    {0.093278500000, 6.066370999999, 189.485480500000, 3471.360486000000},
    {0.093278000000, 6.159650000000, 201.898058000000, 3667.005616000000},
  },
  {
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {1.188792000000, -1.188792000000, 0.000000000000, 0.000000000000},
    {-2.287783500000, 3.476575500000, 1.188792000000, 0.000000000000},
    {1.096821000000, -2.195812500000, 1.278592500000, 2.377584000000},
    {-0.026125000000, 0.023954500000, 0.177430500000, 2.557185000000},
    {0.184304000000, -0.212599500000, 0.146964500000, 2.732445000000},
    {-0.129457000000, 0.285465500000, 0.274677500000, 2.851114000000},
    {0.020048500000, 0.006503000000, 0.457237500000, 3.281800000000},
    {0.066407500000, -0.019807500000, 0.530389000000, 3.765589000000},
    {-0.281781500000, 0.394789000000, 0.689996500000, 4.342578000000},
    {0.451646000000, -0.620420000000, 0.634230000000, 5.145582000000},
    {-0.146602500000, 0.429474500000, 0.748328000000, 5.611038000000},
    {1.139133500000, -1.002864000000, 1.167469500000, 6.642238000000},
    {-0.429624500000, 1.705027500000, 2.579142000000, 7.945977000000},
    {6.458565000000, -5.612786500000, 4.700323500000, 11.800522000000},
    {7.475955500000, -0.171612000000, 12.850445500000, 17.346624000000},
    {-0.315921500000, 15.096220500000, 34.935088000000, 37.501413000000},
    {-9.784137500000, 24.248515000000, 64.179764500000, 87.216800000000},
    {-9.595660500000, 14.275900500000, 83.324382000000, 165.860942000000},
    {4.068289000000, -8.983709500000, 83.089201500000, 253.865564000000},
    {-2.591833000000, 1.744701500000, 77.326649500000, 332.039345000000},
    {3.212239000000, -6.651203500000, 73.040553500000, 408.518863000000},
    {0.052199500000, -0.278925000000, 69.374863500000, 478.120452000000},
    {-2.247585000000, 2.073059000000, 68.973612000000, 547.268590000000},
    {7.365179500000, -9.787290500000, 66.376975000000, 616.067676000000},
    {-1.530353000000, 6.473421500000, 68.897932500000, 680.022540000000},
    {-1.266322500000, 4.679038000000, 77.253716500000, 753.863541000000},
    {-0.238103000000, 2.384496000000, 82.812825000000, 834.529973000000},
    {-4.313524500000, 6.221814500000, 86.867508000000, 919.489191000000},
    {1.293354000000, -3.698588500000, 86.370563500000, 1008.264989000000},
    {-1.987785500000, 0.875905000000, 82.853448500000, 1092.230318000000},
    {5.826957500000, -8.926623500000, 78.641902000000, 1173.971886000000},
    {-8.719547000000, 11.446838500000, 78.269527500000, 1249.514122000000},
    {5.186170500000, -11.178426000000, 75.004563500000, 1330.510941000000},
    {-1.088942000000, 0.282857000000, 68.206223000000, 1399.523249000000},
    {-1.918889500000, 0.023862500000, 65.505111000000, 1466.923387000000},
    {30.163817000000, -33.977733500000, 59.796167500000, 1530.533471000000},
    {-54.965465500000, 81.315365999999, 82.332151500000, 1586.515722000000},
    {48.134235500000, -76.749800500000, 80.066487000000, 1695.197774000000},
    {-28.793539000000, 48.312209500000, 70.969592500000, 1746.648696000000},
    {6.305188500000, -15.580056999999, 81.213394500000, 1837.136959000000},
    {12.512371500000, -15.482051500001, 68.968846000000, 1909.075485000000},
    {-4.497018500000, 14.039710000001, 75.541857500000, 1975.074651000000},
    {-0.419915500000, 5.465588500000, 90.130222000000, 2060.159200000000},
    {0.134503500000, 4.491254000000, 99.801652500000, 2155.335095000000},
    {0.134503500000, 4.625757500000, 109.187671000000, 2259.762505000000},
    {0.134503500000, 4.760261000000, 118.842696500000, 2373.710437000000},
    {0.134503000000, 4.894765000000, 128.766729000000, 2497.447898000000},
    {0.134504000001, 5.029266999999, 138.959768000000, 2631.243895000000},
    {0.134503999999, 5.163771000000, 149.421814000000, 2775.367434000000},
    {0.134502500000, 5.298276499999, 160.152868000000, 2930.087523000000},
    {0.134504000000, 5.432777500000, 171.152928500000, 3095.673170000000},
    {0.134503500000, 5.567282000001, 182.421995500000, 3272.393380000000},
    {0.134503500000, 5.701785499999, 193.960070000000, 3460.517161000000},
    {0.134503500000, 5.836288999999, 205.767151500000, 3660.313520000000},
  },
  {
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.000000000000, 0.000000000000, 0.000000000000, 0.000000000000},
    {0.148498500000, -0.148498500000, 0.000000000000, 0.000000000000},
    {-0.274223000000, 0.422721500000, 0.148498500000, 0.000000000000},
    {0.133226500000, -0.258951000000, 0.171272500000, 0.296997000000},
    {-0.002882000000, 0.010384000000, 0.053050000000, 0.342545000000},
    {0.031281000000, -0.026661000000, 0.065172000000, 0.403097000000},
    {0.007529000000, 0.028372000000, 0.105693000000, 0.472889000000},
    {-0.053713500000, 0.097143500000, 0.185024000000, 0.614483000000},
    {0.044259500000, -0.054543000000, 0.218170500000, 0.842937000000},
    {0.023648000000, 0.010328000000, 0.241863000000, 1.050824000000},
    {0.153253000000, -0.095629000000, 0.333463000000, 1.326663000000},
    {-0.091442000000, 0.302319000000, 0.601964000000, 1.717750000000},
    {1.061245000000, -0.941810000000, 0.932276000000, 2.530591000000},
    {-1.398234500000, 2.578914500000, 2.232391000000, 3.582302000000},
    {5.763138000000, -5.980692500000, 3.195516500000, 6.995373000000},
    {3.697740000000, 1.847843500000, 8.523545500000, 9.973335000000},
    {3.020036000000, 6.223287500000, 23.312452500000, 24.042464000000},
    {-7.635630000000, 19.898989500000, 44.819135500000, 56.598240000000},
    {-12.280767500000, 16.908497000000, 61.710224500000, 113.680735000000},
    {-0.337643500000, -7.315394500000, 58.684916000000, 180.018689000000},
    {4.893850000000, -12.884531500000, 43.041196500000, 231.050567000000},
    {2.852694000000, -5.949525500000, 31.953683500000, 266.101082000000},
    {-0.886192000000, 0.642054500000, 28.612714500000, 294.957934000000},
    {3.580761500000, -4.711091000000, 27.238247500000, 323.326511000000},
    {-4.090435000000, 6.540867000000, 28.558350000000, 349.434429000000},
    {4.297079500000, -5.937082500000, 29.368779000000, 380.443211000000},
    {-1.927613000000, 4.584689500000, 30.385852500000, 408.171987000000},
    {0.611222500000, 0.118241000000, 33.772392500000, 441.214916000000},
    {-0.788830500000, 2.129516500000, 35.842542000000, 475.716772000000},
    {0.893433000000, -0.341577500000, 37.735083500000, 512.900000000000},
    {-5.952585000000, 7.397873500000, 39.732227500000, 551.186939000000},
    {7.132492500000, -11.639789000000, 36.670219500000, 592.364455000000},
    {-12.709463500000, 15.334659500000, 34.788119000000, 624.527378000000},
    {24.269480500000, -34.353748000000, 27.329047500000, 661.940693000000},
    {-17.002259500000, 31.187472500000, 31.429993000000, 679.185473000000},
    {36.960603500000, -39.777650000000, 42.798159500000, 724.800679000000},
    {-49.653179500000, 83.796736500000, 74.124670000000, 764.781792000000},
    {-28.617892000000, 13.108269500000, 92.758604500000, 873.050019000000},
    {106.187137500000, -150.314652000000, 33.121467500000, 950.299001000000},
    {-127.872707000000, 189.932329999999, 51.053576000000, 939.292954000000},
    {114.214179500000, -180.027263500000, 47.300115000000, 1052.406153000000},
    {-34.049746500000, 82.450841999999, 29.888126500000, 1033.893184000000},
    {-8.786131500000, 23.137480500000, 92.640571000000, 1112.182406000000},
    {2.549443000000, 3.015774500000, 112.557137500000, 1219.174326000000},
    {0.131727500000, 7.982933000000, 126.237015500000, 1337.296681000000},
    {0.131727000000, 8.114661000000, 142.598064000000, 1471.648357000000},
    {0.131727500000, 8.246387500000, 159.222567000000, 1622.492809000000},
    {0.131727000000, 8.378115500000, 176.110524500000, 1790.093491000000},
    {0.131728000000, 8.509841500001, 193.261936500000, 1974.713858000000},
    {0.131726500000, 8.641571000000, 210.676803500000, 2176.617364000000},
    {0.131727500000, 8.773296500000, 228.355125000000, 2396.067465000000},
    {0.131727500000, 8.905024000001, 246.296900500000, 2633.327614000000},
    {0.131727500000, 9.036751500000, 264.502131000000, 2888.661266000000},
    {0.131727000000, 9.168479500000, 282.970816500000, 3162.331876000000},
    {0.131727000000, 9.300206500000, 301.702956500000, 3454.602899000000},
  }
};

static const double interp_dgrid_curv_precalc[2][62][4] = {
  {
    {0.000019500000, -0.000323500000, -0.037413000000, 15.962891000000},
    {0.000020500000, -0.000305000000, -0.038001500000, 15.925174000000},
    {0.000019000000, -0.000283000000, -0.038550000000, 15.886888000000},
    {0.000019500000, -0.000264500000, -0.039059000000, 15.848074000000},
    {0.000020500000, -0.000246000000, -0.039529500000, 15.808770000000},
    {0.000019000000, -0.000224000000, -0.039960000000, 15.769015000000},
    {0.000020500000, -0.000206500000, -0.040351000000, 15.728850000000},
    {0.000019000000, -0.000184500000, -0.040702500000, 15.688313000000},
    {0.000020000000, -0.000166500000, -0.041014500000, 15.647445000000},
    {0.001357500000, -0.001484000000, -0.041287500000, 15.606284000000},
    {-0.002804000000, 0.004035000000, -0.040183000000, 15.564870000000},
    {-0.032623000000, 0.031050000000, -0.040525000000, 15.525918000000},
    {-0.033802000000, -0.000394000000, -0.076294000000, 15.483820000000},
    {-0.053460000000, -0.014538000000, -0.178488000000, 15.373330000000},
    {0.139631500000, -0.261089500000, -0.367944000000, 15.126844000000},
    {-0.103804500000, 0.121978000000, -0.471228500000, 14.637442000000},
    {0.058113000000, -0.143744000000, -0.538686000000, 14.184387000000},
    {0.009833500000, -0.037351500000, -0.651835000000, 13.560070000000},
    {-0.018880000000, 0.001195500000, -0.697037500000, 12.880717000000},
    {-0.039197500000, 0.002633000000, -0.751286500000, 12.165995000000},
    {-0.108540000000, 0.032778000000, -0.863613000000, 11.378144000000},
    {0.016713000000, -0.201015000000, -1.123677000000, 10.438769000000},
    {0.089675500000, -0.257264500000, -1.475568000000, 9.130790000000},
    {0.266838500000, -0.344752000000, -1.721070500000, 7.487633000000},
    {-0.013965500000, 0.202890500000, -1.610059000000, 5.688649000000},
    {-0.020401500000, 0.195361000000, -1.246174500000, 4.267515000000},
    {-0.073577000000, 0.228135000000, -0.916657000000, 3.196300000000},
    {-0.012984500000, 0.093965500000, -0.681118000000, 2.434201000000},
    {-0.002924000000, 0.070920500000, -0.532140500000, 1.834064000000},
    {-0.028394000000, 0.093466500000, -0.399071500000, 1.369920000000},
    {-0.006549500000, 0.043228000000, -0.297320500000, 1.035921000000},
    {-0.003768500000, 0.033897500000, -0.230513000000, 0.775279000000},
    {-0.009083500000, 0.035444000000, -0.174023500000, 0.574895000000},
    {-0.001166000000, 0.018443000000, -0.130386000000, 0.427232000000},
    {-0.006565500000, 0.022676500000, -0.096998000000, 0.314123000000},
    {-0.000273500000, 0.009819000000, -0.071341500000, 0.233236000000},
    {-0.005359000000, 0.014631000000, -0.052524000000, 0.171440000000},
    {0.001203500000, 0.002709500000, -0.039339000000, 0.128188000000},
    {-0.001642500000, 0.006759000000, -0.030309500000, 0.092762000000},
    {-0.000848500000, 0.004322500000, -0.021719000000, 0.067569000000},
    {-0.000789500000, 0.003415000000, -0.015619500000, 0.049324000000},
    {-0.000752500000, 0.002588500000, -0.011158000000, 0.036330000000},
    {0.000337000000, 0.000746500000, -0.008238500000, 0.027008000000},
    {-0.001486500000, 0.002907000000, -0.005734500000, 0.019853000000},
    {0.001109000000, -0.001175000000, -0.004380000000, 0.015539000000},
    {-0.000417500000, 0.001460500000, -0.003403000000, 0.011093000000},
    {0.000169500000, 0.000456000000, -0.001734500000, 0.008733000000},
    {-0.002374500000, 0.003169500000, -0.000314000000, 0.007624000000},
    {0.002237500000, -0.003817000000, -0.001098500000, 0.008105000000},
    {-0.000296000000, 0.000954000000, -0.002020000000, 0.005427000000},
    {-0.000332500000, 0.000694500000, -0.001000000000, 0.004065000000},
    {0.000000000000, 0.000029500000, -0.000608500000, 0.003427000000},
    {-0.000001000000, 0.000030500000, -0.000549500000, 0.002848000000},
    {-0.000001000000, 0.000029500000, -0.000491500000, 0.002328000000},
    {-0.000000500000, 0.000028000000, -0.000435500000, 0.001865000000},
    {-0.000001000000, 0.000028000000, -0.000381000000, 0.001457000000},
    {-0.000000500000, 0.000026500000, -0.000328000000, 0.001103000000},
    {-0.000001000000, 0.000026500000, -0.000276500000, 0.000801000000},
    {-0.000001000000, 0.000025500000, -0.000226500000, 0.000550000000},
    {0.000000000000, 0.000023500000, -0.000178500000, 0.000348000000},
    {-0.000001500000, 0.000025000000, -0.000131500000, 0.000193000000},
    {-0.000000500000, 0.000022500000, -0.000086000000, 0.000085000000},
  },
  {
    {0.000227000000, -0.003958500000, -0.007615500000, 15.996116000000},
    {0.000228500000, -0.003733000000, -0.014851500000, 15.984769000000},
    {0.000228000000, -0.003504000000, -0.021632000000, 15.966413000000},
    {0.000227500000, -0.003275500000, -0.027956000000, 15.941505000000},
    {0.000228000000, -0.003048500000, -0.033824500000, 15.910501000000},
    {0.000227500000, -0.002820000000, -0.039237500000, 15.873856000000},
    {0.000228500000, -0.002593500000, -0.044195000000, 15.832026000000},
    {0.000227000000, -0.002363500000, -0.048696500000, 15.785466000000},
    {0.000228500000, -0.002138000000, -0.052742500000, 15.734633000000},
    {0.000185000000, -0.001866000000, -0.056333000000, 15.679981000000},
    {-0.018403000000, 0.016907000000, -0.059510000000, 15.621967000000},
    {-0.015594000000, -0.004305000000, -0.080905000000, 15.560961000000},
    {0.003435500000, -0.038928500000, -0.136297000000, 15.460157000000},
    {-0.142760000000, 0.110702500000, -0.203847500000, 15.288367000000},
    {0.194732500000, -0.369550000000, -0.410722500000, 15.052462000000},
    {-0.170820000000, 0.190735000000, -0.565625000000, 14.466922000000},
    {0.148821500000, -0.299726500000, -0.696615000000, 13.921212000000},
    {-0.064176000000, 0.062092500000, -0.849603500000, 13.073692000000},
    {-0.067613000000, 0.001353500000, -0.917946500000, 12.222005000000},
    {0.216335500000, -0.350208000000, -1.118078500000, 11.237799000000},
    {-0.276602500000, 0.359065500000, -1.169488000000, 9.985848000000},
    {0.217694500000, -0.411834000000, -1.281164500000, 8.898823000000},
    {0.079455500000, -0.055900500000, -1.451749000000, 7.423519000000},
    {-0.006484000000, 0.109494500000, -1.325183500000, 5.995325000000},
    {0.015126000000, 0.081400500000, -1.125646500000, 4.773152000000},
    {-0.030590500000, 0.142243000000, -0.917467500000, 3.744032000000},
    {-0.025273500000, 0.106335500000, -0.724753000000, 2.938217000000},
    {-0.007365000000, 0.063153500000, -0.587902500000, 2.294526000000},
    {0.016001500000, 0.032422000000, -0.483690500000, 1.762412000000},
    {-0.038813000000, 0.103238000000, -0.370842000000, 1.327145000000},
    {0.004491000000, 0.021121000000, -0.280805000000, 1.020728000000},
    {-0.004967000000, 0.035070000000, -0.225090000000, 0.765535000000},
    {-0.008782500000, 0.033918500000, -0.169851000000, 0.570548000000},
    {-0.000782500000, 0.017136000000, -0.128361500000, 0.425833000000},
    {-0.005955500000, 0.021526500000, -0.096437000000, 0.313825000000},
    {-0.000373000000, 0.009988500000, -0.071250500000, 0.232959000000},
    {-0.005379500000, 0.014622000000, -0.052392500000, 0.171324000000},
    {0.001253000000, 0.002610000000, -0.039287000000, 0.128174000000},
    {-0.001639500000, 0.006755500000, -0.030308000000, 0.092750000000},
    {-0.000851500000, 0.004328000000, -0.021715500000, 0.067558000000},
    {-0.000791500000, 0.003416500000, -0.015614000000, 0.049319000000},
    {-0.000750000000, 0.002583500000, -0.011155500000, 0.036330000000},
    {0.000337000000, 0.000746500000, -0.008238500000, 0.027008000000},
    {-0.001486500000, 0.002907000000, -0.005734500000, 0.019853000000},
    {0.001109000000, -0.001175000000, -0.004380000000, 0.015539000000},
    {-0.000417500000, 0.001460500000, -0.003403000000, 0.011093000000},
    {0.000169500000, 0.000456000000, -0.001734500000, 0.008733000000},
    {-0.002374500000, 0.003169500000, -0.000314000000, 0.007624000000},
    {0.002237500000, -0.003817000000, -0.001098500000, 0.008105000000},
    {-0.000296000000, 0.000954000000, -0.002020000000, 0.005427000000},
    {-0.000332500000, 0.000694500000, -0.001000000000, 0.004065000000},
    {0.000000000000, 0.000029500000, -0.000608500000, 0.003427000000},
    {-0.000001000000, 0.000030500000, -0.000549500000, 0.002848000000},
    {-0.000001000000, 0.000029500000, -0.000491500000, 0.002328000000},
    {-0.000000500000, 0.000028000000, -0.000435500000, 0.001865000000},
    {-0.000001000000, 0.000028000000, -0.000381000000, 0.001457000000},
    {-0.000000500000, 0.000026500000, -0.000328000000, 0.001103000000},
    {-0.000001000000, 0.000026500000, -0.000276500000, 0.000801000000},
    {-0.000001000000, 0.000025500000, -0.000226500000, 0.000550000000},
    {0.000000000000, 0.000023500000, -0.000178500000, 0.000348000000},
    {-0.000001500000, 0.000025000000, -0.000131500000, 0.000193000000},
    {-0.000000500000, 0.000022500000, -0.000086000000, 0.000085000000},
  },
};

static double interp_cubic(const double *p, double x) {
    return p[1] + 0.5 * x *
        (p[2] - p[0] +
            x * (2.0 * p[0] - 5.0 * p[1] + 4.0 * p[2] - p[3] +
                x * (3.0 * (p[1] - p[2]) + p[3] - p[0])));
}

static /*INLINE*/ double interp_cubic_precalc(const double *p, double x) {
    return p[3] + x * (p[2] + x * (p[1] + x * p[0]));
}

void av1_model_rd_curvfit(BLOCK_SIZE bsize, double sse_norm, double xqr,
    double *rate_f, double *distbysse_f) {
    const double x_start = -15.5;
    const double x_end = 16.5;
    const double x_step = 0.5;
    const double epsilon = 1e-6;
    const int rcat = bsize_curvfit_model_cat_lookup[bsize];
    const int dcat = sse_norm_curvfit_model_cat_lookup(sse_norm);
    (void)x_end;

    xqr = AOMMAX(xqr, x_start + x_step + epsilon);
    xqr = AOMMIN(xqr, x_end - x_step - epsilon);
    const double x = (xqr - x_start) / x_step;
    const int xi = (int)floor(x);
    const double xo = x - xi;

    assert(xi > 0);

#if NO_LOG2_DOUBLE
    const double *prate = &interp_rgrid_curv[rcat][(xi - 1)];
    *rate_f = prate[1];
    const double *pdist = &interp_dgrid_curv[dcat][(xi - 1)];
    *distbysse_f = pdist[1];
#else
    const double *prate_precalc = &interp_rgrid_curv_precalc[rcat][(xi - 1)];
    *rate_f = interp_cubic_precalc(prate_precalc, xo);
    const double *pdist_precalc = &interp_dgrid_curv_precalc[dcat][(xi - 1)];
    *distbysse_f = interp_cubic_precalc(pdist_precalc, xo);
#endif

}
// Fits a curve for rate and distortion using as feature:
// log2(sse_norm/qstep^2)
static void model_rd_with_curvfit(
    //const AV1_COMP *const cpi,
    //const MACROBLOCK *const x,
    PictureControlSet      *picture_control_set_ptr,
    BLOCK_SIZE plane_bsize, /*int plane,*/
    int64_t sse, int num_samples, int *rate,
    int64_t *dist,
    uint32_t rdmult
    )
{

    (void)plane_bsize;
    //const MACROBLOCKD *const xd = &x->e_mbd;
    //const struct macroblockd_plane *const pd = &xd->plane[plane];
    const int dequant_shift = /*(is_cur_buf_hbd(xd)) ? xd->bd - 5 :*/ 3;

    int32_t current_q_index = MAX(0, MIN(QINDEX_RANGE - 1, picture_control_set_ptr->parent_pcs_ptr->base_qindex));
    Dequants *const dequants = &picture_control_set_ptr->parent_pcs_ptr->deq;
    int16_t quantizer = dequants->y_dequant_Q3[current_q_index][1];

    const int qstep = AOMMAX(quantizer /*pd->dequant_Q3[1]*/ >> dequant_shift, 1);

    if (sse == 0) {
        if (rate) *rate = 0;
        if (dist) *dist = 0;
        return;
    }
    aom_clear_system_state();
    const double sse_norm = (double)sse / num_samples;
#if NO_LOG2_DOUBLE
    const double xqr = (double)LOG2F((sse / num_samples) / (qstep * qstep));
#else
    const double qstepsqr = (double)qstep * qstep;
    const double xqr = log2(sse_norm / qstepsqr);
#endif

    double rate_f, dist_by_sse_norm_f;
    av1_model_rd_curvfit(plane_bsize, sse_norm, xqr, &rate_f,&dist_by_sse_norm_f);

    const double dist_f = dist_by_sse_norm_f * sse_norm;
    int rate_i = (int)(AOMMAX(0.0, rate_f * num_samples) + 0.5);
    int64_t dist_i = (int64_t)(AOMMAX(0.0, dist_f * num_samples) + 0.5);
    aom_clear_system_state();

    // Check if skip is better
    if (rate_i == 0) {
        dist_i = sse << 4;
    }
    else if ( RDCOST( rdmult, rate_i, dist_i) >= RDCOST( rdmult, 0, sse << 4) ) {
        rate_i = 0;
        dist_i = sse << 4;
    }

    if (rate) *rate = rate_i;
    if (dist) *dist = dist_i;
}


/**
 * Compute the element-wise difference of the squares of 2 arrays.
 *
 * d: Difference of the squares of the inputs: a**2 - b**2
 * a: First input array
 * b: Second input array
 * N: Number of elements
 *
 * 'd', 'a', and 'b' are contiguous.
 *
 * The result is saturated to signed 16 bits.
 */
void av1_wedge_compute_delta_squares_c(int16_t *d, const int16_t *a,
                                       const int16_t *b, int N) {
  int i;

  for (i = 0; i < N; i++)
    d[i] = clamp(a[i] * a[i] - b[i] * b[i], INT16_MIN, INT16_MAX);
}

uint64_t aom_sum_squares_i16_c(const int16_t *src, uint32_t n) {
  uint64_t ss = 0;
  do {
    const int16_t v = *src++;
    ss += v * v;
  } while (--n);

  return ss;
}
/**
 * Choose the mask sign for a compound predictor.
 *
 * ds:    Difference of the squares of the residuals.
 *        r0**2 - r1**2
 * m:     The blending mask
 * N:     Number of pixels
 * limit: Pre-computed threshold value.
 *        MAX_MASK_VALUE/2 * (sum(r0**2) - sum(r1**2))
 *
 * 'ds' and 'm' are contiguous.
 *
 * Returns true if the negated mask has lower SSE compared to the positive
 * mask. Computation is based on:
 *  Sum((mask*r0 + (MAX_MASK_VALUE-mask)*r1)**2)
 *                                     >
 *                                Sum(((MAX_MASK_VALUE-mask)*r0 + mask*r1)**2)
 *
 *  which can be simplified to:
 *
 *  Sum(mask*(r0**2 - r1**2)) > MAX_MASK_VALUE/2 * (sum(r0**2) - sum(r1**2))
 *
 *  The right hand side does not depend on the mask, and needs to be passed as
 *  the 'limit' parameter.
 *
 *  After pre-computing (r0**2 - r1**2), which is passed in as 'ds', the left
 *  hand side is simply a scalar product between an int16_t and uint8_t vector.
 *
 *  Note that for efficiency, ds is stored on 16 bits. Real input residuals
 *  being small, this should not cause a noticeable issue.
 */
int8_t av1_wedge_sign_from_residuals_c(const int16_t *ds, const uint8_t *m,
                                       int N, int64_t limit) {
  int64_t acc = 0;

  do {
    acc += *ds++ * *m++;
  } while (--N);

  return acc > limit;
}
static void /*int64_t*/ pick_wedge(
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    //const AV1_COMP *const cpi,
    //const MACROBLOCK *const x,
    const BLOCK_SIZE bsize,
    const uint8_t *const p0,
    const int16_t *const residual1,
    const int16_t *const diff10,
    int8_t *const best_wedge_sign,
    int8_t *const best_wedge_index) {
 // const MACROBLOCKD *const xd = &x->e_mbd;
 // const struct buf_2d *const src = &x->plane[0].src;
    EbPictureBufferDesc   *src_pic = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    uint8_t               *src_buf  = src_pic->buffer_y + (context_ptr->cu_origin_x + src_pic->origin_x) + (context_ptr->cu_origin_y + src_pic->origin_y) * src_pic->stride_y;

  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];
  const int N = bw * bh;
  assert(N >= 64);
  int rate;
  int64_t dist;
  int64_t rd, best_rd = INT64_MAX;
  int8_t wedge_index;
  int8_t wedge_sign;
  int8_t wedge_types = (1 << get_wedge_bits_lookup(bsize));
  const uint8_t *mask;
  uint64_t sse;
  //const int hbd = is_cur_buf_hbd(xd);
  //const int bd_round = hbd ? (xd->bd - 8) * 2 : 0;
  const int bd_round = 0;
  DECLARE_ALIGNED(32, int16_t, residual0[MAX_SB_SQUARE]);  // src - pred0
  //if (hbd) {
  //  aom_highbd_subtract_block(bh, bw, residual0, bw, src->buf, src->stride,
  //                            CONVERT_TO_BYTEPTR(p0), bw, xd->bd);
  //} else {
    aom_subtract_block(bh, bw, residual0, bw, src_buf/*src->buf*/, src_pic->stride_y/*src->stride*/, p0, bw);
  //}

  int64_t sign_limit = ((int64_t)aom_sum_squares_i16(residual0, N) -
                        (int64_t)aom_sum_squares_i16(residual1, N)) *
                       (1 << WEDGE_WEIGHT_BITS) / 2;
  int16_t *ds = residual0;

  av1_wedge_compute_delta_squares(ds, residual0, residual1, N);

  for (wedge_index = 0; wedge_index < wedge_types; ++wedge_index) {
    mask = av1_get_contiguous_soft_mask(wedge_index, 0, bsize);

    wedge_sign = av1_wedge_sign_from_residuals(ds, mask, N, sign_limit);

    mask = av1_get_contiguous_soft_mask(wedge_index, wedge_sign, bsize);
    sse = av1_wedge_sse_from_residuals(residual1, diff10, mask, N);
    sse = ROUND_POWER_OF_TWO(sse, bd_round);

    //model_rd_sse_fn[MODELRD_TYPE_MASKED_COMPOUND](cpi, x, bsize, 0, sse, N,
     //                                             &rate, &dist);
    model_rd_with_curvfit(picture_control_set_ptr,bsize, /*0,*/ sse, N, &rate, &dist, context_ptr->full_lambda);
    // int rate2;
    // int64_t dist2;
    // model_rd_with_curvfit(cpi, x, bsize, 0, sse, N, &rate2, &dist2);
    // printf("sse %"PRId64": leagacy: %d %"PRId64", curvfit %d %"PRId64"\n",
    // sse, rate, dist, rate2, dist2); dist = dist2;
    // rate = rate2;

    //rate += x->wedge_idx_cost[bsize][wedge_index];
    rd = RDCOST(context_ptr->full_lambda/*x->rdmult*/, rate, dist);

    if (rd < best_rd) {
      *best_wedge_index = wedge_index;
      *best_wedge_sign = wedge_sign;
      best_rd = rd;
    }
  }

  /*return best_rd -
         RDCOST(x->rdmult, x->wedge_idx_cost[bsize][*best_wedge_index], 0);*/
}

extern aom_variance_fn_ptr_t mefn_ptr[BlockSizeS_ALL];

// This is used as a reference when computing the source variance for the
//  purposes of activity masking.
// Eventually this should be replaced by custom no-reference routines,
//  which will be faster.
//const uint8_t AV1_VAR_OFFS[MAX_SB_SIZE] = {
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128,
//  128, 128, 128, 128, 128, 128, 128, 128
//};

//unsigned int av1_get_sby_perpixel_variance(const aom_variance_fn_ptr_t *fn_ptr, //const AV1_COMP *cpi,
//                                           const uint8_t *src,int stride,//const struct buf_2d *ref,
//                                           BlockSize bs) {
//  unsigned int sse;
//  const unsigned int var =
//      //cpi->fn_ptr[bs].vf(ref->buf, ref->stride, AV1_VAR_OFFS, 0, &sse);
//     fn_ptr->vf(src,  stride, AV1_VAR_OFFS, 0, &sse);
//  return ROUND_POWER_OF_TWO(var, num_pels_log2_lookup[bs]);
//}

static int8_t estimate_wedge_sign(

    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    //const AV1_COMP *cpi, const MACROBLOCK *x,
    const BLOCK_SIZE bsize,
    const uint8_t *pred0,
    int stride0,
    const uint8_t *pred1,
    int stride1) {
  static const BLOCK_SIZE split_qtr[BlockSizeS_ALL] = {
    //                            4X4
    BLOCK_INVALID,
    // 4X8,        8X4,           8X8
    BLOCK_INVALID, BLOCK_INVALID, BLOCK_4X4,
    // 8X16,       16X8,          16X16
    BLOCK_4X8, BLOCK_8X4, BLOCK_8X8,
    // 16X32,      32X16,         32X32
    BLOCK_8X16, BLOCK_16X8, BLOCK_16X16,
    // 32X64,      64X32,         64X64
    BLOCK_16X32, BLOCK_32X16, BLOCK_32X32,
    // 64x128,     128x64,        128x128
    BLOCK_32X64, BLOCK_64X32, BLOCK_64X64,
    // 4X16,       16X4,          8X32
    BLOCK_INVALID, BLOCK_INVALID, BLOCK_4X16,
    // 32X8,       16X64,         64X16
    BLOCK_16X4, BLOCK_8X32, BLOCK_32X8
  };
 // const struct macroblock_plane *const p = &x->plane[0];
 //const uint8_t *src = p->src.buf;
 //int src_stride = p->src.stride;
  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];
  uint32_t esq[2][4];
  int64_t tl, br;

  const BLOCK_SIZE f_index = split_qtr[bsize];
  assert(f_index != BLOCK_INVALID);

  //if (is_cur_buf_hbd(&x->e_mbd)) {
  //  pred0 = CONVERT_TO_BYTEPTR(pred0);
  //  pred1 = CONVERT_TO_BYTEPTR(pred1);
  //}

    const aom_variance_fn_ptr_t *fn_ptr = &mefn_ptr[bsize];
    EbPictureBufferDesc   *src_pic = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    uint8_t               *src_buf  = src_pic->buffer_y + (context_ptr->cu_origin_x + src_pic->origin_x) + (context_ptr->cu_origin_y + src_pic->origin_y) * src_pic->stride_y;

    //  fn_ptr->vf(src_buf,  src_pic->stride_y , pred0, stride0,  &esq[0][0]);
      //cpi->fn_ptr[bs].vf(ref->buf, ref->stride, AV1_VAR_OFFS, 0, &sse);

  fn_ptr->vf(src_buf                                            , src_pic->stride_y    , pred0                              , stride0, &esq[0][0]);
  fn_ptr->vf(src_buf + bw / 2                                   , src_pic->stride_y    , pred0 + bw / 2                     , stride0, &esq[0][1]);
  fn_ptr->vf(src_buf + bh / 2 * src_pic->stride_y               , src_pic->stride_y    , pred0 + bh / 2 * stride0           , stride0, &esq[0][2]);
  fn_ptr->vf(src_buf + bh / 2 * src_pic->stride_y + bw / 2      , src_pic->stride_y    , pred0 + bh / 2 * stride0 + bw / 2  , stride0, &esq[0][3]);
  fn_ptr->vf(src_buf                                            , src_pic->stride_y    , pred1                              , stride1, &esq[1][0]);
  fn_ptr->vf(src_buf + bw / 2                                   , src_pic->stride_y    , pred1 + bw / 2                     , stride1, &esq[1][1]);
  fn_ptr->vf(src_buf + bh / 2 * src_pic->stride_y               , src_pic->stride_y    , pred1 + bh / 2 * stride1           , stride0, &esq[1][2]);
  fn_ptr->vf(src_buf + bh / 2 * src_pic->stride_y + bw / 2      , src_pic->stride_y    , pred1 + bh / 2 * stride1 + bw / 2  , stride0, &esq[1][3]);

  tl = ((int64_t)esq[0][0] + esq[0][1] + esq[0][2]) -
       ((int64_t)esq[1][0] + esq[1][1] + esq[1][2]);
  br = ((int64_t)esq[1][3] + esq[1][1] + esq[1][2]) -
       ((int64_t)esq[0][3] + esq[0][1] + esq[0][2]);
  return (tl + br > 0);
}

// Choose the best wedge index the specified sign
#if II_COMP
int64_t pick_wedge_fixed_sign(
#else
static int64_t pick_wedge_fixed_sign(
#endif
#if FIX_RATE_E_WEDGE || II_COMP
    ModeDecisionCandidate        *candidate_ptr,
#endif
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    //const AV1_COMP *const cpi,
    //const MACROBLOCK *const x,
    const BLOCK_SIZE bsize,
    const int16_t *const residual1,
    const int16_t *const diff10,
    const int8_t wedge_sign,
    int8_t *const best_wedge_index) {
  //const MACROBLOCKD *const xd = &x->e_mbd;

  const int bw = block_size_wide[bsize];
  const int bh = block_size_high[bsize];
  const int N = bw * bh;
  assert(N >= 64);
  int rate;
  int64_t dist;
  int64_t rd, best_rd = INT64_MAX;
  int8_t wedge_index;
  int8_t wedge_types = (1 << get_wedge_bits_lookup(bsize));
  const uint8_t *mask;
  uint64_t sse;
  //const int hbd = 0;// is_cur_buf_hbd(xd);
  const int bd_round = 0;//hbd ? (xd->bd - 8) * 2 : 0;
  for (wedge_index = 0; wedge_index < wedge_types; ++wedge_index) {
    mask = av1_get_contiguous_soft_mask(wedge_index, wedge_sign, bsize);
    sse = av1_wedge_sse_from_residuals(residual1, diff10, mask, N);
    sse = ROUND_POWER_OF_TWO(sse, bd_round);

    model_rd_with_curvfit(picture_control_set_ptr,bsize, /*0,*/ sse, N,    &rate, &dist, context_ptr->full_lambda);
   // model_rd_sse_fn[MODELRD_TYPE_MASKED_COMPOUND](cpi, x, bsize, 0, sse, N, &rate, &dist);

   // rate += x->wedge_idx_cost[bsize][wedge_index];
#if FIX_RATE_E_WEDGE
    rate  += candidate_ptr->md_rate_estimation_ptr->wedge_idx_fac_bits[bsize][wedge_index];
#endif
    rd = RDCOST(/*x->rdmult*/context_ptr->full_lambda, rate, dist);

    if (rd < best_rd) {
      *best_wedge_index = wedge_index;
      best_rd = rd;
    }
  }
  return best_rd ;//- RDCOST(x->rdmult, x->wedge_idx_cost[bsize][*best_wedge_index], 0);
}
int is_interinter_compound_used(COMPOUND_TYPE type,
    BLOCK_SIZE sb_type);
static void /*int64_t*/ pick_interinter_wedge(
#if FIX_RATE_E_WEDGE  || II_COMP
    ModeDecisionCandidate               *candidate_ptr,
#endif
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    INTERINTER_COMPOUND_DATA             *interinter_comp,
   //const AV1_COMP *const cpi,
   //MACROBLOCK *const x,
    const BLOCK_SIZE bsize,
    const uint8_t *const p0,
    const uint8_t *const p1,
    const int16_t *const residual1,
    const int16_t *const diff10) {
//  MACROBLOCKD *const xd = &x->e_mbd;
//  MB_MODE_INFO *const mbmi = xd->mi[0];
  const int bw = block_size_wide[bsize];

  int64_t rd;
  int8_t wedge_index = -1;
  int8_t wedge_sign = 0;

  assert(is_interinter_compound_used(COMPOUND_WEDGE, bsize));
  //assert(cpi->common.seq_params.enable_masked_compound);
 // assert(picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->seq_header->enable_masked_compound  > 0);

  // Two method
  // Fast seatch method to be added  OMK

  if (picture_control_set_ptr->parent_pcs_ptr->wedge_mode == 2 || picture_control_set_ptr->parent_pcs_ptr->wedge_mode == 3) {
    wedge_sign = estimate_wedge_sign(/*cpi, x, */picture_control_set_ptr,context_ptr, bsize, p0, bw, p1, bw);
#if FIX_RATE_E_WEDGE || II_COMP
    rd = pick_wedge_fixed_sign(/*cpi, x, */
#if FIX_RATE_E_WEDGE
        candidate_ptr,
#endif
        picture_control_set_ptr,context_ptr, bsize, residual1, diff10, wedge_sign,
#else
    rd = pick_wedge_fixed_sign(/*cpi, x, */picture_control_set_ptr,context_ptr, bsize, residual1, diff10, wedge_sign,
#endif
                               &wedge_index);
  } else {
    /*rd =*/ pick_wedge(/*cpi, x, */picture_control_set_ptr,context_ptr,
        bsize, p0, residual1, diff10, &wedge_sign,
                    &wedge_index);
  }

  interinter_comp->wedge_sign = wedge_sign;
  interinter_comp->wedge_index = wedge_index;
//  return rd;
}

static void  pick_interinter_seg(
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    INTERINTER_COMPOUND_DATA             *interinter_comp,
    //const AV1_COMP *const cpi,
    //MACROBLOCK *const x,
    const BLOCK_SIZE bsize,
    const uint8_t *const p0,
    const uint8_t *const p1,
    const int16_t *const residual1,
    const int16_t *const diff10)
{
    //MACROBLOCKD *const xd = &x->e_mbd;
    //MB_MODE_INFO *const mbmi = xd->mi[0];
    const int bw = block_size_wide[bsize];
    const int bh = block_size_high[bsize];
    const int N = 1 << num_pels_log2_lookup[bsize];
    int rate;
    int64_t dist;
    DIFFWTD_MASK_TYPE cur_mask_type;
    int64_t best_rd = INT64_MAX;
    DIFFWTD_MASK_TYPE best_mask_type = 0;
    //const int hbd = is_cur_buf_hbd(xd);
    //const int bd_round = hbd ? (xd->bd - 8) * 2 : 0;
    DECLARE_ALIGNED(16, uint8_t, seg_mask0[2 * MAX_SB_SQUARE]);
    DECLARE_ALIGNED(16, uint8_t, seg_mask1[2 * MAX_SB_SQUARE]);
    uint8_t *tmp_mask[2] = { seg_mask0, seg_mask1 };

    // try each mask type and its inverse
    for (cur_mask_type = 0; cur_mask_type < DIFFWTD_MASK_TYPES; cur_mask_type++) {

        // build mask and inverse
#if COMP_AVX
        av1_build_compound_diffwtd_mask(tmp_mask[cur_mask_type], cur_mask_type,
            p0, bw, p1, bw, bh, bw);
#else
        av1_build_compound_diffwtd_mask_c(tmp_mask[cur_mask_type], cur_mask_type,
            p0, bw, p1, bw, bh, bw);
#endif
        // compute rd for mask
#if COMP_AVX
        uint64_t sse = av1_wedge_sse_from_residuals(residual1, diff10, tmp_mask[cur_mask_type], N);
#else
        uint64_t sse =  av1_wedge_sse_from_residuals_c(residual1, diff10, tmp_mask[cur_mask_type], N);
#endif
        sse = ROUND_POWER_OF_TWO(sse, 0 /*bd_round*/);

        model_rd_with_curvfit(picture_control_set_ptr,bsize, /*0,*/ sse, N, &rate, &dist, context_ptr->full_lambda);

        const int64_t rd0 = RDCOST(context_ptr->full_lambda /*x->rdmult*/, rate, dist);

        if (rd0 < best_rd) {
            best_mask_type = cur_mask_type;
            best_rd = rd0;
        }
    }

    interinter_comp->mask_type = best_mask_type;
    //if (best_mask_type == DIFFWTD_38_INV) {
    //    memcpy(xd->seg_mask, seg_mask, N * 2);
    //}
    //return best_rd;
}
void pick_interinter_mask(
#if FIX_RATE_E_WEDGE  || II_COMP
    ModeDecisionCandidate        *candidate_ptr,
#endif
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    INTERINTER_COMPOUND_DATA             *interinter_comp,
    const BLOCK_SIZE bsize,
    const uint8_t *const p0,
    const uint8_t *const p1,
    const int16_t *const residual1,
    const int16_t *const diff10)
{

    if (interinter_comp->type == COMPOUND_WEDGE) {

        //return
#if FIX_RATE_E_WEDGE  || II_COMP
        pick_interinter_wedge(candidate_ptr,picture_control_set_ptr,context_ptr, interinter_comp, bsize, p0, p1, residual1, diff10);
#else
        pick_interinter_wedge(picture_control_set_ptr,context_ptr, interinter_comp, bsize, p0, p1, residual1, diff10);
#endif
    }
    else if (interinter_comp->type == COMPOUND_DIFFWTD) {
        pick_interinter_seg(picture_control_set_ptr,context_ptr, interinter_comp,bsize, p0, p1, residual1, diff10);
    }
    else {
        assert(0);
    }
}
void search_compound_diff_wedge(
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    ModeDecisionCandidate                *candidate_ptr    )
{

#if PRE_BILINEAR_CLEAN_UP // compound
#if BILINEAR_INJECTION
    candidate_ptr->interp_filters = av1_make_interp_filters(BILINEAR, BILINEAR);
#else
    candidate_ptr->interp_filters = 0;
#endif
#endif
    //if (*calc_pred_masked_compound)
    {
        EbPictureBufferDesc   *src_pic = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
        uint8_t               *src_buf  = src_pic->buffer_y + (context_ptr->cu_origin_x + src_pic->origin_x) + (context_ptr->cu_origin_y + src_pic->origin_y) * src_pic->stride_y;

        uint32_t  bwidth = context_ptr->blk_geom->bwidth;
        uint32_t  bheight = context_ptr->blk_geom->bheight;
        EbPictureBufferDesc  pred_desc;
        pred_desc.origin_x = pred_desc.origin_y = 0;
        pred_desc.stride_y = bwidth;

        SequenceControlSet* sequence_control_set_ptr = ((SequenceControlSet*)(picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr));
        EbPictureBufferDesc  *ref_pic_list0;
        EbPictureBufferDesc  *ref_pic_list1 = NULL;
        Mv mv_0;
        Mv mv_1;
        mv_0.x = candidate_ptr->motion_vector_xl0;
        mv_0.y = candidate_ptr->motion_vector_yl0;
        mv_1.x = candidate_ptr->motion_vector_xl1;
        mv_1.y = candidate_ptr->motion_vector_yl1;
        MvUnit mv_unit;
        mv_unit.mv[0] = mv_0;
        mv_unit.mv[1] = mv_1;
        int8_t ref_idx_l0 = candidate_ptr->ref_frame_index_l0;
        int8_t ref_idx_l1 = candidate_ptr->ref_frame_index_l1;
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, candidate_ptr->ref_frame_type);
        uint8_t list_idx0, list_idx1;
        list_idx0 = get_list_idx(rf[0]);
        if (rf[1] == NONE_FRAME)
            list_idx1 = get_list_idx(rf[0]);
        else
            list_idx1 = get_list_idx(rf[1]);
        assert(list_idx0 < MAX_NUM_OF_REF_PIC_LIST);
        assert(list_idx1 < MAX_NUM_OF_REF_PIC_LIST);
        if (ref_idx_l0 >= 0)
            ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr)->reference_picture;
        else
            ref_pic_list0 = (EbPictureBufferDesc*)EB_NULL;
        if (ref_idx_l1 >= 0)
            ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]->object_ptr)->reference_picture;
        else
            ref_pic_list1 = (EbPictureBufferDesc*)EB_NULL;

        //CHKN get seperate prediction of each ref(Luma only)
        //ref0 prediction
        mv_unit.pred_direction = UNI_PRED_LIST_0;
        pred_desc.buffer_y = context_ptr->pred0;

        //we call the regular inter prediction path here(no compound)
        av1_inter_prediction(
            picture_control_set_ptr,
#if PRE_BILINEAR_CLEAN_UP // compound
            candidate_ptr->interp_filters,
#else
            0,//fixed interpolation filter for compound search
#endif
            context_ptr->cu_ptr,
            candidate_ptr->ref_frame_type,
            &mv_unit,
            0,//use_intrabc,
#if COMP_MODE
            1,//compound_idx not used
#endif
#if COMP_DIFF
            NULL,// interinter_comp not used
#endif
#if II_ED
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            0,
            0,
            0,
#endif
            context_ptr->cu_origin_x,
            context_ptr->cu_origin_y,
            bwidth,
            bheight,
            ref_pic_list0,
            ref_pic_list1,
            &pred_desc, //output
            0,          //output origin_x,
            0,          //output origin_y,
            0,//do chroma
            sequence_control_set_ptr->encode_context_ptr->asm_type);

        //ref1 prediction
        mv_unit.pred_direction = UNI_PRED_LIST_1;
        pred_desc.buffer_y = context_ptr->pred1;

        //we call the regular inter prediction path here(no compound)
        av1_inter_prediction(
            picture_control_set_ptr,
#if PRE_BILINEAR_CLEAN_UP // compound
            candidate_ptr->interp_filters,
#else
            0,//fixed interpolation filter for compound search
#endif
            context_ptr->cu_ptr,
            candidate_ptr->ref_frame_type,
            &mv_unit,
            0,//use_intrabc,
#if COMP_MODE
            1,//compound_idx not used
#endif
#if COMP_DIFF
            NULL,// interinter_comp not used
#endif
#if II_ED
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            0,
            0,
            0,
#endif
            context_ptr->cu_origin_x,
            context_ptr->cu_origin_y,
            bwidth,
            bheight,
            ref_pic_list0,
            ref_pic_list1,
            &pred_desc, //output
            0,          //output origin_x,
            0,          //output origin_y,
            0,//do chroma
            sequence_control_set_ptr->encode_context_ptr->asm_type);
#if COMP_AVX
        aom_subtract_block(bheight, bwidth, context_ptr->residual1, bwidth, src_buf, src_pic->stride_y, context_ptr->pred1, bwidth);
        aom_subtract_block(bheight, bwidth, context_ptr->diff10, bwidth, context_ptr->pred1, bwidth, context_ptr->pred0, bwidth);
#else
        aom_subtract_block_c(bheight, bwidth, context_ptr->residual1, bwidth, src_buf, src_pic->stride_y, context_ptr->pred1, bwidth);
        aom_subtract_block_c(bheight, bwidth, context_ptr->diff10, bwidth, context_ptr->pred1, bwidth, context_ptr->pred0, bwidth);
#endif
        //*calc_pred_masked_compound = 0;


#if COMP_MODE
        if (picture_control_set_ptr->parent_pcs_ptr->wedge_mode == 1 || picture_control_set_ptr->parent_pcs_ptr->wedge_mode == 3)
            if (candidate_ptr->interinter_comp.type == COMPOUND_DIFFWTD && context_ptr->variance_ready == 0) {
                const aom_variance_fn_ptr_t *fn_ptr = &mefn_ptr[context_ptr->blk_geom->bsize];
                //EbPictureBufferDesc   *src_pic = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
                //uint8_t               *src_buf  = src_pic->buffer_y + (context_ptr->cu_origin_x + src_pic->origin_x) + (context_ptr->cu_origin_y + src_pic->origin_y) * src_pic->stride_y;

                //  fn_ptr->vf(src_buf,  src_pic->stride_y , pred0, stride0,  &esq[0][0]);
                  //cpi->fn_ptr[bs].vf(ref->buf, ref->stride, AV1_VAR_OFFS, 0, &sse);
                unsigned int sse;
                (void)fn_ptr->vf(context_ptr->pred0, bwidth, context_ptr->pred1, pred_desc.stride_y, &sse);

                /*  if (cpi->sf.prune_wedge_pred_diff_based && compound_type == COMPOUND_WEDGE) {
                    unsigned int sse;
                    if (is_cur_buf_hbd(xd))
                      (void)cpi->fn_ptr[bsize].vf(CONVERT_TO_BYTEPTR(*preds0), *strides,
                                                CONVERT_TO_BYTEPTR(*preds1), *strides, &sse);
                    else
                      (void)cpi->fn_ptr[bsize].vf(*preds0, *strides, *preds1, *strides, &sse); */
                /*    const unsigned int */context_ptr->prediction_mse = ROUND_POWER_OF_TWO(sse, num_pels_log2_lookup[context_ptr->blk_geom->bsize]);
                context_ptr->variance_ready  = 1;
                // If two predictors are very similar, skip wedge compound mode search
                //if (mse < 8 || (!have_newmv_in_inter_mode(this_mode) && mse < 64)) {
                //  *comp_model_rd_cur = INT64_MAX;
                //  return INT64_MAX;
                //}
              //}
            }
#endif
    }
    pick_interinter_mask(
#if FIX_RATE_E_WEDGE    || II_COMP
        candidate_ptr,
#endif
        picture_control_set_ptr,
        context_ptr,
        &candidate_ptr->interinter_comp,
        context_ptr->blk_geom->bsize,
        context_ptr->pred0,
        context_ptr->pred1,
        context_ptr->residual1,
        context_ptr->diff10);
}

int64_t aom_sse_c(const uint8_t *a, int a_stride, const uint8_t *b,
    int b_stride, int width, int height) {
    int y, x;
    int64_t sse = 0;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            const int32_t diff = abs(a[x] - b[x]);
            sse += diff * diff;
        }

        a += a_stride;
        b += b_stride;
    }
    return sse;
}
#if II_COMP
void model_rd_for_sb_with_curvfit(
#else
static void model_rd_for_sb_with_curvfit(
#endif
    //const AV1_COMP *const cpi,  MACROBLOCK *x, MACROBLOCKD *xd,
    PictureControlSet      *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    BLOCK_SIZE bsize,int bw, int bh,
    uint8_t* src_buf, uint32_t src_stride, uint8_t* pred_buf, uint32_t pred_stride,
    int plane_from, int plane_to, int mi_row, int mi_col, int *out_rate_sum,
    int64_t *out_dist_sum, int *skip_txfm_sb, int64_t *skip_sse_sb,
    int *plane_rate, int64_t *plane_sse, int64_t *plane_dist) {
    (void)mi_row;
    (void)mi_col;
    // Note our transform coeffs are 8 times an orthogonal transform.
    // Hence quantizer step is also 8 times. To get effective quantizer
    // we need to divide by 8 before sending to modeling function.

    int64_t rate_sum = 0;
    int64_t dist_sum = 0;
    int64_t total_sse = 0;

    for (int plane = plane_from; plane <= plane_to; ++plane) {
        //CHKN struct macroblockd_plane *const pd = 0;   &xd->plane[plane];
        int32_t subsampling = plane == 0 ? 0 : 1;

        const BLOCK_SIZE plane_bsize =
            get_plane_block_size(bsize, /*pd->*/subsampling, /*pd->*/subsampling);
        int64_t dist, sse;
        int rate;

        //CHKN if (x->skip_chroma_rd && plane) continue;


        //const struct macroblock_plane *const p = &x->plane[plane];
        const int shift = 0;//CHKN (xd->bd - 8);
        //CHKN get_txb_dimensions(xd, plane, plane_bsize, 0, 0, plane_bsize, NULL, NULL,&bw, &bh);

/*        if (is_cur_buf_hbd(xd)) {
            sse = aom_highbd_sse(p->src.buf, p->src.stride, pd->dst.buf,pd->dst.stride, bw, bh);
        }
        else */{
#if COMP_AVX
              sse = aom_sse(src_buf, src_stride,pred_buf, pred_stride, bw,bh);
#else
              sse = aom_sse_c(src_buf, src_stride,pred_buf, pred_stride, bw,bh);
#endif
        }

        sse = ROUND_POWER_OF_TWO(sse, shift * 2);
        model_rd_with_curvfit(picture_control_set_ptr /*cpi, x*/, plane_bsize, /*plane,*/ sse, bw * bh, &rate,        &dist, context_ptr->full_lambda);

        //if (plane == 0) x->pred_sse[ref] = (unsigned int)AOMMIN(sse, UINT_MAX);

        total_sse += sse;
        rate_sum += rate;
        dist_sum += dist;

        if (plane_rate) plane_rate[plane] = rate;
        if (plane_sse) plane_sse[plane] = sse;
        if (plane_dist) plane_dist[plane] = dist;
    }

    if (skip_txfm_sb) *skip_txfm_sb = total_sse == 0;
    if (skip_sse_sb) *skip_sse_sb = total_sse << 4;
    *out_rate_sum = (int)rate_sum;
    *out_dist_sum = dist_sum;
}
int get_comp_index_context_enc(
    PictureParentControlSet   *pcs_ptr,
    int cur_frame_index,
    int bck_frame_index,
    int fwd_frame_index,
    const MACROBLOCKD *xd);
void search_compound_avg_dist(
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionContext                  *context_ptr,
    ModeDecisionCandidate                *candidate_ptr)
{
    int64_t est_rd[2];

    MbModeInfo *const mbmi = &context_ptr->cu_ptr->av1xd->mi[0]->mbmi;
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, candidate_ptr->ref_frame_type);
    mbmi->ref_frame[0] = rf[0];
    mbmi->ref_frame[1] = rf[1];
    const int comp_index_ctx = get_comp_index_context_enc(
        picture_control_set_ptr->parent_pcs_ptr,
        picture_control_set_ptr->parent_pcs_ptr->cur_order_hint,
        picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[0] - 1],
        picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[1] - 1],
        context_ptr->cu_ptr->av1xd);

    //COMPOUND AVERAGE
    COMPOUND_TYPE  comp_i;

    for(comp_i= COMPOUND_AVERAGE;  comp_i<=COMPOUND_DISTWTD;    comp_i++)
    {
        //assign compound type temporary for RD test
        candidate_ptr->interinter_comp.type = comp_i;
        candidate_ptr->comp_group_idx = 0;
        candidate_ptr->compound_idx = (comp_i == COMPOUND_AVERAGE) ? 1 : 0;

        EbPictureBufferDesc   *src_pic = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
        uint8_t               *src_buf = src_pic->buffer_y + (context_ptr->cu_origin_x + src_pic->origin_x) + (context_ptr->cu_origin_y + src_pic->origin_y) * src_pic->stride_y;

        uint32_t  bwidth = context_ptr->blk_geom->bwidth;
        uint32_t  bheight = context_ptr->blk_geom->bheight;
        EbPictureBufferDesc  pred_desc;
        pred_desc.origin_x = pred_desc.origin_y = 0;
        pred_desc.stride_y = bwidth;
        pred_desc.buffer_y = context_ptr->pred0;

        SequenceControlSet* sequence_control_set_ptr = ((SequenceControlSet*)(picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr));
        EbPictureBufferDesc  *ref_pic_list0;
        EbPictureBufferDesc  *ref_pic_list1 = NULL;
        Mv mv_0;
        Mv mv_1;
        mv_0.x = candidate_ptr->motion_vector_xl0;
        mv_0.y = candidate_ptr->motion_vector_yl0;
        mv_1.x = candidate_ptr->motion_vector_xl1;
        mv_1.y = candidate_ptr->motion_vector_yl1;
        MvUnit mv_unit;
        mv_unit.mv[0] = mv_0;
        mv_unit.mv[1] = mv_1;
        mv_unit.pred_direction = BI_PRED;
        int8_t ref_idx_l0 = candidate_ptr->ref_frame_index_l0;
        int8_t ref_idx_l1 = candidate_ptr->ref_frame_index_l1;
        MvReferenceFrame rf[2];
        av1_set_ref_frame(rf, candidate_ptr->ref_frame_type);
        uint8_t list_idx0, list_idx1;
        list_idx0 = get_list_idx(rf[0]);
        if (rf[1] == NONE_FRAME)
            list_idx1 = get_list_idx(rf[0]);
        else
            list_idx1 = get_list_idx(rf[1]);
        assert(list_idx0 < MAX_NUM_OF_REF_PIC_LIST);
        assert(list_idx1 < MAX_NUM_OF_REF_PIC_LIST);
        if (ref_idx_l0 >= 0)
            ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr)->reference_picture;
        else
            ref_pic_list0 = (EbPictureBufferDesc*)EB_NULL;
        if (ref_idx_l1 >= 0)
            ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]->object_ptr)->reference_picture;
        else
            ref_pic_list1 = (EbPictureBufferDesc*)EB_NULL;


        av1_inter_prediction(
            picture_control_set_ptr,
            0,//fixed interpolation filter for compound search
            context_ptr->cu_ptr,
            candidate_ptr->ref_frame_type,
            &mv_unit,
            0,//use_intrabc,
#if COMP_MODE
            candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
            &candidate_ptr->interinter_comp,
#endif
#if II_ED
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            0,
            0,
            0,
#endif
            context_ptr->cu_origin_x,
            context_ptr->cu_origin_y,
            bwidth,
            bheight,
            ref_pic_list0,
            ref_pic_list1,
            &pred_desc, //output
            0,          //output origin_x,
            0,          //output origin_y,
            0,//do chroma
            sequence_control_set_ptr->encode_context_ptr->asm_type);

        int32_t est_rate;
        int64_t est_dist;

        model_rd_for_sb_with_curvfit (picture_control_set_ptr /*cpi*/, context_ptr,context_ptr->blk_geom->bsize, bwidth, bheight,
            src_buf, src_pic->stride_y, pred_desc.buffer_y, pred_desc.stride_y,
            /*x, xd*/ 0, 0, 0, 0, &est_rate/*[COMPOUND_AVERAGE]*/,
            &est_dist/*[COMPOUND_AVERAGE]*/, NULL, NULL, NULL, NULL, NULL);

        est_rate += candidate_ptr->md_rate_estimation_ptr->comp_idx_fac_bits[comp_index_ctx][candidate_ptr->compound_idx];

        est_rd[comp_i] =
            RDCOST(context_ptr->full_lambda /*x->rdmult*/, est_rate/*[COMPOUND_AVERAGE]*//* + *rate_mv*/,
                est_dist/*[COMPOUND_AVERAGE]*/);

    }


    //assign the best compound type
    if (est_rd[COMPOUND_AVERAGE] <= est_rd[COMPOUND_DISTWTD]) {
        candidate_ptr->interinter_comp.type = COMPOUND_AVERAGE;
        candidate_ptr->comp_group_idx = 0;
        candidate_ptr->compound_idx = 1;
    }
    else {
        candidate_ptr->interinter_comp.type = COMPOUND_DISTWTD;
        candidate_ptr->comp_group_idx = 0;
        candidate_ptr->compound_idx = 0;
    }



}
#endif

#if II_COMP
// Blending with alpha mask. Mask values come from the range [0, 64],
// as described for AOM_BLEND_A64 in aom_dsp/blend.h. src0 or src1 can
// be the same as dst, or dst can be different from both sources.

void aom_blend_a64_mask_c(uint8_t *dst, uint32_t dst_stride,
    const uint8_t *src0, uint32_t src0_stride,
    const uint8_t *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int w,
    int h, int subw, int subh) {
    int i, j;

    assert(IMPLIES(src0 == dst, src0_stride == dst_stride));
    assert(IMPLIES(src1 == dst, src1_stride == dst_stride));

    assert(h >= 1);
    assert(w >= 1);
    assert(IS_POWER_OF_TWO(h));
    assert(IS_POWER_OF_TWO(w));

    if (subw == 0 && subh == 0) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                const int m = mask[i * mask_stride + j];
                dst[i * dst_stride + j] = AOM_BLEND_A64(m, src0[i * src0_stride + j],
                    src1[i * src1_stride + j]);
            }
        }
    }
    else if (subw == 1 && subh == 1) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                const int m = ROUND_POWER_OF_TWO(
                    mask[(2 * i) * mask_stride + (2 * j)] +
                    mask[(2 * i + 1) * mask_stride + (2 * j)] +
                    mask[(2 * i) * mask_stride + (2 * j + 1)] +
                    mask[(2 * i + 1) * mask_stride + (2 * j + 1)],
                    2);
                dst[i * dst_stride + j] = AOM_BLEND_A64(m, src0[i * src0_stride + j],
                    src1[i * src1_stride + j]);
            }
        }
    }
    else if (subw == 1 && subh == 0) {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                const int m = AOM_BLEND_AVG(mask[i * mask_stride + (2 * j)],
                    mask[i * mask_stride + (2 * j + 1)]);
                dst[i * dst_stride + j] = AOM_BLEND_A64(m, src0[i * src0_stride + j],
                    src1[i * src1_stride + j]);
            }
        }
    }
    else {
        for (i = 0; i < h; ++i) {
            for (j = 0; j < w; ++j) {
                const int m = AOM_BLEND_AVG(mask[(2 * i) * mask_stride + j],
                    mask[(2 * i + 1) * mask_stride + j]);
                dst[i * dst_stride + j] = AOM_BLEND_A64(m, src0[i * src0_stride + j],
                    src1[i * src1_stride + j]);
            }
        }
    }
}

int is_interintra_wedge_used(BLOCK_SIZE sb_type);

 void combine_interintra(INTERINTRA_MODE mode,
    int8_t use_wedge_interintra, int wedge_index,
    int wedge_sign, BLOCK_SIZE bsize,
    BLOCK_SIZE plane_bsize, uint8_t *comppred,
    int compstride, const uint8_t *interpred,
    int interstride, const uint8_t *intrapred,
    int intrastride)
 {
    const int bw = block_size_wide[plane_bsize];
    const int bh = block_size_high[plane_bsize];

    if (use_wedge_interintra) {
        if (is_interintra_wedge_used(bsize)) {
            const uint8_t *mask =
                av1_get_contiguous_soft_mask(wedge_index, wedge_sign, bsize);
            const int subw = 2 * mi_size_wide[bsize] == bw;
            const int subh = 2 * mi_size_high[bsize] == bh;
            aom_blend_a64_mask/*_c*/(comppred, compstride, intrapred, intrastride,     ////------------------------------ASM
                interpred, interstride, mask, block_size_wide[bsize],
                bw, bh, subw, subh);
        }
        return;
    }
    else {
        /* assert(0 && "TBD_smooth");*/

        uint8_t mask[MAX_SB_SQUARE];
        build_smooth_interintra_mask(mask, bw, plane_bsize, mode);
        aom_blend_a64_mask/*_c*/(comppred, compstride, intrapred, intrastride, interpred, ////------------------------------ASM
            interstride, mask, bw, bw, bh, 0, 0);
    }
}
#endif
#if II_ED
 extern void av1_predict_intra_block(
    TileInfo * tile,
    STAGE       stage,
    const BlockGeom            * blk_geom,
    const Av1Common *cm,
    int32_t wpx,
    int32_t hpx,
    TxSize tx_size,
    PredictionMode mode,
    int32_t angle_delta,
    int32_t use_palette,
    FilterIntraMode filter_intra_mode,
    uint8_t* topNeighArray,
    uint8_t* leftNeighArray,
    EbPictureBufferDesc  *recon_buffer,
    int32_t col_off,
    int32_t row_off,
    int32_t plane,
    BlockSize bsize,
#if ATB_EP
    uint32_t tu_org_x_pict,
    uint32_t tu_org_y_pict,
#endif
    uint32_t bl_org_x_pict,
    uint32_t bl_org_y_pict,
    uint32_t bl_org_x_mb,
    uint32_t bl_org_y_mb);
 #define INTERINTRA_WEDGE_SIGN 0
 // Mapping of interintra to intra mode for use in the intra component
static const PredictionMode interintra_to_intra_mode[INTERINTRA_MODES] = {
  DC_PRED, V_PRED, H_PRED, SMOOTH_PRED
};
#endif
EbErrorType av1_inter_prediction(
    PictureControlSet                    *picture_control_set_ptr,
    uint32_t                                interp_filters,
    CodingUnit                           *cu_ptr,
    uint8_t                                 ref_frame_type,
    MvUnit                               *mv_unit,
    uint8_t                                  use_intrabc,
#if COMP_MODE
    uint8_t                                compound_idx,
#endif
#if COMP_DIFF
    INTERINTER_COMPOUND_DATA               *interinter_comp,
#endif
#if II_ED
    TileInfo                                * tile,
    NeighborArrayUnit                       *luma_recon_neighbor_array,
    NeighborArrayUnit                       *cb_recon_neighbor_array ,
    NeighborArrayUnit                       *cr_recon_neighbor_array ,
    uint8_t                                 is_interintra_used ,
    INTERINTRA_MODE                        interintra_mode,
    uint8_t                                use_wedge_interintra,
    int32_t                                interintra_wedge_index,

#endif
    uint16_t                                pu_origin_x,
    uint16_t                                pu_origin_y,
    uint8_t                                 bwidth,
    uint8_t                                 bheight,
    EbPictureBufferDesc                  *ref_pic_list0,
    EbPictureBufferDesc                  *ref_pic_list1,
    EbPictureBufferDesc                  *prediction_ptr,
    uint16_t                                dst_origin_x,
    uint16_t                                dst_origin_y,
    EbBool                                  perform_chroma,
    EbAsm                                   asm_type)
{
    (void)asm_type;
    EbErrorType  return_error = EB_ErrorNone;
    uint8_t         is_compound = (mv_unit->pred_direction == BI_PRED) ? 1 : 0;
    DECLARE_ALIGNED(32, uint16_t, tmp_dstY[128 * 128]);//move this to context if stack does not hold.
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);

    MV  mv, mv_q4;

    int32_t subpel_x, subpel_y;
    uint8_t * src_ptr;
    uint8_t * dst_ptr;
    int32_t src_stride;
    int32_t dst_stride;
    ConvolveParams conv_params;

    InterpFilterParams filter_params_x, filter_params_y;

    const BlockGeom * blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);

    //special treatment for chroma in 4XN/NX4 blocks
    //if one of the neighbour blocks of the parent square is intra the chroma prediction will follow the normal path using the luma MV of the current nsq block which is the latest sub8x8.
    //for this case: only uniPred is allowed.

    int32_t sub8x8_inter = 0;
    if(perform_chroma && (blk_geom->has_uv && (blk_geom->bwidth == 4 || blk_geom->bheight == 4)))

    {
        //CHKN setup input param

        int32_t bw = blk_geom->bwidth_uv;
        int32_t bh = blk_geom->bheight_uv;
        UNUSED_VARIABLE(bw);
        UNUSED_VARIABLE(bh);

        uint32_t mi_x = pu_origin_x;       //these are luma picture wise
        uint32_t mi_y = pu_origin_y;

        MacroBlockD  *xd = cu_ptr->av1xd;
#if INCOMPLETE_SB_FIX
        xd->mi_stride = picture_control_set_ptr->mi_stride;
#else
        xd->mi_stride = picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->picture_width_in_sb*(BLOCK_SIZE_64 / 4);
#endif
        const int32_t offset = (mi_y >> MI_SIZE_LOG2) * xd->mi_stride + (mi_x >> MI_SIZE_LOG2);
        xd->mi = picture_control_set_ptr->mi_grid_base + offset;

        //CHKN fill current mi from current block
        {
            ModeInfo *miPtr = *xd->mi;
            uint8_t  miX, miY;
            MvReferenceFrame rf[2];
            av1_set_ref_frame(rf, ref_frame_type);
            for (miY = 0; miY < (blk_geom->bheight >> MI_SIZE_LOG2); miY++) {
                for (miX = 0; miX < (blk_geom->bwidth >> MI_SIZE_LOG2); miX++) {
                    miPtr[miX + miY * xd->mi_stride].mbmi.use_intrabc = use_intrabc;
                    miPtr[miX + miY * xd->mi_stride].mbmi.ref_frame[0] = rf[0];
                    if (mv_unit->pred_direction == UNI_PRED_LIST_0) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                    }
                    else if (mv_unit->pred_direction == UNI_PRED_LIST_1) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                    else {
                        // printf("ERRRRRRR");

                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                }
            }
        }

        int32_t build_for_obmc = 0;

        const BlockSize bsize = blk_geom->bsize;//mi->sb_type;
        assert(bsize < BlockSizeS_ALL);
        const int32_t ss_x = 1;// pd->subsampling_x;
        const int32_t ss_y = 1;//pd->subsampling_y;
        sub8x8_inter = (block_size_wide[bsize] < 8 && ss_x) ||
            (block_size_high[bsize] < 8 && ss_y);

        if (use_intrabc) sub8x8_inter = 0;

        // For sub8x8 chroma blocks, we may be covering more than one luma block's
        // worth of pixels. Thus (mi_x, mi_y) may not be the correct coordinates for
        // the top-left corner of the prediction source - the correct top-left corner
        // is at (pre_x, pre_y).
        const int32_t row_start =
            (block_size_high[bsize] == 4) && ss_y && !build_for_obmc ? -1 : 0;
        const int32_t col_start =
            (block_size_wide[bsize] == 4) && ss_x && !build_for_obmc ? -1 : 0;

        const int32_t pre_x = (mi_x + MI_SIZE * col_start) >> ss_x;
        const int32_t pre_y = (mi_y + MI_SIZE * row_start) >> ss_y;
        UNUSED_VARIABLE(pre_x);
        UNUSED_VARIABLE(pre_y);

        sub8x8_inter = sub8x8_inter && !build_for_obmc;
        if (sub8x8_inter) {
            for (int32_t row = row_start; row <= 0 && sub8x8_inter; ++row) {
                for (int32_t col = col_start; col <= 0; ++col) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    if (!is_inter_block(this_mbmi)) sub8x8_inter = 0;
                    //if (is_intrabc_block(this_mbmi)) sub8x8_inter = 0;
                }
            }
        }

        if (sub8x8_inter) {
            // block size
            const int32_t b4_w = block_size_wide[bsize] >> ss_x;
            const int32_t b4_h = block_size_high[bsize] >> ss_y;
            const BlockSize plane_bsize = scale_chroma_bsize(bsize, ss_x, ss_y);
            assert(plane_bsize < BlockSizeS_ALL);
            const int32_t b8_w = block_size_wide[plane_bsize] >> ss_x;
            const int32_t b8_h = block_size_high[plane_bsize] >> ss_y;

            assert(!is_compound);

            if (is_compound)
                printf("ETTTT");

            //const struct Buf2d orig_pred_buf[2] = { pd->pre[0], pd->pre[1] };

            int32_t row = row_start;
            int32_t src_stride;
            for (int32_t y = 0; y < b8_h; y += b4_h) {
                int32_t col = col_start;
                for (int32_t x = 0; x < b8_w; x += b4_w) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    // MbModeInfo *this_mbmi = xd->mi[row * xd->mi_stride + col];
                     //is_compound = has_second_ref(this_mbmi);
                    int32_t tmp_dst_stride = 8;
                    UNUSED_VARIABLE(tmp_dst_stride);
                    assert(bw < 8 || bh < 8);

                    // ConvolveParams conv_params = get_conv_params_no_round(
                     //    0, plane, xd->tmp_conv_dst, tmp_dst_stride, is_compound, xd->bd);
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, BLOCK_SIZE_64, is_compound, EB_8BIT);
                    conv_params.use_jnt_comp_avg = 0;
#if MCP_4XN_FIX
                    uint8_t ref_idx = get_ref_frame_idx(this_mbmi->ref_frame[0]);
                    assert(ref_idx < REF_LIST_MAX_DEPTH);
                    EbPictureBufferDesc  *ref_pic = this_mbmi->ref_frame[0] ==
                        LAST_FRAME || this_mbmi->ref_frame[0] == LAST2_FRAME || this_mbmi->ref_frame[0] == LAST3_FRAME || this_mbmi->ref_frame[0] == GOLDEN_FRAME ?
                        ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_0][ref_idx]->object_ptr)->reference_picture :
                        ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_1][ref_idx]->object_ptr)->reference_picture;
#else
                    EbPictureBufferDesc                  *ref_pic = this_mbmi->ref_frame[0] == LAST_FRAME ? ref_pic_list0 : ref_pic_list1;
#endif
                    assert(ref_pic != NULL);
                    src_ptr = ref_pic->buffer_cb + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cb;
                    dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
                    src_stride = ref_pic->stride_cb;
                    dst_stride = prediction_ptr->stride_cb;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cb;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cb;

                    const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);

                    av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);


                    //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
                    //    printf("to do -> to do");

                    convolve[subpel_x != 0][subpel_y != 0][is_compound](
                        src_ptr,
                        src_stride,
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        subpel_x,
                        subpel_y,
                        &conv_params);

                    //Cr
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, BLOCK_SIZE_64, is_compound, EB_8BIT);
                    conv_params.use_jnt_comp_avg = 0;

                    src_ptr = ref_pic->buffer_cr + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cr;
                    dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;

                    src_stride = ref_pic->stride_cr;
                    dst_stride = prediction_ptr->stride_cr;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cr;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cr;

                    // const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);

                    av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);
                    //
                    //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
                    //    printf("to do -> to do");

                    convolve[subpel_x != 0][subpel_y != 0][is_compound](
                        src_ptr,
                        src_stride,
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        subpel_x,
                        subpel_y,
                        &conv_params);

                    ++col;
                }
                ++row;
            }

            //for (ref = 0; ref < 2; ++ref) pd->pre[ref] = orig_pred_buf[ref];

            //return;
        }
    }

#if COMP_MODE
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, ref_frame_type);
#endif
    if (mv_unit->pred_direction == UNI_PRED_LIST_0 || mv_unit->pred_direction == BI_PRED) {
        //List0-Y
        mv.col = mv_unit->mv[REF_LIST_0].x;
        mv.row = mv_unit->mv[REF_LIST_0].y;
        assert(ref_pic_list0 != NULL);
        src_ptr = ref_pic_list0->buffer_y + ref_pic_list0->origin_x + pu_origin_x + (ref_pic_list0->origin_y + pu_origin_y) * ref_pic_list0->stride_y;
        dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list0->stride_y;
        dst_stride = prediction_ptr->stride_y;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;
        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstY, 128, is_compound, EB_8BIT);
        av1_get_convolve_filter_params(interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);

        //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
        //    printf("to do -> to do");

        convolve[subpel_x != 0][subpel_y != 0][is_compound](
            src_ptr,
            src_stride,
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,//av1RegularFilter,
            &filter_params_y,//av1RegularFilter,
            subpel_x,
            subpel_y,
            &conv_params);
        if (perform_chroma && blk_geom->has_uv && sub8x8_inter == 0) {
            //List0-Cb
            src_ptr = ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list0->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);

            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
            //    printf("to do -> to do");

            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                convolve_2d_for_intrabc((const uint8_t *)src_ptr, src_stride, dst_ptr, dst_stride, blk_geom->bwidth_uv, blk_geom->bheight_uv, subpel_x,
                    subpel_y, &conv_params);
            else
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                subpel_x,
                subpel_y,
                &conv_params);

            //List0-Cr
            src_ptr = ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list0->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);

            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                convolve_2d_for_intrabc((const uint8_t *)src_ptr, src_stride, dst_ptr, dst_stride, blk_geom->bwidth_uv, blk_geom->bheight_uv, subpel_x,
                    subpel_y, &conv_params);
            else
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                subpel_x,
                subpel_y,
                &conv_params);
        }
    }

    if ((mv_unit->pred_direction == UNI_PRED_LIST_1 || mv_unit->pred_direction == BI_PRED) ) {
        //List0-Y
        mv.col = mv_unit->mv[REF_LIST_1].x;
        mv.row = mv_unit->mv[REF_LIST_1].y;
        assert(ref_pic_list1 != NULL);
        src_ptr = ref_pic_list1->buffer_y + ref_pic_list1->origin_x + pu_origin_x + (ref_pic_list1->origin_y + pu_origin_y) * ref_pic_list1->stride_y;
        dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list1->stride_y;
        dst_stride = prediction_ptr->stride_y;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;

        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstY, 128, is_compound, EB_8BIT);
        av1_get_convolve_filter_params(interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);

        //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
        //    printf("to do -> to do");

#if COMP_MODE
        //the luma data is applied to chroma below
        av1_dist_wtd_comp_weight_assign(
            picture_control_set_ptr,
            picture_control_set_ptr->parent_pcs_ptr->cur_order_hint,// cur_frame_index,
            picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[0] - 1],// bck_frame_index,
            picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[1] - 1],// fwd_frame_index,
            compound_idx,
            0,// order_idx,
            &conv_params.fwd_offset, &conv_params.bck_offset,
            &conv_params.use_dist_wtd_comp_avg, is_compound);
        conv_params.use_jnt_comp_avg =  conv_params.use_dist_wtd_comp_avg;
#endif

#if COMP_DIFF
        if (is_compound && is_masked_compound_type(interinter_comp->type)) {
            conv_params.do_average = 0;
            av1_make_masked_inter_predictor(
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom,
                bwidth,
                bheight,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                interinter_comp,
                EB_8BIT,
                0//plane=Luma  seg_mask is computed based on luma and used for chroma
                );
        }
        else
#endif

        convolve[subpel_x != 0][subpel_y != 0][is_compound](
            src_ptr,
            src_stride,
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,//&av1RegularFilter,
            &filter_params_y,//&av1RegularFilter,
            subpel_x,
            subpel_y,
            &conv_params);
        if (perform_chroma && blk_geom->has_uv && sub8x8_inter == 0) {
            //List0-Cb
            src_ptr = ref_pic_list1->buffer_cb + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cb;
            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list1->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);
#if COMP_MODE
            av1_dist_wtd_comp_weight_assign(
                picture_control_set_ptr,
                picture_control_set_ptr->parent_pcs_ptr->cur_order_hint,// cur_frame_index,
                picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[0] - 1],// bck_frame_index,
                picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[1] - 1],// fwd_frame_index,
                compound_idx,
                0,// order_idx,
                &conv_params.fwd_offset, &conv_params.bck_offset,
                &conv_params.use_dist_wtd_comp_avg, is_compound);
            conv_params.use_jnt_comp_avg = conv_params.use_dist_wtd_comp_avg;
#endif
            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            //if (filter_params_x.interp_filter != 3 || filter_params_y.interp_filter != 3)
            //    printf("to do -> to do");
#if COMP_DIFF
            if (is_compound && is_masked_compound_type(interinter_comp->type)) {
                conv_params.do_average = 0;
                av1_make_masked_inter_predictor(
                    src_ptr,
                    src_stride,
                    dst_ptr,
                    dst_stride,
                    blk_geom,
                    blk_geom->bwidth_uv,
                    blk_geom->bheight_uv,
                    &filter_params_x,
                    &filter_params_y,
                    subpel_x,
                    subpel_y,
                    &conv_params,
                    interinter_comp,
                    EB_8BIT,
                    1//plane=cb  seg_mask is computed based on luma and used for chroma
                );
            }
            else
#endif
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                subpel_x,
                subpel_y,
                &conv_params);

            //List0-Cr
            src_ptr = ref_pic_list1->buffer_cr + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cr;
            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list1->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);
#if COMP_MODE
            av1_dist_wtd_comp_weight_assign(
                picture_control_set_ptr,
                picture_control_set_ptr->parent_pcs_ptr->cur_order_hint,// cur_frame_index,
                picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[0] - 1],// bck_frame_index,
                picture_control_set_ptr->parent_pcs_ptr->ref_order_hint[rf[1] - 1],// fwd_frame_index,
                compound_idx,
                0,// order_idx,
                &conv_params.fwd_offset, &conv_params.bck_offset,
                &conv_params.use_dist_wtd_comp_avg, is_compound);
            conv_params.use_jnt_comp_avg = conv_params.use_dist_wtd_comp_avg;
#endif

#if COMP_DIFF
            if (is_compound && is_masked_compound_type(interinter_comp->type)) {
                conv_params.do_average = 0;
                av1_make_masked_inter_predictor(
                    src_ptr,
                    src_stride,
                    dst_ptr,
                    dst_stride,
                    blk_geom,
                    blk_geom->bwidth_uv,
                    blk_geom->bheight_uv,
                    &filter_params_x,
                    &filter_params_y,
                    subpel_x,
                    subpel_y,
                    &conv_params,
                    interinter_comp,
                    EB_8BIT,
                    1//plane=Cr  seg_mask is computed based on luma and used for chroma
                );
            }
            else
#endif
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                subpel_x,
                subpel_y,
                &conv_params);
        }
    }

#if II_ED
    if ( is_interintra_used ) {
        int32_t start_plane = 0;
        int32_t end_plane = blk_geom->has_uv ? MAX_MB_PLANE: 1;
            // temp buffer for intra pred
            DECLARE_ALIGNED(16, uint8_t, intra_pred[MAX_SB_SQUARE]);
            DECLARE_ALIGNED(16, uint8_t, intra_pred_cb[MAX_SB_SQUARE]);
            DECLARE_ALIGNED(16, uint8_t, intra_pred_cr[MAX_SB_SQUARE]);

        int32_t  intra_stride;

        for (int32_t plane = start_plane; plane < end_plane; ++plane) {

            EbPictureBufferDesc  inra_pred_desc;
            inra_pred_desc.origin_x     = inra_pred_desc.origin_y  = 0;
            inra_pred_desc.stride_y     = bwidth;
            inra_pred_desc.stride_cb    = bwidth/2;
            inra_pred_desc.stride_cr    = bwidth/2;
            inra_pred_desc.buffer_y     = intra_pred;
            inra_pred_desc.buffer_cb    = intra_pred_cb;
            inra_pred_desc.buffer_cr    = intra_pred_cr;

            const int ssx = plane ? 1 : 0;
            const int ssy = plane ? 1 : 0;
            const BLOCK_SIZE plane_bsize = get_plane_block_size(blk_geom->bsize, ssx, ssy);
            //av1_build_interintra_predictors_sbp
            uint8_t    topNeighArray[64 * 2 + 1];
            uint8_t    leftNeighArray[64 * 2 + 1];

            uint32_t cu_originx_uv = (dst_origin_x >> 3 << 3) >> 1;
            uint32_t cu_originy_uv = (dst_origin_y >> 3 << 3) >> 1;

            if (plane == 0) {
                dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
                dst_stride = prediction_ptr->stride_y;
                intra_stride = inra_pred_desc.stride_y;
                if (dst_origin_y != 0)
                    memcpy(topNeighArray + 1, luma_recon_neighbor_array->top_array + dst_origin_x,  blk_geom->bwidth * 2);
                if (dst_origin_x != 0)
                    memcpy(leftNeighArray + 1, luma_recon_neighbor_array->left_array + dst_origin_y, blk_geom->bheight * 2);
                if (dst_origin_y != 0 && dst_origin_x != 0)
                    topNeighArray[0] = leftNeighArray[0] = luma_recon_neighbor_array->top_left_array[MAX_PICTURE_HEIGHT_SIZE + dst_origin_x - dst_origin_y];
            }

            else if (plane == 1) {
                dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
                 dst_stride = prediction_ptr->stride_cb;
                intra_stride = inra_pred_desc.stride_cb;
                if (cu_originy_uv != 0)
                    memcpy(topNeighArray + 1, cb_recon_neighbor_array->top_array + cu_originx_uv, blk_geom->bwidth_uv * 2);

                if (cu_originx_uv != 0)

                    memcpy(leftNeighArray + 1, cb_recon_neighbor_array->left_array + cu_originy_uv, blk_geom->bheight_uv * 2);

                if (cu_originy_uv != 0 && cu_originx_uv != 0)
                    topNeighArray[0] = leftNeighArray[0] = cb_recon_neighbor_array->top_left_array[MAX_PICTURE_HEIGHT_SIZE / 2 + cu_originx_uv - cu_originy_uv / 2];
            }
            else {
                dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
                 dst_stride = prediction_ptr->stride_cr;
                 intra_stride = inra_pred_desc.stride_cr;
                if (cu_originy_uv != 0)

                    memcpy(topNeighArray + 1, cr_recon_neighbor_array->top_array + cu_originx_uv, blk_geom->bwidth_uv * 2);

                if (cu_originx_uv != 0)

                    memcpy(leftNeighArray + 1, cr_recon_neighbor_array->left_array + cu_originy_uv, blk_geom->bheight_uv * 2);

                if (cu_originy_uv != 0 && cu_originx_uv != 0)
                    topNeighArray[0] = leftNeighArray[0] = cr_recon_neighbor_array->top_left_array[MAX_PICTURE_HEIGHT_SIZE / 2 + cu_originx_uv - cu_originy_uv / 2];
            }
            TxSize  tx_size = blk_geom->txsize[0][0]; // Nader - Intra 128x128 not supported
            TxSize  tx_size_Chroma = blk_geom->txsize_uv[0][0]; //Nader - Intra 128x128 not supported

            av1_predict_intra_block(
                tile,
#if MDLEVELS
                !ED_STAGE,
#else
                MD_STAGE,
#endif
                blk_geom,
                picture_control_set_ptr->parent_pcs_ptr->av1_cm,                                    //const Av1Common *cm,
                plane ? blk_geom->bwidth_uv : blk_geom->bwidth,     //int32_t wpx,
                plane ? blk_geom->bheight_uv : blk_geom->bheight,   //int32_t hpx,
                plane ? tx_size_Chroma : tx_size,                                                   //TxSize tx_size,
                interintra_to_intra_mode[interintra_mode],                                  //PredictionMode mode,
#if SEARCH_UV_MODE // conformance
                0,
#else
                plane ? 0 : candidate_buffer_ptr->candidate_ptr->angle_delta[PLANE_TYPE_Y],         //int32_t angle_delta,
#endif
                0,                                                                              //int32_t use_palette,
                FILTER_INTRA_MODES,                                                             //CHKN FilterIntraMode filter_intra_mode,
                topNeighArray + 1,
                leftNeighArray + 1,
                &inra_pred_desc,                                              //uint8_t *dst,
                                                                                                //int32_t dst_stride,
                0,                                                                              //int32_t col_off,
                0,                                                                              //int32_t row_off,
                plane,                                                                          //int32_t plane,
                blk_geom->bsize,       //uint32_t puSize,
#if ATB_EP
                dst_origin_x,
                dst_origin_y,
#endif
                dst_origin_x,                  //uint32_t cuOrgX,
                dst_origin_y,                  //uint32_t cuOrgY
                0,  //uint32_t cuOrgX used only for prediction Ptr
                0   //uint32_t cuOrgY used only for prediction Ptr
            );

            //combine_interintra
            combine_interintra(
                interintra_mode,//xd->mi[0]->interintra_mode,
                use_wedge_interintra,//xd->mi[0]->use_wedge_interintra,
                interintra_wedge_index,//xd->mi[0]->interintra_wedge_index,
                INTERINTRA_WEDGE_SIGN,
                blk_geom->bsize,//bsize,
                plane_bsize,
                dst_ptr,//xd->plane[plane].dst.buf,
                dst_stride,//xd->plane[plane].dst.stride,
                dst_ptr,//inter_pred,
                dst_stride,//inter_stride,
                (plane == 0) ? intra_pred : (plane == 1) ? intra_pred_cb : intra_pred_cr,//intra_pred,
                intra_stride);//intra_stride);

        }
    }

#endif
    //if (is_interintra_pred(xd->mi[0])) {
    //    BUFFER_SET default_ctx = { { NULL, NULL, NULL }, { 0, 0, 0 } };
    //    if (!ctx) {
    //        default_ctx.plane[plane_idx] = xd->plane[plane_idx].dst.buf;
    //        default_ctx.stride[plane_idx] = xd->plane[plane_idx].dst.stride;
    //        ctx = &default_ctx;
    //    }
    //    av1_build_interintra_predictors_sbp(cm, xd, xd->plane[plane_idx].dst.buf,
    //        xd->plane[plane_idx].dst.stride, ctx,
    //        plane_idx, bsize);
    //av1_predict_intra_block
  /*  DECLARE_ALIGNED(16, uint8_t, intrapredictor[MAX_SB_SQUARE]);
    av1_build_intra_predictors_for_interintra(cm, xd, bsize, plane, ctx,
        intrapredictor, MAX_SB_SIZE);
  combine_interintra(
      xd->mi[0]->interintra_mode, xd->mi[0]->use_wedge_interintra,
      xd->mi[0]->interintra_wedge_index, INTERINTRA_WEDGE_SIGN, bsize,
      plane_bsize, xd->plane[plane].dst.buf, xd->plane[plane].dst.stride,
      inter_pred, inter_stride, intra_pred, intra_stride);

    //}
    */
    return return_error;
}

#if !UNPACK_REF_POST_EP
/***************************************************
*  PreLoad Reference Block  for 16bit mode
***************************************************/
void Av1UnPackReferenceBlock(
    uint16_t                *SrcBuffer,
    uint32_t                 SrcStride,
    uint32_t                 pu_width,
    uint32_t                 pu_height,
    uint8_t                  *eightBitBuffer,
    uint32_t                 eightBitStride,
    EbBool                sub_pred,
    EbAsm                 asm_type,
    uint8_t                  Tap)
{
    pu_width += Tap;
    pu_height += Tap;
    uint16_t *ptr16 = (uint16_t *)SrcBuffer - (Tap >> 1) - ((Tap >> 1)*SrcStride);

    extract8_bitdata_safe_sub(
        ptr16,
        SrcStride << sub_pred,
        eightBitBuffer,
        eightBitStride << sub_pred,
        pu_width,
        pu_height >> sub_pred,
        sub_pred,
        asm_type
    );
}
#endif

#if !REMOVE_UNPACK_REF
EbErrorType AV1InterPrediction10BitMD(
    uint32_t                                interp_filters,
    PictureControlSet                     *picture_control_set_ptr,
    uint8_t                                  ref_frame_type,
    ModeDecisionContext                   *md_context_ptr,
    CodingUnit                            *cu_ptr,
    MvUnit                                *mv_unit,
    uint8_t                                 use_intrabc,
    uint16_t                                 pu_origin_x,
    uint16_t                                 pu_origin_y,
    uint8_t                                  bwidth,
    uint8_t                                  bheight,
    EbPictureBufferDesc                   *ref_pic_list0,
    EbPictureBufferDesc                   *ref_pic_list1,
    EbPictureBufferDesc                   *prediction_ptr,
    uint16_t                                 dst_origin_x,
    uint16_t                                 dst_origin_y,
    EbBool                                  perform_chroma,
    EbAsm                                    asm_type)
{
    EbErrorType  return_error = EB_ErrorNone;
    InterPredictionContext *context_ptr = (InterPredictionContext*)(md_context_ptr->inter_prediction_context);
    uint8_t         is_compound = (mv_unit->pred_direction == BI_PRED) ? 1 : 0;
    DECLARE_ALIGNED(32, uint16_t, tmp_dstY[128 * 128]);//move this to context if stack does not hold.
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);

    MV  mv, mv_q4;

    int32_t subpel_x, subpel_y;
#if UNPACK_REF_POST_EP
    uint8_t * src_ptr;
#else
    uint16_t * src_ptr;
#endif
    uint8_t * dst_ptr;
    int32_t src_stride;
    int32_t dst_stride;
    ConvolveParams conv_params;
    InterpFilterParams filter_params_x, filter_params_y;

    const BlockGeom * blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);

    //special treatment for chroma in 4XN/NX4 blocks
   //if one of the neighbour blocks of the parent square is intra the chroma prediction will follow the normal path using the luma MV of the current nsq block which is the latest sub8x8.
   //for this case: only uniPred is allowed.

    int32_t sub8x8_inter = 0;
    if (perform_chroma && blk_geom->has_uv && (blk_geom->bwidth == 4 || blk_geom->bheight == 4))
    {
        //CHKN setup input param

        int32_t bw = blk_geom->bwidth_uv;
        int32_t bh = blk_geom->bheight_uv;
        UNUSED_VARIABLE(bw);
        UNUSED_VARIABLE(bh);

        uint32_t mi_x = pu_origin_x;       //these are luma picture wise
        uint32_t mi_y = pu_origin_y;

        MacroBlockD  *xd = cu_ptr->av1xd;
        xd->mi_stride = picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->picture_width_in_sb*(BLOCK_SIZE_64 / 4);
        const int32_t offset = (mi_y >> MI_SIZE_LOG2) * xd->mi_stride + (mi_x >> MI_SIZE_LOG2);
        xd->mi = picture_control_set_ptr->mi_grid_base + offset;

        //CHKN fill current mi from current block
        {
            ModeInfo *miPtr = *xd->mi;
            uint8_t  miX, miY;
            MvReferenceFrame rf[2];
            av1_set_ref_frame(rf, ref_frame_type);
            for (miY = 0; miY < (blk_geom->bheight >> MI_SIZE_LOG2); miY++) {
                for (miX = 0; miX < (blk_geom->bwidth >> MI_SIZE_LOG2); miX++) {
                    miPtr[miX + miY * xd->mi_stride].mbmi.use_intrabc = use_intrabc;
                    miPtr[miX + miY * xd->mi_stride].mbmi.ref_frame[0] = rf[0];
                    if (mv_unit->pred_direction == UNI_PRED_LIST_0) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                    }
                    else if (mv_unit->pred_direction == UNI_PRED_LIST_1) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                    else {
                        // printf("ERRRRRRR");

                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                }
            }
        }

        int32_t build_for_obmc = 0;

        const BlockSize bsize = blk_geom->bsize;//mi->sb_type;
        assert(bsize < BlockSizeS_ALL);
        const int32_t ss_x = 1;// pd->subsampling_x;
        const int32_t ss_y = 1;//pd->subsampling_y;
        sub8x8_inter = (block_size_wide[bsize] < 8 && ss_x) ||
            (block_size_high[bsize] < 8 && ss_y);

        if (use_intrabc) sub8x8_inter = 0;
        // For sub8x8 chroma blocks, we may be covering more than one luma block's
        // worth of pixels. Thus (mi_x, mi_y) may not be the correct coordinates for
        // the top-left corner of the prediction source - the correct top-left corner
        // is at (pre_x, pre_y).
        const int32_t row_start =
            (block_size_high[bsize] == 4) && ss_y && !build_for_obmc ? -1 : 0;
        const int32_t col_start =
            (block_size_wide[bsize] == 4) && ss_x && !build_for_obmc ? -1 : 0;

        const int32_t pre_x = (mi_x + MI_SIZE * col_start) >> ss_x;
        const int32_t pre_y = (mi_y + MI_SIZE * row_start) >> ss_y;
        UNUSED_VARIABLE(pre_x);
        UNUSED_VARIABLE(pre_y);

        sub8x8_inter = sub8x8_inter && !build_for_obmc;
        if (sub8x8_inter) {
            for (int32_t row = row_start; row <= 0 && sub8x8_inter; ++row) {
                for (int32_t col = col_start; col <= 0; ++col) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    if (!is_inter_block(this_mbmi)) sub8x8_inter = 0;
                    //if (is_intrabc_block(this_mbmi)) sub8x8_inter = 0;
                }
            }
        }

        if (sub8x8_inter) {
            // block size
            const int32_t b4_w = block_size_wide[bsize] >> ss_x;
            const int32_t b4_h = block_size_high[bsize] >> ss_y;
            const BlockSize plane_bsize = scale_chroma_bsize(bsize, ss_x, ss_y);
            assert(plane_bsize < BlockSizeS_ALL);
            const int32_t b8_w = block_size_wide[plane_bsize] >> ss_x;
            const int32_t b8_h = block_size_high[plane_bsize] >> ss_y;

            assert(!is_compound);
            if (is_compound)
                printf("ETTTT");

            //const struct Buf2d orig_pred_buf[2] = { pd->pre[0], pd->pre[1] };

            int32_t row = row_start;
            int32_t src_stride;
            for (int32_t y = 0; y < b8_h; y += b4_h) {
                int32_t col = col_start;
                for (int32_t x = 0; x < b8_w; x += b4_w) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    // MbModeInfo *this_mbmi = xd->mi[row * xd->mi_stride + col];
                     //is_compound = has_second_ref(this_mbmi);
                    int32_t tmp_dst_stride = 8;
                    UNUSED_VARIABLE(tmp_dst_stride);
                    assert(bw < 8 || bh < 8);

                    // ConvolveParams conv_params = get_conv_params_no_round(
                     //    0, plane, xd->tmp_conv_dst, tmp_dst_stride, is_compound, xd->bd);
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, BLOCK_SIZE_64, is_compound, EB_8BIT);
                    conv_params.use_jnt_comp_avg = 0;

                    EbPictureBufferDesc                  *ref_pic = this_mbmi->ref_frame[0] == LAST_FRAME ? ref_pic_list0 : ref_pic_list1;
                    assert(ref_pic != NULL);
#if UNPACK_REF_POST_EP
                    src_ptr = ref_pic->buffer_cb + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cb;
#else
                    src_ptr = (uint16_t*)ref_pic->buffer_cb + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cb;
#endif
                    dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;

                    src_stride = ref_pic->stride_cb;
                    dst_stride = prediction_ptr->stride_cb;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cb;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cb;

                    const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
#if !UNPACK_REF_POST_EP
                    EbPictureBufferDesc                  *local_buffer = this_mbmi->ref_frame[0] == LAST_FRAME ? context_ptr->mcp_context->local_reference_block8_bitl0 : context_ptr->mcp_context->local_reference_block8_bitl1;
#endif
                    av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);
#if !UNPACK_REF_POST_EP
                    Av1UnPackReferenceBlock(
                        src_ptr,
                        src_stride,
                        b4_w,
                        b4_h,
                        local_buffer->buffer_cb,
                        local_buffer->stride_cb,
                        EB_FALSE,
                        asm_type,
                        8);
#endif
                    convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                        src_ptr,
                        src_stride,
#else
                        local_buffer->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cb),
                        local_buffer->stride_cb,
#endif
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,
                        &filter_params_y,
                        subpel_x,
                        subpel_y,
                        &conv_params);

                    //Cr
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, BLOCK_SIZE_64, is_compound, EB_8BIT);
                    conv_params.use_jnt_comp_avg = 0;
#if UNPACK_REF_POST_EP
                    src_ptr = ref_pic->buffer_cr + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cr;
#else
                    src_ptr = (uint16_t*)ref_pic->buffer_cr + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cr;
#endif
                    dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
                    src_stride = ref_pic->stride_cr;
                    dst_stride = prediction_ptr->stride_cr;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cr;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cr;

                    // const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);

                    av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);
#if !UNPACK_REF_POST_EP
                    //EbPictureBufferDesc                  *local_buffer = this_mbmi->ref_frame[0] == LAST_FRAME ? context_ptr->mcp_context->local_reference_block8_bitl0 : context_ptr->mcp_context->local_reference_block8_bitl1;

                    Av1UnPackReferenceBlock(
                        src_ptr,
                        src_stride,
                        b4_w,
                        b4_h,
                        local_buffer->buffer_cr,
                        local_buffer->stride_cr,
                        EB_FALSE,
                        asm_type,
                        8);
#endif
                    convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                        src_ptr,
                        src_stride,
#else
                        local_buffer->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr),
                        local_buffer->stride_cr,
#endif
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,
                        &filter_params_y,
                        subpel_x,
                        subpel_y,
                        &conv_params);

                    ++col;
                }
                ++row;
            }

            //for (ref = 0; ref < 2; ++ref) pd->pre[ref] = orig_pred_buf[ref];

            //return;
        }
    }

    if (mv_unit->pred_direction == UNI_PRED_LIST_0 || mv_unit->pred_direction == BI_PRED) {
        //List0-Y
        mv.col = mv_unit->mv[REF_LIST_0].x;
        mv.row = mv_unit->mv[REF_LIST_0].y;
#if UNPACK_REF_POST_EP
        src_ptr = ref_pic_list0->buffer_y + ref_pic_list0->origin_x + pu_origin_x + (ref_pic_list0->origin_y + pu_origin_y) * ref_pic_list0->stride_y;
#else
        src_ptr = (uint16_t*)ref_pic_list0->buffer_y + ref_pic_list0->origin_x + pu_origin_x + (ref_pic_list0->origin_y + pu_origin_y) * ref_pic_list0->stride_y;
#endif
        dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list0->stride_y;
        dst_stride = prediction_ptr->stride_y;
        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.

        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;
        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstY, 128, is_compound, EB_8BIT);

        av1_get_convolve_filter_params(interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);
#if !UNPACK_REF_POST_EP

        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list0->stride_y,
            bwidth,
            bheight,
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_y,
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_y,
            EB_FALSE,
            asm_type,
            16);
#endif
        convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
            src_ptr,
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_y + 8 + (8 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_y),
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_y,
#endif
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params);

        if (perform_chroma && blk_geom->has_uv && sub8x8_inter == 0) {
            //List0-Cb
#if UNPACK_REF_POST_EP
            src_ptr = ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
#else
            src_ptr = (uint16_t*)ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
#endif
            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list0->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);

            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);
            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

#if !UNPACK_REF_POST_EP

            Av1UnPackReferenceBlock(
                src_ptr,
                ref_pic_list0->stride_cb,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb,
                context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
                EB_FALSE,
                asm_type,
                8);
#endif
            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                convolve_2d_for_intrabc(
#if UNPACK_REF_POST_EP
                    src_ptr,
                    src_stride,
#else
                (const uint8_t *)(context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb)),
                    context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
#endif
                    dst_ptr,
                    dst_stride,
                    blk_geom->bwidth_uv,
                    blk_geom->bheight_uv,
                    subpel_x,
                    subpel_y,
                    &conv_params);
            else
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                    src_ptr,
                    src_stride,
#else
                context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb),
                context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
#endif
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params);

            //List0-Cr
#if UNPACK_REF_POST_EP
            src_ptr = ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
#else
            src_ptr = (uint16_t*)ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
#endif
            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list0->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);

            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);

#if !UNPACK_REF_POST_EP
            Av1UnPackReferenceBlock(
                src_ptr,
                ref_pic_list0->stride_cr,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr,
                context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
                EB_FALSE,
                asm_type,
                8);
#endif

            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                convolve_2d_for_intrabc(
#if UNPACK_REF_POST_EP
                    src_ptr,
                    src_stride,
#else
                (const uint8_t *)(context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr)),
                    context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
#endif
                    dst_ptr,
                    dst_stride,
                    blk_geom->bwidth_uv,
                    blk_geom->bheight_uv,
                    subpel_x,
                    subpel_y,
                    &conv_params);
            else
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                    src_ptr,
                    src_stride,
#else
                context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr),
                context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
#endif
                    dst_ptr,
                    dst_stride,
                    blk_geom->bwidth_uv,
                    blk_geom->bheight_uv,
                    &filter_params_x,
                    &filter_params_y,
                    subpel_x,
                    subpel_y,
                    &conv_params);
        }
    }

    if (mv_unit->pred_direction == UNI_PRED_LIST_1 || mv_unit->pred_direction == BI_PRED) {
        //List1-Y
        mv.col = mv_unit->mv[REF_LIST_1].x;
        mv.row = mv_unit->mv[REF_LIST_1].y;
        assert(ref_pic_list1 != NULL);
#if UNPACK_REF_POST_EP
        src_ptr = ref_pic_list1->buffer_y + ref_pic_list1->origin_x + pu_origin_x + (ref_pic_list1->origin_y + pu_origin_y) * ref_pic_list1->stride_y;
#else
        src_ptr = (uint16_t*)ref_pic_list1->buffer_y + ref_pic_list1->origin_x + pu_origin_x + (ref_pic_list1->origin_y + pu_origin_y) * ref_pic_list1->stride_y;
#endif
        dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list1->stride_y;
        dst_stride = prediction_ptr->stride_y;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.

        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;

        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstY, 128, is_compound, EB_8BIT);

        av1_get_convolve_filter_params(interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);

#if !UNPACK_REF_POST_EP

        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list1->stride_y,
            bwidth,
            bheight,
            context_ptr->mcp_context->local_reference_block8_bitl1->buffer_y,
            context_ptr->mcp_context->local_reference_block8_bitl1->stride_y,
            EB_FALSE,
            asm_type,
            16);
#endif
        convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
            src_ptr,
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl1->buffer_y + 8 + (8 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_y),
            context_ptr->mcp_context->local_reference_block8_bitl1->stride_y,
#endif
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params);

        if (perform_chroma && blk_geom->has_uv && sub8x8_inter == 0) {
            //List1-Cb
#if UNPACK_REF_POST_EP
            src_ptr = ref_pic_list1->buffer_cb + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cb;
#else
            src_ptr = (uint16_t*)ref_pic_list1->buffer_cb + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cb;
#endif
            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list1->stride_cb;
            dst_stride = prediction_ptr->stride_cb;
            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);

            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);
#if !UNPACK_REF_POST_EP
            Av1UnPackReferenceBlock(
                src_ptr,
                ref_pic_list1->stride_cb,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                context_ptr->mcp_context->local_reference_block8_bitl1->buffer_cb,
                context_ptr->mcp_context->local_reference_block8_bitl1->stride_cb,
                EB_FALSE,
                asm_type,
                8);
#endif
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                src_ptr,
                src_stride,
#else
                context_ptr->mcp_context->local_reference_block8_bitl1->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cb),
                context_ptr->mcp_context->local_reference_block8_bitl1->stride_cb,
#endif
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params);

            //List1-Cr
#if UNPACK_REF_POST_EP
            src_ptr = ref_pic_list1->buffer_cr + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cr;
#else
            src_ptr = (uint16_t*)ref_pic_list1->buffer_cr + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cr;
#endif
            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list1->stride_cr;
            dst_stride = prediction_ptr->stride_cr;
            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);

            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);
#if !UNPACK_REF_POST_EP
            Av1UnPackReferenceBlock(
                src_ptr,
                ref_pic_list1->stride_cr,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                context_ptr->mcp_context->local_reference_block8_bitl1->buffer_cr,
                context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr,
                EB_FALSE,
                asm_type,
                8);
#endif

            convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
                src_ptr,
                src_stride,
#else
                context_ptr->mcp_context->local_reference_block8_bitl1->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr),
                context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr,
#endif
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params);
        }
    }

    return return_error;
}
#endif

EbErrorType av1_inter_prediction_hbd(
    PictureControlSet                    *picture_control_set_ptr,
    uint8_t                                   ref_frame_type,
    CodingUnit                           *cu_ptr,
    MvUnit                               *mv_unit,
    uint8_t                                  use_intrabc,
    uint16_t                                  pu_origin_x,
    uint16_t                                  pu_origin_y,
    uint8_t                                   bwidth,
    uint8_t                                   bheight,
    EbPictureBufferDesc                  *ref_pic_list0,
    EbPictureBufferDesc                  *ref_pic_list1,
    EbPictureBufferDesc                  *prediction_ptr,
    uint16_t                                  dst_origin_x,
    uint16_t                                  dst_origin_y,
    uint8_t                                   bit_depth,
    EbAsm                                  asm_type)
{
    (void)asm_type;
    EbErrorType  return_error = EB_ErrorNone;
    uint8_t         is_compound = (mv_unit->pred_direction == BI_PRED) ? 1 : 0;
    DECLARE_ALIGNED(32, uint16_t, tmp_dstY[128 * 128]);//move this to context if stack does not hold.
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
    DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);
    MV  mv, mv_q4;

    int32_t subpel_x, subpel_y;
    uint16_t * src_ptr;
    uint16_t * dst_ptr;
    int32_t src_stride;
    int32_t dst_stride;
    ConvolveParams conv_params;
    InterpFilterParams filter_params_x, filter_params_y;

    const BlockGeom * blk_geom = get_blk_geom_mds(cu_ptr->mds_idx);

    //special treatment for chroma in 4XN/NX4 blocks
   //if one of the neighbour blocks of the parent square is intra the chroma prediction will follow the normal path using the luma MV of the current nsq block which is the latest sub8x8.
   //for this case: only uniPred is allowed.

    int32_t sub8x8_inter = 0;

    if (blk_geom->has_uv &&
        (blk_geom->bwidth == 4 || blk_geom->bheight == 4)
        )
    {
        //CHKN setup input param

        int32_t bw = blk_geom->bwidth_uv;
        int32_t bh = blk_geom->bheight_uv;
        UNUSED_VARIABLE(bw);
        UNUSED_VARIABLE(bh);

        uint32_t mi_x = pu_origin_x;       //these are luma picture wise
        uint32_t mi_y = pu_origin_y;

        MacroBlockD  *xd = cu_ptr->av1xd;
#if INCOMPLETE_SB_FIX
        xd->mi_stride = picture_control_set_ptr->mi_stride;
#else
        xd->mi_stride = picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->picture_width_in_sb*(BLOCK_SIZE_64 / 4);
#endif
        const int32_t offset = (mi_y >> MI_SIZE_LOG2) * xd->mi_stride + (mi_x >> MI_SIZE_LOG2);
        xd->mi = picture_control_set_ptr->mi_grid_base + offset;

        //CHKN fill current mi from current block
        {
            ModeInfo *miPtr = *xd->mi;
            uint8_t  miX, miY;
            MvReferenceFrame rf[2];
            av1_set_ref_frame(rf, ref_frame_type);
            for (miY = 0; miY < (blk_geom->bheight >> MI_SIZE_LOG2); miY++) {
                for (miX = 0; miX < (blk_geom->bwidth >> MI_SIZE_LOG2); miX++) {
                    miPtr[miX + miY * xd->mi_stride].mbmi.ref_frame[0] = rf[0];
                    miPtr[miX + miY * xd->mi_stride].mbmi.use_intrabc = use_intrabc;
                    if (mv_unit->pred_direction == UNI_PRED_LIST_0) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                    }
                    else if (mv_unit->pred_direction == UNI_PRED_LIST_1) {
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                    else {
                        // printf("ERRRRRRR");

                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.col = mv_unit->mv[REF_LIST_0].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[0].as_mv.row = mv_unit->mv[REF_LIST_0].y;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.col = mv_unit->mv[REF_LIST_1].x;
                        miPtr[miX + miY * xd->mi_stride].mbmi.mv[1].as_mv.row = mv_unit->mv[REF_LIST_1].y;
                    }
                }
            }
        }

        int32_t build_for_obmc = 0;

        const BlockSize bsize = blk_geom->bsize;//mi->sb_type;
        assert(bsize < BlockSizeS_ALL);
        const int32_t ss_x = 1;// pd->subsampling_x;
        const int32_t ss_y = 1;//pd->subsampling_y;
        sub8x8_inter = (block_size_wide[bsize] < 8 && ss_x) ||
            (block_size_high[bsize] < 8 && ss_y);

        if (use_intrabc) sub8x8_inter = 0;
        // For sub8x8 chroma blocks, we may be covering more than one luma block's
        // worth of pixels. Thus (mi_x, mi_y) may not be the correct coordinates for
        // the top-left corner of the prediction source - the correct top-left corner
        // is at (pre_x, pre_y).
        const int32_t row_start =
            (block_size_high[bsize] == 4) && ss_y && !build_for_obmc ? -1 : 0;
        const int32_t col_start =
            (block_size_wide[bsize] == 4) && ss_x && !build_for_obmc ? -1 : 0;

        const int32_t pre_x = (mi_x + MI_SIZE * col_start) >> ss_x;
        const int32_t pre_y = (mi_y + MI_SIZE * row_start) >> ss_y;
        UNUSED_VARIABLE(pre_x);
        UNUSED_VARIABLE(pre_y);

        sub8x8_inter = sub8x8_inter && !build_for_obmc;
        if (sub8x8_inter) {
            for (int32_t row = row_start; row <= 0 && sub8x8_inter; ++row) {
                for (int32_t col = col_start; col <= 0; ++col) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    if (!is_inter_block(this_mbmi)) sub8x8_inter = 0;
                    //if (is_intrabc_block(this_mbmi)) sub8x8_inter = 0;
                }
            }
        }

        if (sub8x8_inter) {
            // block size
            const int32_t b4_w = block_size_wide[bsize] >> ss_x;
            const int32_t b4_h = block_size_high[bsize] >> ss_y;
            const BlockSize plane_bsize = scale_chroma_bsize(bsize, ss_x, ss_y);
            assert(plane_bsize < BlockSizeS_ALL);
            const int32_t b8_w = block_size_wide[plane_bsize] >> ss_x;
            const int32_t b8_h = block_size_high[plane_bsize] >> ss_y;

            assert(!is_compound);

            if (is_compound)
                printf("ETTTT");

            //const struct Buf2d orig_pred_buf[2] = { pd->pre[0], pd->pre[1] };

            int32_t row = row_start;
            int32_t src_stride;
            for (int32_t y = 0; y < b8_h; y += b4_h) {
                int32_t col = col_start;
                for (int32_t x = 0; x < b8_w; x += b4_w) {
                    ModeInfo *miPtr = *xd->mi;
                    const MbModeInfo *this_mbmi = &miPtr[row * xd->mi_stride + col].mbmi;

                    // MbModeInfo *this_mbmi = xd->mi[row * xd->mi_stride + col];
                     //is_compound = has_second_ref(this_mbmi);
                    int32_t tmp_dst_stride = 8;
                    UNUSED_VARIABLE(tmp_dst_stride);
                    assert(bw < 8 || bh < 8);

                    // ConvolveParams conv_params = get_conv_params_no_round(
                     //    0, plane, xd->tmp_conv_dst, tmp_dst_stride, is_compound, xd->bd);
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, BLOCK_SIZE_64, is_compound, bit_depth);
                    conv_params.use_jnt_comp_avg = 0;
#if MCP_4XN_FIX
                    uint8_t ref_idx = get_ref_frame_idx(this_mbmi->ref_frame[0]);
                    assert(ref_idx < REF_LIST_MAX_DEPTH);
                    EbPictureBufferDesc  *ref_pic = this_mbmi->ref_frame[0] ==
                        LAST_FRAME || this_mbmi->ref_frame[0] == LAST2_FRAME || this_mbmi->ref_frame[0] == LAST3_FRAME || this_mbmi->ref_frame[0] == GOLDEN_FRAME ?
                        ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_0][ref_idx]->object_ptr)->reference_picture16bit :
                        ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_1][ref_idx]->object_ptr)->reference_picture16bit;
#else
                    EbPictureBufferDesc                  *ref_pic = this_mbmi->ref_frame[0] == LAST_FRAME ? ref_pic_list0 : ref_pic_list1;
#endif
                    src_ptr = (uint16_t*)ref_pic->buffer_cb + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cb;
                    dst_ptr = (uint16_t*)prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
                    src_stride = ref_pic->stride_cb;
                    dst_stride = prediction_ptr->stride_cb;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cb;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cb;

                    const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);

                    av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

                    convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                        src_ptr,
                        src_stride,
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        subpel_x,
                        subpel_y,
                        &conv_params,
                        bit_depth);

                    //Cr
                    conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, BLOCK_SIZE_64, is_compound, bit_depth);
                    conv_params.use_jnt_comp_avg = 0;

                    src_ptr = (uint16_t*)ref_pic->buffer_cr + (ref_pic->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic->stride_cr;
                    dst_ptr = (uint16_t*)prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
                    src_stride = ref_pic->stride_cr;
                    dst_stride = prediction_ptr->stride_cr;
                    src_ptr = src_ptr + x + y * ref_pic->stride_cr;
                    dst_ptr = dst_ptr + x + y * prediction_ptr->stride_cr;

                    // const MV mv = this_mbmi->mv[0].as_mv;
                    mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
                    subpel_x = mv_q4.col & SUBPEL_MASK;
                    subpel_y = mv_q4.row & SUBPEL_MASK;
                    src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);

                    av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                        &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

                    convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                        src_ptr,
                        src_stride,
                        dst_ptr,
                        dst_stride,
                        b4_w,
                        b4_h,
                        &filter_params_x,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        &filter_params_y,//puSize > 8 ? &av1RegularFilter : &av1RegularFilterW4,
                        subpel_x,
                        subpel_y,
                        &conv_params,
                        bit_depth);

                    ++col;
                }
                ++row;
            }

            //for (ref = 0; ref < 2; ++ref) pd->pre[ref] = orig_pred_buf[ref];

            //return;
        }
    }

    if (mv_unit->pred_direction == UNI_PRED_LIST_0 || mv_unit->pred_direction == BI_PRED) {
        //List0-Y
        mv.col = mv_unit->mv[REF_LIST_0].x;
        mv.row = mv_unit->mv[REF_LIST_0].y;

        src_ptr = (uint16_t*)ref_pic_list0->buffer_y + ref_pic_list0->origin_x + pu_origin_x + (ref_pic_list0->origin_y + pu_origin_y) * ref_pic_list0->stride_y;
        dst_ptr = (uint16_t*)prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list0->stride_y;
        dst_stride = prediction_ptr->stride_y;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;
        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstY, 128, is_compound, bit_depth);
        av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);

        convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
            src_ptr,
            src_stride,
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params,
            bit_depth);

        if (blk_geom->has_uv && sub8x8_inter == 0) {
            //List0-Cb
            src_ptr = (uint16_t*)ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
            dst_ptr = (uint16_t*)prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list0->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, bit_depth);

            av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                highbd_convolve_2d_for_intrabc(
                (const uint16_t *)src_ptr,
                    src_stride,
                    dst_ptr, dst_stride, blk_geom->bwidth_uv, blk_geom->bheight_uv, subpel_x,
                    subpel_y, &conv_params, bit_depth);
            else
            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);

            //List0-Cr
            src_ptr = (uint16_t*)ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
            dst_ptr = (uint16_t*)prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list0->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, bit_depth);
            if (use_intrabc && (subpel_x != 0 || subpel_y != 0))
                highbd_convolve_2d_for_intrabc(
                (const uint16_t *)src_ptr,
                    src_stride,
                    dst_ptr, dst_stride, blk_geom->bwidth_uv, blk_geom->bheight_uv, subpel_x,
                    subpel_y, &conv_params, bit_depth);
            else
            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);
        }
    }

    if (mv_unit->pred_direction == UNI_PRED_LIST_1 || mv_unit->pred_direction == BI_PRED) {
        //List0-Y
        mv.col = mv_unit->mv[REF_LIST_1].x;
        mv.row = mv_unit->mv[REF_LIST_1].y;

        src_ptr = (uint16_t*)ref_pic_list1->buffer_y + ref_pic_list1->origin_x + pu_origin_x + (ref_pic_list1->origin_y + pu_origin_y) * ref_pic_list1->stride_y;
        dst_ptr = (uint16_t*)prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        src_stride = ref_pic_list1->stride_y;
        dst_stride = prediction_ptr->stride_y;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, bwidth, bheight, 0, 0);//mv_q4 has 1 extra bit for fractionnal to accomodate chroma when accessing filter coeffs.
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;

        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstY, 128, is_compound, bit_depth);
        av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
            &filter_params_y, bwidth, bheight);

        convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
            src_ptr,
            src_stride,
            dst_ptr,
            dst_stride,
            bwidth,
            bheight,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params,
            bit_depth);

        if (blk_geom->has_uv && sub8x8_inter == 0) {
            //List0-Cb
            src_ptr = (uint16_t*)ref_pic_list1->buffer_cb + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cb;
            dst_ptr = (uint16_t*)prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list1->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCb, 64, is_compound, bit_depth);
            av1_get_convolve_filter_params(cu_ptr->interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);

            //List0-Cr
            src_ptr = (uint16_t*)ref_pic_list1->buffer_cr + (ref_pic_list1->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list1->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list1->stride_cr;
            dst_ptr = (uint16_t*)prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list1->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, (mv_unit->pred_direction == BI_PRED) ? 1 : 0, 0, tmp_dstCr, 64, is_compound, bit_depth);
            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);
        }
    }

    return return_error;
}

EbErrorType warped_motion_prediction(
    MvUnit                               *mv_unit,
    uint16_t                                pu_origin_x,
    uint16_t                                pu_origin_y,
    CodingUnit                           *cu_ptr,
    const BlockGeom                        *blk_geom,
    EbPictureBufferDesc                  *ref_pic_list0,
    EbPictureBufferDesc                  *prediction_ptr,
    uint16_t                                dst_origin_x,
    uint16_t                                dst_origin_y,
    EbWarpedMotionParams                   *wm_params,
    uint8_t                                 bit_depth,
    EbBool                                  perform_chroma,
    EbAsm                                   asm_type)
{
    (void)asm_type;

    EbErrorType  return_error = EB_ErrorNone;
    uint8_t is_compound = (mv_unit->pred_direction == BI_PRED) ? 1 : 0;
    assert(!is_compound);
    EbBool  is16bit = (EbBool)(bit_depth > EB_8BIT);

    int32_t src_stride;
    int32_t dst_stride;
    uint16_t buf_width;
    uint16_t buf_height;
    ConvolveParams conv_params;
    uint8_t ss_x = 1; // subsamplings
    uint8_t ss_y = 1;

    if (!is16bit) {
        uint8_t *src_ptr;
        uint8_t *dst_ptr;
        assert(ref_pic_list0 != NULL);
        // Y - UNI_PRED_LIST_0
        src_ptr = ref_pic_list0->buffer_y + ref_pic_list0->origin_x + ref_pic_list0->origin_y * ref_pic_list0->stride_y;
        src_stride = ref_pic_list0->stride_y;
        buf_width = ref_pic_list0->width;
        buf_height = ref_pic_list0->height;

        dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        dst_stride = prediction_ptr->stride_y;
        conv_params = get_conv_params_no_round(0, 0, 0, NULL, 128, is_compound, EB_8BIT);

        av1_warp_plane(
            wm_params,
            (int) is16bit,
            bit_depth,
            src_ptr,
            (int) buf_width,
            (int) buf_height,
            src_stride,
            dst_ptr,
            pu_origin_x,
            pu_origin_y,
            blk_geom->bwidth,
            blk_geom->bheight,
            dst_stride,
            0, //int subsampling_x,
            0, //int subsampling_y,
            &conv_params);

        if (!blk_geom->has_uv)
            return return_error;

        if (perform_chroma) {
         if (blk_geom->bwidth >= 16  && blk_geom->bheight >= 16 ) {
            // Cb
            src_ptr = ref_pic_list0->buffer_cb + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2) * ref_pic_list0->stride_cb;
            src_stride = ref_pic_list0->stride_cb;

            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            dst_stride = prediction_ptr->stride_cb;
            conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, EB_8BIT);

            av1_warp_plane(
                wm_params,
                (int) is16bit,
                bit_depth,
                src_ptr,
                buf_width >> ss_x,
                buf_height >> ss_y,
                src_stride,
                dst_ptr,
                pu_origin_x >> ss_x,
                pu_origin_y >> ss_y,
                blk_geom->bwidth >> ss_x,
                blk_geom->bheight >> ss_y,
                dst_stride,
                ss_x,
                ss_y,
                &conv_params);

            // Cr
            src_ptr = ref_pic_list0->buffer_cr + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2 ) * ref_pic_list0->stride_cr;
            src_stride = ref_pic_list0->stride_cr;

            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, EB_8BIT);

            av1_warp_plane(
                wm_params,
                (int) is16bit,
                bit_depth,
                src_ptr,
                (int) buf_width >> ss_x,
                (int) buf_height >> ss_y,
                src_stride,
                dst_ptr,
                pu_origin_x >> ss_x,
                pu_origin_y >> ss_y,
                blk_geom->bwidth >> ss_x,
                blk_geom->bheight >> ss_y,
                dst_stride,
                ss_x,
                ss_y,
                &conv_params);
        } else { // Translation prediction when chroma block is smaller than 8x8
            DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
            DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);
            InterpFilterParams filter_params_x, filter_params_y;
            const uint32_t interp_filters = 0;
            MV  mv, mv_q4;
            int32_t subpel_x, subpel_y;

            mv.col = mv_unit->mv[REF_LIST_0].x;
            mv.row = mv_unit->mv[REF_LIST_0].y;

            //List0-Cb
            src_ptr = ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
            dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list0->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);

            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params);

            //List0-Cr
            src_ptr = ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
            dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list0->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);
            convolve[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params);
            }
        }
    } else { // HBD
        uint16_t *src_ptr;
        uint16_t *dst_ptr;

        // Y - UNI_PRED_LIST_0
        src_ptr = (uint16_t *)ref_pic_list0->buffer_y + ref_pic_list0->origin_x + ref_pic_list0->origin_y * ref_pic_list0->stride_y;
        src_stride = ref_pic_list0->stride_y;
        buf_width = ref_pic_list0->width;
        buf_height = ref_pic_list0->height;

        dst_ptr = (uint16_t *)prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
        dst_stride = prediction_ptr->stride_y;
        conv_params = get_conv_params_no_round(0, 0, 0, NULL, 128, is_compound, bit_depth);

        av1_warp_plane_hbd(
            wm_params,
            bit_depth,
            src_ptr,
            (int) buf_width,
            (int) buf_height,
            src_stride,
            dst_ptr,
            pu_origin_x,
            pu_origin_y,
            blk_geom->bwidth,
            blk_geom->bheight,
            dst_stride,
            0, //int subsampling_x,
            0, //int subsampling_y,
            &conv_params);

        if (!blk_geom->has_uv)
            return return_error;

        if (perform_chroma) {
         if (blk_geom->bwidth >= 16  && blk_geom->bheight >= 16 ) {
            // Cb
            src_ptr = (uint16_t *)ref_pic_list0->buffer_cb + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2) * ref_pic_list0->stride_cb;
            src_stride = ref_pic_list0->stride_cb;

            dst_ptr = (uint16_t *)prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            dst_stride = prediction_ptr->stride_cb;
            conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, bit_depth);

            av1_warp_plane_hbd(
                wm_params,
                bit_depth,
                src_ptr,
                buf_width >> ss_x,
                buf_height >> ss_y,
                src_stride,
                dst_ptr,
                pu_origin_x >> ss_x,
                pu_origin_y >> ss_y,
                blk_geom->bwidth >> ss_x,
                blk_geom->bheight >> ss_y,
                dst_stride,
                ss_x,
                ss_y,
                &conv_params);

            // Cr
            src_ptr = (uint16_t *)ref_pic_list0->buffer_cr + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2 ) * ref_pic_list0->stride_cr;
            src_stride = ref_pic_list0->stride_cr;

            dst_ptr = (uint16_t *)prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, bit_depth);

            av1_warp_plane_hbd(
                wm_params,
                bit_depth,
                src_ptr,
                (int) buf_width >> ss_x,
                (int) buf_height >> ss_y,
                src_stride,
                dst_ptr,
                pu_origin_x >> ss_x,
                pu_origin_y >> ss_y,
                blk_geom->bwidth >> ss_x,
                blk_geom->bheight >> ss_y,
                dst_stride,
                ss_x,
                ss_y,
                &conv_params);
        } else { // Simple translation prediction when chroma block is smaller than 8x8
            DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
            DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);
            InterpFilterParams filter_params_x, filter_params_y;
            const uint32_t interp_filters = 0;
            MV  mv, mv_q4;
            int32_t subpel_x, subpel_y;

            mv.col = mv_unit->mv[REF_LIST_0].x;
            mv.row = mv_unit->mv[REF_LIST_0].y;

            //List0-Cb
            src_ptr = (uint16_t *)ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
            dst_ptr = (uint16_t *)prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
            src_stride = ref_pic_list0->stride_cb;
            dst_stride = prediction_ptr->stride_cb;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, bit_depth);

            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);

            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);

            //List0-Cr
            src_ptr = (uint16_t *)ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
            dst_ptr = (uint16_t *)prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
            src_stride = ref_pic_list0->stride_cr;
            dst_stride = prediction_ptr->stride_cr;

            mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
            subpel_x = mv_q4.col & SUBPEL_MASK;
            subpel_y = mv_q4.row & SUBPEL_MASK;
            src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
            conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, bit_depth);
            convolveHbd[subpel_x != 0][subpel_y != 0][is_compound](
                src_ptr,
                src_stride,
                dst_ptr,
                dst_stride,
                blk_geom->bwidth_uv,
                blk_geom->bheight_uv,
                &filter_params_x,
                &filter_params_y,
                subpel_x,
                subpel_y,
                &conv_params,
                bit_depth);
            }
        }
    }

    return return_error;
}

EbErrorType warped_motion_prediction_md(
    MvUnit                               *mv_unit,
    ModeDecisionContext                  *md_context_ptr,
    uint16_t                                pu_origin_x,
    uint16_t                                pu_origin_y,
    CodingUnit                           *cu_ptr,
    const BlockGeom                        *blk_geom,
    EbPictureBufferDesc                  *ref_pic_list0,
    EbPictureBufferDesc                  *prediction_ptr,
    uint16_t                                dst_origin_x,
    uint16_t                                dst_origin_y,
    EbWarpedMotionParams                   *wm_params,
    EbAsm                                   asm_type)
{
    EbErrorType  return_error = EB_ErrorNone;
    uint8_t is_compound = (mv_unit->pred_direction == BI_PRED) ? 1 : 0;
    (void)asm_type;
    assert(!is_compound);

    int32_t src_stride;
    int32_t dst_stride;
    uint16_t buf_width;
    uint16_t buf_height;
    ConvolveParams conv_params;
    uint8_t ss_x = 1; // subsamplings
    uint8_t ss_y = 1;
#if UNPACK_REF_POST_EP
    uint8_t *src_ptr;
#else
    uint16_t *src_ptr;
#endif
    uint8_t *dst_ptr;

    // Y - UNI_PRED_LIST_0
    assert(ref_pic_list0 != NULL);
#if UNPACK_REF_POST_EP
    src_ptr = ref_pic_list0->buffer_y + ref_pic_list0->origin_x + ref_pic_list0->origin_y * ref_pic_list0->stride_y;
#else
    src_ptr = (uint16_t *)ref_pic_list0->buffer_y + ref_pic_list0->origin_x + ref_pic_list0->origin_y * ref_pic_list0->stride_y;
#endif
    src_stride = ref_pic_list0->stride_y;
    buf_width = ref_pic_list0->width;
    buf_height = ref_pic_list0->height;

    dst_ptr = prediction_ptr->buffer_y + prediction_ptr->origin_x + dst_origin_x + (prediction_ptr->origin_y + dst_origin_y) * prediction_ptr->stride_y;
    dst_stride = prediction_ptr->stride_y;
    conv_params = get_conv_params_no_round(0, 0, 0, NULL, 128, is_compound, EB_8BIT);

#if !UNPACK_REF_POST_EP
    Av1UnPackReferenceBlock(
        src_ptr,
        ref_pic_list0->stride_y,
        blk_geom->bwidth,
        blk_geom->bheight,
        context_ptr->mcp_context->local_reference_block8_bitl0->buffer_y,
        context_ptr->mcp_context->local_reference_block8_bitl0->stride_y,
        EB_FALSE,
        asm_type,
        16);
#endif

    av1_warp_plane(
        wm_params,
        0,  // int use_hbd
        8,  // int bd
#if UNPACK_REF_POST_EP
        src_ptr,
#else
        context_ptr->mcp_context->local_reference_block8_bitl0->buffer_y + 8 + (8 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_y),
#endif
        (int) buf_width,
        (int) buf_height,
#if UNPACK_REF_POST_EP
        src_stride,
#else
        context_ptr->mcp_context->local_reference_block8_bitl0->stride_y,
#endif
        dst_ptr,
        pu_origin_x,
        pu_origin_y,
        blk_geom->bwidth,
        blk_geom->bheight,
        dst_stride,
        0, // int subsampling_x
        0, // int subsampling_y
        &conv_params);

    if (!blk_geom->has_uv)
        return return_error;
    if (md_context_ptr->chroma_level <= CHROMA_MODE_1) {
     if (blk_geom->bwidth >= 16  && blk_geom->bheight >= 16 ) {
        // Cb
#if UNPACK_REF_POST_EP
         src_ptr = ref_pic_list0->buffer_cb + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2) * ref_pic_list0->stride_cb;
#else
        src_ptr = (uint16_t *)ref_pic_list0->buffer_cb + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2) * ref_pic_list0->stride_cb;
#endif
        src_stride = ref_pic_list0->stride_cb;

        dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
        dst_stride = prediction_ptr->stride_cb;
        conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, EB_8BIT);
#if !UNPACK_REF_POST_EP
        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list0->stride_cb,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb,
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
            EB_FALSE,
            asm_type,
            8);
#endif
        av1_warp_plane(
            wm_params,
            0, // int use_hbd
            8, // int bd
#if UNPACK_REF_POST_EP
            src_ptr,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb),
#endif
            buf_width >> ss_x,
            buf_height >> ss_y,
#if UNPACK_REF_POST_EP
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
#endif
            dst_ptr,
            pu_origin_x >> ss_x,
            pu_origin_y >> ss_y,
            blk_geom->bwidth >> ss_x,
            blk_geom->bheight >> ss_y,
            dst_stride,
            ss_x,
            ss_y,
            &conv_params);

        // Cr
#if UNPACK_REF_POST_EP
        src_ptr = ref_pic_list0->buffer_cr + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2) * ref_pic_list0->stride_cr;
#else
        src_ptr = (uint16_t *)ref_pic_list0->buffer_cr + ref_pic_list0->origin_x / 2 + (ref_pic_list0->origin_y / 2 ) * ref_pic_list0->stride_cr;
#endif
        src_stride = ref_pic_list0->stride_cr;

        dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
        dst_stride = prediction_ptr->stride_cr;

            conv_params = get_conv_params_no_round(0, 0, 0, NULL, 64, is_compound, EB_8BIT);
#if !UNPACK_REF_POST_EP
        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list0->stride_cr,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr,
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
            EB_FALSE,
            asm_type,
            8);
#endif
        av1_warp_plane(
            wm_params,
            0, // int use_hbd
            8, // int bd
#if UNPACK_REF_POST_EP
            src_ptr,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr),
#endif
            (int) buf_width >> ss_x,
            (int) buf_height >> ss_y,
#if UNPACK_REF_POST_EP
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
#endif
            dst_ptr,
            pu_origin_x >> ss_x,
            pu_origin_y >> ss_y,
            blk_geom->bwidth >> ss_x,
            blk_geom->bheight >> ss_y,
            dst_stride,
            ss_x,
            ss_y,
            &conv_params);
    } else { // Simple translation prediction when chroma block is smaller than 8x8
        DECLARE_ALIGNED(32, uint16_t, tmp_dstCb[64 * 64]);
        DECLARE_ALIGNED(32, uint16_t, tmp_dstCr[64 * 64]);
        InterpFilterParams filter_params_x, filter_params_y;
        const uint32_t interp_filters = 0;
        MV  mv, mv_q4;
        int32_t subpel_x, subpel_y;

            mv.col = mv_unit->mv[REF_LIST_0].x;
            mv.row = mv_unit->mv[REF_LIST_0].y;

        //List0-Cb
#if UNPACK_REF_POST_EP
        src_ptr = ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
#else
        src_ptr = (uint16_t *)ref_pic_list0->buffer_cb + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cb;
#endif
        dst_ptr = prediction_ptr->buffer_cb + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cb;
        src_stride = ref_pic_list0->stride_cb;
        dst_stride = prediction_ptr->stride_cb;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;
        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCb, 64, is_compound, EB_8BIT);

            av1_get_convolve_filter_params(interp_filters, &filter_params_x,
                &filter_params_y, blk_geom->bwidth_uv, blk_geom->bheight_uv);
#if !UNPACK_REF_POST_EP
        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list0->stride_cb,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb,
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
            EB_FALSE,
            asm_type,
            8);
#endif
        convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
            src_ptr,
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cb + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb),
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cb,
#endif
            dst_ptr,
            dst_stride,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params);

        //List0-Cr
#if UNPACK_REF_POST_EP
        src_ptr = ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
#else
        src_ptr = (uint16_t *)ref_pic_list0->buffer_cr + (ref_pic_list0->origin_x + ((pu_origin_x >> 3) << 3)) / 2 + (ref_pic_list0->origin_y + ((pu_origin_y >> 3) << 3)) / 2 * ref_pic_list0->stride_cr;
#endif
        dst_ptr = prediction_ptr->buffer_cr + (prediction_ptr->origin_x + ((dst_origin_x >> 3) << 3)) / 2 + (prediction_ptr->origin_y + ((dst_origin_y >> 3) << 3)) / 2 * prediction_ptr->stride_cr;
        src_stride = ref_pic_list0->stride_cr;
        dst_stride = prediction_ptr->stride_cr;

        mv_q4 = clamp_mv_to_umv_border_sb(cu_ptr->av1xd, &mv, blk_geom->bwidth_uv, blk_geom->bheight_uv, 1, 1);
        subpel_x = mv_q4.col & SUBPEL_MASK;
        subpel_y = mv_q4.row & SUBPEL_MASK;
        src_ptr = src_ptr + (mv_q4.row >> SUBPEL_BITS) * src_stride + (mv_q4.col >> SUBPEL_BITS);
        conv_params = get_conv_params_no_round(0, 0, 0, tmp_dstCr, 64, is_compound, EB_8BIT);
#if !UNPACK_REF_POST_EP
        Av1UnPackReferenceBlock(
            src_ptr,
            ref_pic_list0->stride_cr,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr,
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
            EB_FALSE,
            asm_type,
            8);
#endif
        convolve[subpel_x != 0][subpel_y != 0][is_compound](
#if UNPACK_REF_POST_EP
            src_ptr,
            src_stride,
#else
            context_ptr->mcp_context->local_reference_block8_bitl0->buffer_cr + 4 + (4 * context_ptr->mcp_context->local_reference_block8_bitl1->stride_cr),
            context_ptr->mcp_context->local_reference_block8_bitl0->stride_cr,
#endif
            dst_ptr,
            dst_stride,
            blk_geom->bwidth_uv,
            blk_geom->bheight_uv,
            &filter_params_x,
            &filter_params_y,
            subpel_x,
            subpel_y,
            &conv_params);
        }
    }

    return return_error;
}

#define SWITCHABLE_INTERP_RATE_FACTOR 1
extern int32_t av1_get_pred_context_switchable_interp(
    NeighborArrayUnit     *ref_frame_type_neighbor_array,
    MvReferenceFrame rf0,
    MvReferenceFrame rf1,
    NeighborArrayUnit32     *interpolation_type_neighbor_array,
    uint32_t cu_origin_x,
    uint32_t cu_origin_y,
    int32_t dir
);

int32_t av1_get_switchable_rate(
    ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    const Av1Common *const cm,
    ModeDecisionContext *md_context_ptr//,
    // Macroblock *x,
    // const MacroBlockD *xd
) {
    if (cm->interp_filter == SWITCHABLE) {
        // const MbModeInfo *const mbmi = xd->mi[0];
        int32_t inter_filter_cost = 0;
        int32_t dir;

        for (dir = 0; dir < 2; ++dir) {
            MvReferenceFrame rf[2];
            av1_set_ref_frame(rf, candidate_buffer_ptr->candidate_ptr->ref_frame_type);
            const int32_t ctx = av1_get_pred_context_switchable_interp(
                md_context_ptr->ref_frame_type_neighbor_array,
                rf[0],
                rf[1],
                md_context_ptr->interpolation_type_neighbor_array,
                md_context_ptr->cu_origin_x,
                md_context_ptr->cu_origin_y,
                //xd,
                dir
            );

            const InterpFilter filter = av1_extract_interp_filter(/*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters, dir);
            assert(ctx < SWITCHABLE_FILTER_CONTEXTS);
            assert(filter < SWITCHABLE_FILTERS);
            inter_filter_cost +=  /*x->switchable_interp_costs*/md_context_ptr->md_rate_estimation_ptr->switchable_interp_fac_bitss[ctx][filter];
        }
        return SWITCHABLE_INTERP_RATE_FACTOR * inter_filter_cost;
    }
    else
        return 0;
}
//void model_rd_norm(int32_t xsq_q10, int32_t *r_q10, int32_t *d_q10) {
 // NOTE: The tables below must be of the same size.

 // The functions described below are sampled at the four most significant
 // bits of x^2 + 8 / 256.

void highbd_variance64_c(const uint8_t *a8, int32_t a_stride,
    const uint8_t *b8, int32_t b_stride, int32_t w, int32_t h,
    uint64_t *sse) {
    const uint8_t *a = a8;//CONVERT_TO_SHORTPTR(a8);
    const uint8_t *b = b8;//CONVERT_TO_SHORTPTR(b8);
    uint64_t tsse = 0;
    for (int32_t i = 0; i < h; ++i) {
        for (int32_t j = 0; j < w; ++j) {
            const int32_t diff = a[j] - b[j];
            tsse += (uint32_t)(diff * diff);
        }
        a += a_stride;
        b += b_stride;
    }
    *sse = tsse;
}
void highbd_8_variance(const uint8_t *a8, int32_t a_stride,
    const uint8_t *b8, int32_t b_stride, int32_t w, int32_t h,
    uint32_t *sse) {
    uint64_t sse_long = 0;
    highbd_variance64(a8, a_stride, b8, b_stride, w, h, &sse_long);
    *sse = (uint32_t)sse_long;
}
/*static*/ /*INLINE*/ void variance4x4_64_sse4_1(uint8_t *a8, int32_t a_stride,
    uint8_t *b8, int32_t b_stride,
    uint64_t *sse, int64_t *sum) {
    __m128i u0, u1, u2, u3;
    __m128i s0, s1, s2, s3;
    __m128i t0, t1, x0, y0;
    __m128i a0, a1, a2, a3;
    __m128i b0, b1, b2, b3;
    __m128i k_one_epi16 = _mm_set1_epi16((int16_t)1);

    uint8_t *a = a8;//CONVERT_TO_SHORTPTR(a8);
    uint8_t *b = b8;//CONVERT_TO_SHORTPTR(b8);

    a0 = _mm_loadl_epi64((__m128i const *)(a + 0 * a_stride));
    a1 = _mm_loadl_epi64((__m128i const *)(a + 1 * a_stride));
    a2 = _mm_loadl_epi64((__m128i const *)(a + 2 * a_stride));
    a3 = _mm_loadl_epi64((__m128i const *)(a + 3 * a_stride));

    b0 = _mm_loadl_epi64((__m128i const *)(b + 0 * b_stride));
    b1 = _mm_loadl_epi64((__m128i const *)(b + 1 * b_stride));
    b2 = _mm_loadl_epi64((__m128i const *)(b + 2 * b_stride));
    b3 = _mm_loadl_epi64((__m128i const *)(b + 3 * b_stride));

    u0 = _mm_unpacklo_epi16(a0, a1);
    u1 = _mm_unpacklo_epi16(a2, a3);
    u2 = _mm_unpacklo_epi16(b0, b1);
    u3 = _mm_unpacklo_epi16(b2, b3);

    s0 = _mm_sub_epi16(u0, u2);
    s1 = _mm_sub_epi16(u1, u3);

    t0 = _mm_madd_epi16(s0, k_one_epi16);
    t1 = _mm_madd_epi16(s1, k_one_epi16);

    s2 = _mm_hadd_epi32(t0, t1);
    s3 = _mm_hadd_epi32(s2, s2);
    y0 = _mm_hadd_epi32(s3, s3);

    t0 = _mm_madd_epi16(s0, s0);
    t1 = _mm_madd_epi16(s1, s1);

    s2 = _mm_hadd_epi32(t0, t1);
    s3 = _mm_hadd_epi32(s2, s2);
    x0 = _mm_hadd_epi32(s3, s3);

    *sse = (uint64_t)_mm_extract_epi32(x0, 0);
    *sum = (int64_t)_mm_extract_epi32(y0, 0);
}

#define RDDIV_BITS 7
#define RDCOST(RM, R, D)                                            \
  (ROUND_POWER_OF_TWO(((uint64_t)(R)) * (RM), AV1_PROB_COST_SHIFT) + \
   ((D) * (1 << RDDIV_BITS)))

static void model_rd_norm(int32_t xsq_q10, int32_t *r_q10, int32_t *d_q10) {
    // NOTE: The tables below must be of the same size.

    // The functions described below are sampled at the four most significant
    // bits of x^2 + 8 / 256.

    // Normalized rate:
    // This table models the rate for a Laplacian source with given variance
    // when quantized with a uniform quantizer with given stepsize. The
    // closed form expression is:
    // Rn(x) = H(sqrt(r)) + sqrt(r)*[1 + H(r)/(1 - r)],
    // where r = exp(-sqrt(2) * x) and x = qpstep / sqrt(variance),
    // and H(x) is the binary entropy function.
    static const int32_t rate_tab_q10[] = {
      65536, 6086, 5574, 5275, 5063, 4899, 4764, 4651, 4553, 4389, 4255, 4142,
      4044,  3958, 3881, 3811, 3748, 3635, 3538, 3453, 3376, 3307, 3244, 3186,
      3133,  3037, 2952, 2877, 2809, 2747, 2690, 2638, 2589, 2501, 2423, 2353,
      2290,  2232, 2179, 2130, 2084, 2001, 1928, 1862, 1802, 1748, 1698, 1651,
      1608,  1530, 1460, 1398, 1342, 1290, 1243, 1199, 1159, 1086, 1021, 963,
      911,   864,  821,  781,  745,  680,  623,  574,  530,  490,  455,  424,
      395,   345,  304,  269,  239,  213,  190,  171,  154,  126,  104,  87,
      73,    61,   52,   44,   38,   28,   21,   16,   12,   10,   8,    6,
      5,     3,    2,    1,    1,    1,    0,    0,
    };
    // Normalized distortion:
    // This table models the normalized distortion for a Laplacian source
    // with given variance when quantized with a uniform quantizer
    // with given stepsize. The closed form expression is:
    // Dn(x) = 1 - 1/sqrt(2) * x / sinh(x/sqrt(2))
    // where x = qpstep / sqrt(variance).
    // Note the actual distortion is Dn * variance.
    static const int32_t dist_tab_q10[] = {
      0,    0,    1,    1,    1,    2,    2,    2,    3,    3,    4,    5,
      5,    6,    7,    7,    8,    9,    11,   12,   13,   15,   16,   17,
      18,   21,   24,   26,   29,   31,   34,   36,   39,   44,   49,   54,
      59,   64,   69,   73,   78,   88,   97,   106,  115,  124,  133,  142,
      151,  167,  184,  200,  215,  231,  245,  260,  274,  301,  327,  351,
      375,  397,  418,  439,  458,  495,  528,  559,  587,  613,  637,  659,
      680,  717,  749,  777,  801,  823,  842,  859,  874,  899,  919,  936,
      949,  960,  969,  977,  983,  994,  1001, 1006, 1010, 1013, 1015, 1017,
      1018, 1020, 1022, 1022, 1023, 1023, 1023, 1024,
    };
    static const int32_t xsq_iq_q10[] = {
      0,      4,      8,      12,     16,     20,     24,     28,     32,
      40,     48,     56,     64,     72,     80,     88,     96,     112,
      128,    144,    160,    176,    192,    208,    224,    256,    288,
      320,    352,    384,    416,    448,    480,    544,    608,    672,
      736,    800,    864,    928,    992,    1120,   1248,   1376,   1504,
      1632,   1760,   1888,   2016,   2272,   2528,   2784,   3040,   3296,
      3552,   3808,   4064,   4576,   5088,   5600,   6112,   6624,   7136,
      7648,   8160,   9184,   10208,  11232,  12256,  13280,  14304,  15328,
      16352,  18400,  20448,  22496,  24544,  26592,  28640,  30688,  32736,
      36832,  40928,  45024,  49120,  53216,  57312,  61408,  65504,  73696,
      81888,  90080,  98272,  106464, 114656, 122848, 131040, 147424, 163808,
      180192, 196576, 212960, 229344, 245728,
    };
    const int32_t tmp = (xsq_q10 >> 2) + 8;
    const int32_t k = get_msb(tmp) - 3;
    const int32_t xq = (k << 3) + ((tmp >> k) & 0x7);
    const int32_t one_q10 = 1 << 10;
    const int32_t a_q10 = ((xsq_q10 - xsq_iq_q10[xq]) << 10) >> (2 + k);
    const int32_t b_q10 = one_q10 - a_q10;
    *r_q10 = (rate_tab_q10[xq] * b_q10 + rate_tab_q10[xq + 1] * a_q10) >> 10;
    *d_q10 = (dist_tab_q10[xq] * b_q10 + dist_tab_q10[xq + 1] * a_q10) >> 10;
}

void av1_model_rd_from_var_lapndz(int64_t var, uint32_t n_log2,
    uint32_t qstep, int32_t *rate,
    int64_t *dist) {
    // This function models the rate and distortion for a Laplacian
    // source with given variance when quantized with a uniform quantizer
    // with given stepsize. The closed form expressions are in:
    // Hang and Chen, "Source Model for transform video coder and its
    // application - Part I: Fundamental Theory", IEEE Trans. Circ.
    // Sys. for Video Tech., April 1997.
    if (var == 0) {
        *rate = 0;
        *dist = 0;
    }
    else {
        int32_t d_q10, r_q10;
        static const uint32_t MAX_XSQ_Q10 = 245727;
        const uint64_t xsq_q10_64 =
            (((uint64_t)qstep * qstep << (n_log2 + 10)) + (var >> 1)) / var;
        const int32_t xsq_q10 = (int32_t)MIN(xsq_q10_64, MAX_XSQ_Q10);
        model_rd_norm(xsq_q10, &r_q10, &d_q10);
        *rate = ROUND_POWER_OF_TWO(r_q10 << n_log2, 10 - AV1_PROB_COST_SHIFT);
        *dist = (var * (int64_t)d_q10 + 512) >> 10;
    }
}

/*static*/ void model_rd_from_sse(
    BlockSize bsize,
    int16_t quantizer,
    //const Av1Comp *const cpi,
    //const MacroBlockD *const xd,
    //BlockSize bsize,
    //int32_t plane,
    uint64_t sse,
    uint32_t *rate,
    uint64_t *dist){
    // OMK
  //const struct MacroblockdPlane *const pd = &xd->plane[plane];
    int32_t dequant_shift = 3;
    /* OMK (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) ? xd->bd - 5 :3;*/

// Fast approximate the modelling function.
    if (0/*cpi->sf.simple_model_rd_from_var*/) {
        int64_t square_error = (uint64_t)sse;
        quantizer = quantizer >> dequant_shift;

        if (quantizer < 120)
            *rate = (int32_t)((square_error * (280 - quantizer)) >>
            (16 - AV1_PROB_COST_SHIFT));
        else
            *rate = 0;
        *dist = (uint64_t)(square_error * quantizer) >> 8;
    }
    else {
        av1_model_rd_from_var_lapndz((uint64_t)sse, num_pels_log2_lookup[bsize],
            quantizer >> dequant_shift, (int32_t*)rate,
            (int64_t*)dist);
    }

    *dist <<= 4;
}

extern /*static*/ void model_rd_for_sb(
    PictureControlSet *picture_control_set_ptr,
    EbPictureBufferDesc *prediction_ptr,
    ModeDecisionContext *md_context_ptr,
    //const Av1Comp *const cpi,
    //BlockSize bsize,
    //Macroblock *x,
    //MacroBlockD *xd,
    int32_t plane_from,
    int32_t plane_to,
    int32_t *out_rate_sum,
    int64_t *out_dist_sum,
    int32_t *skip_txfm_sb,
    int64_t *skip_sse_sb,
    int32_t *plane_rate,
    int64_t *plane_sse,
    int64_t *plane_dist) {
    // Note our transform coeffs are 8 times an orthogonal transform.
    // Hence quantizer step is also 8 times. To get effective quantizer
    // we need to divide by 8 before sending to modeling function.
    int32_t plane;
    // const int32_t ref = xd->mi[0]->ref_frame[0];

    uint64_t rate_sum = 0;
    uint64_t dist_sum = 0;
    uint64_t total_sse = 0;

    EbPictureBufferDesc                  *input_picture_ptr = picture_control_set_ptr->parent_pcs_ptr->enhanced_picture_ptr;
    const uint32_t inputOriginIndex = (md_context_ptr->cu_origin_y + input_picture_ptr->origin_y) * input_picture_ptr->stride_y + (md_context_ptr->cu_origin_x + input_picture_ptr->origin_x);
    const uint32_t inputChromaOriginIndex = ((md_context_ptr->cu_origin_y + input_picture_ptr->origin_y) * input_picture_ptr->stride_cb + (md_context_ptr->cu_origin_x + input_picture_ptr->origin_x)) / 2;

    for (plane = plane_from; plane <= plane_to; ++plane) {
        // struct MacroblockPlane *const p = &x->plane[plane];
         // struct MacroblockdPlane *const pd = &xd->plane[plane];
         // const BlockSize bs = get_plane_block_size(bsize, pd);
        uint32_t sse;
        uint32_t rate;

        uint64_t dist;

        // if (x->skip_chroma_rd && plane) continue;

         // TODO(geza): Write direct sse functions that do not compute
         // variance as well.
        uint32_t offset;

        if (plane)
            offset = (prediction_ptr->origin_x + md_context_ptr->blk_geom->origin_x + (prediction_ptr->origin_y + md_context_ptr->blk_geom->origin_y) * prediction_ptr->stride_cb) / 2;
        else
            offset = prediction_ptr->origin_x + md_context_ptr->blk_geom->origin_x + (prediction_ptr->origin_y + md_context_ptr->blk_geom->origin_y) * prediction_ptr->stride_y;

        highbd_8_variance(
            plane == 0 ? (&(input_picture_ptr->buffer_y[inputOriginIndex])) : plane == 1 ? (&(input_picture_ptr->buffer_cb[inputChromaOriginIndex])) : (&(input_picture_ptr->buffer_cr[inputChromaOriginIndex])),
            plane == 0 ? input_picture_ptr->stride_y : plane == 1 ? input_picture_ptr->stride_cb : input_picture_ptr->stride_cr,
            plane == 0 ? (prediction_ptr->buffer_y + offset) : plane == 1 ? (prediction_ptr->buffer_cb + offset) : (prediction_ptr->buffer_cr + offset),
            plane == 0 ? prediction_ptr->stride_y : plane == 1 ? prediction_ptr->stride_cb : prediction_ptr->stride_cr,
            plane == 0 ? md_context_ptr->blk_geom->bwidth : md_context_ptr->blk_geom->bwidth_uv,
            plane == 0 ? md_context_ptr->blk_geom->bheight : md_context_ptr->blk_geom->bheight_uv,
            &sse
        );

        total_sse += sse;

        int32_t current_q_index = MAX(0, MIN(QINDEX_RANGE - 1, picture_control_set_ptr->parent_pcs_ptr->base_qindex));
        Dequants *const dequants = &picture_control_set_ptr->parent_pcs_ptr->deq;

        int16_t quantizer = dequants->y_dequant_Q3[current_q_index][1];
        model_rd_from_sse(
            plane == 0 ? md_context_ptr->blk_geom->bsize : md_context_ptr->blk_geom->bsize_uv,
            quantizer,
            sse,
            &rate,
            &dist);

        rate_sum += rate;
        dist_sum += dist;
        if (plane_rate) plane_rate[plane] = (int)rate;
        if (plane_sse) plane_sse[plane] = (int)sse;
        if (plane_dist) plane_dist[plane] = (int)dist;
    }

    *skip_txfm_sb = total_sse == 0;
    *skip_sse_sb = total_sse << 4;
    *out_rate_sum = (int32_t)rate_sum;
    *out_dist_sum = dist_sum;
}

/*static*/ /*INLINE*/ int32_t is_nontrans_global_motion(
    BlockSize sb_type,
    ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    PictureControlSet *picture_control_set_ptr
) {
    int32_t ref;

    // First check if all modes are GLOBALMV
    if (candidate_buffer_ptr->candidate_ptr->pred_mode != GLOBALMV && candidate_buffer_ptr->candidate_ptr->pred_mode != GLOBAL_GLOBALMV)
        return 0;

    if (MIN(mi_size_wide[sb_type], mi_size_high[sb_type]) < 2)
        return 0;
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, candidate_buffer_ptr->candidate_ptr->ref_frame_type);
    // Now check if all global motion is non translational
    for (ref = 0; ref < 1 + candidate_buffer_ptr->candidate_ptr->is_compound/*has_second_ref(mbmi)*/; ++ref) {
        if (picture_control_set_ptr->parent_pcs_ptr->global_motion[ref ? rf[1] : rf[0]].wmtype == TRANSLATION)
            //if (xd->global_motion[mbmi->ref_frame[ref]].wmtype == TRANSLATION)
            return 0;
    }
    return 1;
}
static INLINE int32_t av1_is_interp_needed(
    ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    PictureControlSet *picture_control_set_ptr,
    BlockSize bsize)
{
    if (candidate_buffer_ptr->candidate_ptr->merge_flag)
        return 0;

    if (candidate_buffer_ptr->candidate_ptr->motion_mode == WARPED_CAUSAL)
        return 0;

    if (is_nontrans_global_motion( bsize,
        candidate_buffer_ptr, picture_control_set_ptr))
        return 0;

    return 1;
}

#define DUAL_FILTER_SET_SIZE (SWITCHABLE_FILTERS * SWITCHABLE_FILTERS)
static const int32_t filter_sets[DUAL_FILTER_SET_SIZE][2] = {
  { 0, 0 }, { 0, 1 }, { 0, 2 }, { 1, 0 }, { 1, 1 },
  { 1, 2 }, { 2, 0 }, { 2, 1 }, { 2, 2 },
};

/*static*/ void interpolation_filter_search(
    PictureControlSet *picture_control_set_ptr,
    EbPictureBufferDesc *prediction_ptr,
    ModeDecisionContext *md_context_ptr,
    ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    MvUnit mv_unit,
    EbPictureBufferDesc  *ref_pic_list0,
    EbPictureBufferDesc  *ref_pic_list1,
    EbAsm asm_type,
    //Macroblock *const xd,
    //const Av1Comp *const cpi,
    //BlockSize bsize,
    //int32_t mi_row,
    //int32_t mi_col,
    //const BUFFER_SET *const tmp_dst,
    //BUFFER_SET *const orig_dst,
    /* InterpFilter (*const single_filter)[REF_FRAMES],*/
    int64_t *const rd,
    int32_t *const switchable_rate,
    int32_t *const skip_txfm_sb,
    int64_t *const skip_sse_sb) {
    const Av1Common *cm = picture_control_set_ptr->parent_pcs_ptr->av1_cm;//&cpi->common;
    EbBool use_uv = (md_context_ptr->blk_geom->has_uv && md_context_ptr->chroma_level <= CHROMA_MODE_1 &&
        picture_control_set_ptr->parent_pcs_ptr->interpolation_search_level != IT_SEARCH_FAST_LOOP_UV_BLIND) ? EB_TRUE : EB_FALSE;
    const int32_t num_planes = use_uv ? MAX_MB_PLANE : 1;

    int32_t i;
    int32_t tmp_rate;
    int64_t tmp_dist;

    //(void)single_filter;

    InterpFilter assign_filter = SWITCHABLE;

    if (cm->interp_filter != SWITCHABLE)
        assign_filter = cm->interp_filter;

    //set_default_interp_filters(mbmi, assign_filter);
    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters =//EIGHTTAP_REGULAR ;
        av1_broadcast_interp_filter(av1_unswitchable_filter(assign_filter));

    *switchable_rate = av1_get_switchable_rate(
        candidate_buffer_ptr,
        cm,
        md_context_ptr//,
        //x,
        //xd
    );

    av1_inter_prediction(
        picture_control_set_ptr,
        candidate_buffer_ptr->candidate_ptr->interp_filters,
        md_context_ptr->cu_ptr,
        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
        &mv_unit,
        0,
#if COMP_MODE
        candidate_buffer_ptr->candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
        &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
        &md_context_ptr->sb_ptr->tile_info,
        md_context_ptr->luma_recon_neighbor_array,
        md_context_ptr->cb_recon_neighbor_array,
        md_context_ptr->cr_recon_neighbor_array,
        candidate_buffer_ptr->candidate_ptr->is_interintra_used,
        candidate_buffer_ptr->candidate_ptr->interintra_mode,
        candidate_buffer_ptr->candidate_ptr->use_wedge_interintra,
        candidate_buffer_ptr->candidate_ptr->interintra_wedge_index,
#endif
        md_context_ptr->cu_origin_x,
        md_context_ptr->cu_origin_y,
        md_context_ptr->blk_geom->bwidth,
        md_context_ptr->blk_geom->bheight,
        ref_pic_list0,
        ref_pic_list1,
        prediction_ptr,
        md_context_ptr->blk_geom->origin_x,
        md_context_ptr->blk_geom->origin_y,
        use_uv,
        asm_type);

    model_rd_for_sb(
        picture_control_set_ptr,
        prediction_ptr,
        md_context_ptr,
        0,
        num_planes - 1,
        &tmp_rate,
        &tmp_dist,
        skip_txfm_sb,
        skip_sse_sb,
        NULL, NULL, NULL);

    *rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, *switchable_rate + tmp_rate, tmp_dist);
#if IFS_EARLY_EXIT
    if ((assign_filter == SWITCHABLE) && (tmp_dist < ((md_context_ptr->blk_geom->bheight * md_context_ptr->blk_geom->bwidth) << 8))) {
#else
    if (assign_filter == SWITCHABLE) {
#endif
        // do interp_filter search

        if (av1_is_interp_needed(candidate_buffer_ptr, picture_control_set_ptr, md_context_ptr->blk_geom->bsize) /*&& av1_is_interp_search_needed(xd)*/) {
            const int32_t filter_set_size = DUAL_FILTER_SET_SIZE;
            int32_t best_in_temp = 0;
            uint32_t best_filters = 0;// mbmi->interp_filters;

            if (picture_control_set_ptr->parent_pcs_ptr->interpolation_search_level &&
                picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->seq_header.enable_dual_filter) {
                int32_t tmp_skip_sb = 0;
                int64_t tmp_skip_sse = INT64_MAX;
                int32_t tmp_rs;
                int64_t tmp_rd;

                // default to (R,R): EIGHTTAP_REGULARxEIGHTTAP_REGULAR
                int32_t best_dual_mode = 0;
                // Find best of {R}x{R,Sm,Sh}
                // EIGHTTAP_REGULAR mode is calculated beforehand
                for (i = 1; i < SWITCHABLE_FILTERS; ++i) {
                    tmp_skip_sb = 0;
                    tmp_skip_sse = INT64_MAX;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = (InterpFilter)
                        av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );

                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);
                    av1_inter_prediction(
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        md_context_ptr->cu_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        &mv_unit,
                        0,
#if COMP_MODE
                        candidate_buffer_ptr->candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
                        &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
                        &md_context_ptr->sb_ptr->tile_info,
                        md_context_ptr->luma_recon_neighbor_array,
                        md_context_ptr->cb_recon_neighbor_array,
                        md_context_ptr->cr_recon_neighbor_array,
                        candidate_buffer_ptr->candidate_ptr->is_interintra_used,
                        candidate_buffer_ptr->candidate_ptr->interintra_mode,
                        candidate_buffer_ptr->candidate_ptr->use_wedge_interintra,
                        candidate_buffer_ptr->candidate_ptr->interintra_wedge_index,
#endif
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        best_dual_mode = i;

                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                          restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                          restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }

                // From best of horizontal EIGHTTAP_REGULAR modes, check vertical modes
                for (i = best_dual_mode + SWITCHABLE_FILTERS; i < filter_set_size;
                    i += SWITCHABLE_FILTERS) {
                    tmp_skip_sb = 0;
                    tmp_skip_sse = INT64_MAX;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters =
                        av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );
                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);

                    av1_inter_prediction(
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        md_context_ptr->cu_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        &mv_unit,
                        0,
#if COMP_MODE
                        candidate_buffer_ptr->candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
                        &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
                        &md_context_ptr->sb_ptr->tile_info,
                        md_context_ptr->luma_recon_neighbor_array,
                        md_context_ptr->cb_recon_neighbor_array,
                        md_context_ptr->cr_recon_neighbor_array,
                        candidate_buffer_ptr->candidate_ptr->is_interintra_used,
                        candidate_buffer_ptr->candidate_ptr->interintra_mode,
                        candidate_buffer_ptr->candidate_ptr->use_wedge_interintra,
                        candidate_buffer_ptr->candidate_ptr->interintra_wedge_index,
#endif
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                          restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                          restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }
            }
            else {
                // EIGHTTAP_REGULAR mode is calculated beforehand
                for (i = 1; i < filter_set_size; ++i) {
                    int32_t tmp_skip_sb = 0;
                    int64_t tmp_skip_sse = INT64_MAX;
                    int32_t tmp_rs;
                    int64_t tmp_rd;

                    if (/*cm->seq_params.enable_dual_filter*/picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->seq_header.enable_dual_filter == 0)
                        if (filter_sets[i][0] != filter_sets[i][1]) continue;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );
                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);

                    av1_inter_prediction(
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        md_context_ptr->cu_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        &mv_unit,
                        0,
#if COMP_MODE
                        candidate_buffer_ptr->candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
                        &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
                        &md_context_ptr->sb_ptr->tile_info,
                        md_context_ptr->luma_recon_neighbor_array,
                        md_context_ptr->cb_recon_neighbor_array,
                        md_context_ptr->cr_recon_neighbor_array,
                        candidate_buffer_ptr->candidate_ptr->is_interintra_used,
                        candidate_buffer_ptr->candidate_ptr->interintra_mode,
                        candidate_buffer_ptr->candidate_ptr->use_wedge_interintra,
                        candidate_buffer_ptr->candidate_ptr->interintra_wedge_index,
#endif
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                          restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                          restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }
            }

            /*if (best_in_temp) {
              restore_dst_buf(xd, *tmp_dst, num_planes);
            } else {
              restore_dst_buf(xd, *orig_dst, num_planes);
            }*/
            /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = best_filters;
        }
        else {
            candidate_buffer_ptr->candidate_ptr->interp_filters = 0;

            /*assert(mbmi->cu_ptr->interp_filters ==
                   av1_broadcast_interp_filter(EIGHTTAP_REGULAR));*/
        }
    }
    //  return 0;
}

#if !REMOVE_UNPACK_REF
/*static*/ void interpolation_filter_search_HBD(
    PictureControlSet *picture_control_set_ptr,
    EbPictureBufferDesc *prediction_ptr,
    ModeDecisionContext *md_context_ptr,
    ModeDecisionCandidateBuffer *candidate_buffer_ptr,
    MvUnit mv_unit,
    EbPictureBufferDesc  *ref_pic_list0,
    EbPictureBufferDesc  *ref_pic_list1,
    EbAsm asm_type,
    //Macroblock *const xd,
    //const Av1Comp *const cpi,
    //BlockSize bsize,
    //int32_t mi_row,
    //int32_t mi_col,
    //const BUFFER_SET *const tmp_dst,
    //BUFFER_SET *const orig_dst,
    /* InterpFilter (*const single_filter)[REF_FRAMES],*/
    int64_t *const rd,
    int32_t *const switchable_rate,
    int32_t *const skip_txfm_sb,
    int64_t *const skip_sse_sb) {
    const Av1Common *cm = picture_control_set_ptr->parent_pcs_ptr->av1_cm;//&cpi->common;

    EbBool use_uv = (md_context_ptr->blk_geom->has_uv && md_context_ptr->chroma_level <= CHROMA_MODE_1 &&
        picture_control_set_ptr->parent_pcs_ptr->interpolation_search_level != IT_SEARCH_FAST_LOOP_UV_BLIND) ? EB_TRUE : EB_FALSE;
    const int32_t num_planes = use_uv ? MAX_MB_PLANE : 1;
    int32_t i;
    int32_t tmp_rate;
    int64_t tmp_dist;

    //(void)single_filter;

    InterpFilter assign_filter = SWITCHABLE;

    if (cm->interp_filter != SWITCHABLE)
        assign_filter = cm->interp_filter;

    //set_default_interp_filters(mbmi, assign_filter);
    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters =//EIGHTTAP_REGULAR ;
        av1_broadcast_interp_filter(av1_unswitchable_filter(assign_filter));

    *switchable_rate = av1_get_switchable_rate(
        candidate_buffer_ptr,
        cm,
        md_context_ptr//,
        //x,
        //xd
    );

    AV1InterPrediction10BitMD(
        candidate_buffer_ptr->candidate_ptr->interp_filters,
        picture_control_set_ptr,
        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
        md_context_ptr,
        md_context_ptr->cu_ptr,
        &mv_unit,
        0,//use_intra_bc
        md_context_ptr->cu_origin_x,
        md_context_ptr->cu_origin_y,
        md_context_ptr->blk_geom->bwidth,
        md_context_ptr->blk_geom->bheight,
        ref_pic_list0,
        ref_pic_list1,
        prediction_ptr,
        md_context_ptr->blk_geom->origin_x,
        md_context_ptr->blk_geom->origin_y,
        use_uv,
        asm_type);

    model_rd_for_sb(
        picture_control_set_ptr,
        prediction_ptr,
        md_context_ptr,
        0,
        num_planes - 1,
        &tmp_rate,
        &tmp_dist,
        skip_txfm_sb,
        skip_sse_sb,
        NULL, NULL, NULL);

    *rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, *switchable_rate + tmp_rate, tmp_dist);

    if (assign_filter == SWITCHABLE) {
        // do interp_filter search
        if (av1_is_interp_needed(candidate_buffer_ptr, picture_control_set_ptr, md_context_ptr->blk_geom->bsize) /*&& av1_is_interp_search_needed(xd)*/) {
            const int32_t filter_set_size = DUAL_FILTER_SET_SIZE;
            int32_t best_in_temp = 0;
            uint32_t best_filters = 0;// mbmi->interp_filters;

            if (picture_control_set_ptr->parent_pcs_ptr->interpolation_search_level &&
                picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->enable_dual_filter) {
                int32_t tmp_skip_sb = 0;
                int64_t tmp_skip_sse = INT64_MAX;
                int32_t tmp_rs;
                int64_t tmp_rd;

                // default to (R,R): EIGHTTAP_REGULARxEIGHTTAP_REGULAR
                int32_t best_dual_mode = 0;
                // Find best of {R}x{R,Sm,Sh}
                // EIGHTTAP_REGULAR mode is calculated beforehand
                for (i = 1; i < SWITCHABLE_FILTERS; ++i) {
                    tmp_skip_sb = 0;
                    tmp_skip_sse = INT64_MAX;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = (InterpFilter)
                        av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );

                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);

                    AV1InterPrediction10BitMD(
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        md_context_ptr,
                        md_context_ptr->cu_ptr,
                        &mv_unit,
                        0,//use_intra_bc
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        best_dual_mode = i;

                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                        restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                        restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }

                // From best of horizontal EIGHTTAP_REGULAR modes, check vertical modes
                for (i = best_dual_mode + SWITCHABLE_FILTERS; i < filter_set_size;
                    i += SWITCHABLE_FILTERS) {
                    tmp_skip_sb = 0;
                    tmp_skip_sse = INT64_MAX;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters =
                        av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );
                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);

                    AV1InterPrediction10BitMD(
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        md_context_ptr,
                        md_context_ptr->cu_ptr,
                        &mv_unit,
                        0,//use_intra_bc
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                        restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                        restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }
            }
            else {
                // EIGHTTAP_REGULAR mode is calculated beforehand
                for (i = 1; i < filter_set_size; ++i) {
                    int32_t tmp_skip_sb = 0;
                    int64_t tmp_skip_sse = INT64_MAX;
                    int32_t tmp_rs;
                    int64_t tmp_rd;

                    if (/*cm->seq_params.enable_dual_filter*/picture_control_set_ptr->parent_pcs_ptr->sequence_control_set_ptr->enable_dual_filter == 0)
                        if (filter_sets[i][0] != filter_sets[i][1]) continue;

                    /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = av1_make_interp_filters((InterpFilter)filter_sets[i][0], (InterpFilter)filter_sets[i][1]);

                    tmp_rs = av1_get_switchable_rate(
                        candidate_buffer_ptr,
                        cm,
                        md_context_ptr//,
                        //x,
                        //xd
                    );
                    //av1_build_inter_predictors_sb(
                    //                              cm,
                    //                              xd,
                    //                              mi_row,
                    //                              mi_col,
                    //                              orig_dst,
                    //                              bsize);

                    AV1InterPrediction10BitMD(
                        candidate_buffer_ptr->candidate_ptr->interp_filters,
                        picture_control_set_ptr,
                        candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                        md_context_ptr,
                        md_context_ptr->cu_ptr,
                        &mv_unit,
                        0,//use_intra_bc
                        md_context_ptr->cu_origin_x,
                        md_context_ptr->cu_origin_y,
                        md_context_ptr->blk_geom->bwidth,
                        md_context_ptr->blk_geom->bheight,
                        ref_pic_list0,
                        ref_pic_list1,
                        prediction_ptr,
                        md_context_ptr->blk_geom->origin_x,
                        md_context_ptr->blk_geom->origin_y,
                        use_uv,
                        asm_type);

                    model_rd_for_sb(
                        picture_control_set_ptr,
                        prediction_ptr,
                        md_context_ptr,
                        0,
                        num_planes - 1,
                        &tmp_rate,
                        &tmp_dist,
                        &tmp_skip_sb,
                        &tmp_skip_sse,
                        NULL, NULL, NULL);
                    tmp_rd = RDCOST(md_context_ptr->full_lambda/*x->rdmult*/, tmp_rs + tmp_rate, tmp_dist);

                    if (tmp_rd < *rd) {
                        *rd = tmp_rd;
                        *switchable_rate = tmp_rs;
                        best_filters = /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters;
                        *skip_txfm_sb = tmp_skip_sb;
                        *skip_sse_sb = tmp_skip_sse;
                        best_in_temp = !best_in_temp;
                        /*if (best_in_temp) {
                        restore_dst_buf(xd, *orig_dst, num_planes);
                        } else {
                        restore_dst_buf(xd, *tmp_dst, num_planes);
                        }*/
                    }
                }
            }

            /*if (best_in_temp) {
            restore_dst_buf(xd, *tmp_dst, num_planes);
            } else {
            restore_dst_buf(xd, *orig_dst, num_planes);
            }*/
            /*mbmi*/candidate_buffer_ptr->candidate_ptr->interp_filters = best_filters;
        }
        else {
            candidate_buffer_ptr->candidate_ptr->interp_filters = 0;

            /*assert(mbmi->cu_ptr->interp_filters ==
            av1_broadcast_interp_filter(EIGHTTAP_REGULAR));*/
        }
    }
    //  return 0;
}

#endif

EbErrorType inter_pu_prediction_av1(
    ModeDecisionContext                  *md_context_ptr,
    PictureControlSet                    *picture_control_set_ptr,
    ModeDecisionCandidateBuffer          *candidate_buffer_ptr,
    EbAsm                                   asm_type)
{
    EbErrorType            return_error = EB_ErrorNone;
    EbPictureBufferDesc  *ref_pic_list0;
    EbPictureBufferDesc  *ref_pic_list1 = NULL;
    ModeDecisionCandidate *const candidate_ptr = candidate_buffer_ptr->candidate_ptr;

    Mv mv_0;
    Mv mv_1;
    mv_0.x = candidate_buffer_ptr->candidate_ptr->motion_vector_xl0;
    mv_0.y = candidate_buffer_ptr->candidate_ptr->motion_vector_yl0;
    mv_1.x = candidate_buffer_ptr->candidate_ptr->motion_vector_xl1;
    mv_1.y = candidate_buffer_ptr->candidate_ptr->motion_vector_yl1;
    MvUnit mv_unit;
    mv_unit.pred_direction = candidate_buffer_ptr->candidate_ptr->prediction_direction[md_context_ptr->pu_itr];
    mv_unit.mv[0] = mv_0;
    mv_unit.mv[1] = mv_1;

    SequenceControlSet* sequence_control_set_ptr = ((SequenceControlSet*)(picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr));
    EbBool  is16bit = (EbBool)(sequence_control_set_ptr->static_config.encoder_bit_depth > EB_8BIT);

    int64_t skip_sse_sb = INT64_MAX;
    int32_t skip_txfm_sb = 0;
    int32_t rs = 0;
    int64_t rd = INT64_MAX;

#if OPT_IFS
    if (mv_unit.pred_direction == 0) {
        if (mv_unit.mv[0].x % 8 == 0 && mv_unit.mv[0].y % 8 == 0)
            md_context_ptr->skip_interpolation_search = 1;
    }
    if (mv_unit.pred_direction == 1) {
        if (mv_unit.mv[1].x % 8 == 0 && mv_unit.mv[1].y % 8 == 0)
            md_context_ptr->skip_interpolation_search = 1;
    }
    if (mv_unit.pred_direction == 0) {
        if (mv_unit.mv[0].x % 8 == 0 &&
            mv_unit.mv[0].y % 8 == 0 &&
            mv_unit.mv[1].x % 8 == 0 &&
            mv_unit.mv[1].y % 8 == 0)
            md_context_ptr->skip_interpolation_search = 1;
    }
    if (!av1_is_interp_needed(candidate_buffer_ptr, picture_control_set_ptr, md_context_ptr->blk_geom->bsize))
        md_context_ptr->skip_interpolation_search = 1;
#endif

    if (candidate_buffer_ptr->candidate_ptr->use_intrabc)
    {
#if REMOVE_UNPACK_REF
        ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)->reference_picture;
        av1_inter_prediction(
            picture_control_set_ptr,
            candidate_buffer_ptr->candidate_ptr->interp_filters,
            md_context_ptr->cu_ptr,
            candidate_buffer_ptr->candidate_ptr->ref_frame_type,
            &mv_unit,
            1,//use_intrabc
#if COMP_MODE
            1,//1 for avg
#endif
#if COMP_DIFF
            &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
            NULL,
            NULL,
            NULL,
            NULL,
            0,
            0,
            0,
            0,
#endif
            md_context_ptr->cu_origin_x,
            md_context_ptr->cu_origin_y,
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            ref_pic_list0,
            0,//ref_pic_list1,
            candidate_buffer_ptr->prediction_ptr,
            md_context_ptr->blk_geom->origin_x,
            md_context_ptr->blk_geom->origin_y,
#if RE_FACTURE_PRED_KERNEL
            md_context_ptr->chroma_level <= CHROMA_MODE_1 && md_context_ptr->shut_chroma_comp == EB_FALSE,
#else
            md_context_ptr->chroma_level <= CHROMA_MODE_1,
#endif
            asm_type);
        return return_error;
#else
        if (is16bit) {
#if !UNPACK_REF_POST_EP
            ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)->reference_picture16bit;
#endif
            AV1InterPrediction10BitMD(
                candidate_buffer_ptr->candidate_ptr->interp_filters,
                picture_control_set_ptr,
                candidate_buffer_ptr->candidate_ptr->ref_frame_type,
                md_context_ptr,
                md_context_ptr->cu_ptr,
                &mv_unit,
                1,//use_intrabc
                md_context_ptr->cu_origin_x,
                md_context_ptr->cu_origin_y,
                md_context_ptr->blk_geom->bwidth,
                md_context_ptr->blk_geom->bheight,
                ref_pic_list0,
                0,// ref_pic_list1,
                candidate_buffer_ptr->prediction_ptr,
                md_context_ptr->blk_geom->origin_x,
                md_context_ptr->blk_geom->origin_y,
                md_context_ptr->chroma_level <= CHROMA_MODE_1,
                asm_type);

            return return_error;
        }
        else {
#if !UNPACK_REF_POST_EP
            ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->parent_pcs_ptr->reference_picture_wrapper_ptr->object_ptr)->reference_picture;
#endif
        av1_inter_prediction(
            picture_control_set_ptr,
            candidate_buffer_ptr->candidate_ptr->interp_filters,
            md_context_ptr->cu_ptr,
            candidate_buffer_ptr->candidate_ptr->ref_frame_type,
            &mv_unit,
            1,//use_intrabc
            md_context_ptr->cu_origin_x,
            md_context_ptr->cu_origin_y,
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            ref_pic_list0,
            0,//ref_pic_list1,
            candidate_buffer_ptr->prediction_ptr,
            md_context_ptr->blk_geom->origin_x,
            md_context_ptr->blk_geom->origin_y,
                md_context_ptr->chroma_level <= CHROMA_MODE_1,
            asm_type);

        return return_error;
        }
#endif
    }

#if UNPACK_REF_POST_EP
 #if MRP_MD
     int8_t ref_idx_l0 = candidate_buffer_ptr->candidate_ptr->ref_frame_index_l0;
     int8_t ref_idx_l1 = candidate_buffer_ptr->candidate_ptr->ref_frame_index_l1;
    // MRP_MD_UNI_DIR_BIPRED
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, candidate_buffer_ptr->candidate_ptr->ref_frame_type);
    uint8_t list_idx0, list_idx1;
    list_idx0 = get_list_idx(rf[0]);
    if (rf[1] == NONE_FRAME)
        list_idx1 = get_list_idx(rf[0]);
    else
        list_idx1 = get_list_idx(rf[1]);
    assert(list_idx0 < MAX_NUM_OF_REF_PIC_LIST);
    assert(list_idx1 < MAX_NUM_OF_REF_PIC_LIST);
    if (ref_idx_l0 >= 0)
        ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx0][ref_idx_l0]->object_ptr)->reference_picture;
    else
        ref_pic_list0 = (EbPictureBufferDesc*)EB_NULL;
    if (ref_idx_l1 >= 0)
        ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[list_idx1][ref_idx_l1]->object_ptr)->reference_picture;
    else
        ref_pic_list1 = (EbPictureBufferDesc*)EB_NULL;

 #else
    ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_0]->object_ptr)->reference_picture;
    if (picture_control_set_ptr->slice_type == B_SLICE)
        ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_1]->object_ptr)->reference_picture;
 #endif
#else
    if (is16bit) {
        ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_0]->object_ptr)->reference_picture16bit;
        if (picture_control_set_ptr->slice_type == B_SLICE)
            ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_1]->object_ptr)->reference_picture16bit;
    } else {
        ref_pic_list0 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_0]->object_ptr)->reference_picture;
        if (picture_control_set_ptr->slice_type == B_SLICE)
            ref_pic_list1 = ((EbReferenceObject*)picture_control_set_ptr->ref_pic_ptr_array[REF_LIST_1]->object_ptr)->reference_picture;
    }
#endif
    if (picture_control_set_ptr->parent_pcs_ptr->allow_warped_motion
        && candidate_ptr->motion_mode != WARPED_CAUSAL)
            wm_count_samples(
                md_context_ptr->cu_ptr,
                md_context_ptr->blk_geom,
                md_context_ptr->cu_origin_x,
                md_context_ptr->cu_origin_y,
                candidate_ptr->ref_frame_type,
                picture_control_set_ptr,
                &candidate_ptr->num_proj_ref);

    if (candidate_ptr->motion_mode == WARPED_CAUSAL) {
        if (is16bit) {
            warped_motion_prediction_md(
                &mv_unit,
                md_context_ptr,
                md_context_ptr->cu_origin_x,
                md_context_ptr->cu_origin_y,
                md_context_ptr->cu_ptr,
                md_context_ptr->blk_geom,
                ref_pic_list0,
                candidate_buffer_ptr->prediction_ptr,
                md_context_ptr->blk_geom->origin_x,
                md_context_ptr->blk_geom->origin_y,
                &candidate_ptr->wm_params,
                asm_type);
        } else {
            assert(ref_pic_list0 != NULL);
            warped_motion_prediction(
                &mv_unit,
                md_context_ptr->cu_origin_x,
                md_context_ptr->cu_origin_y,
                md_context_ptr->cu_ptr,
                md_context_ptr->blk_geom,
                ref_pic_list0,
                candidate_buffer_ptr->prediction_ptr,
                md_context_ptr->blk_geom->origin_x,
                md_context_ptr->blk_geom->origin_y,
                &candidate_ptr->wm_params,
                (uint8_t) sequence_control_set_ptr->static_config.encoder_bit_depth,
#if RE_FACTURE_PRED_KERNEL
                md_context_ptr->chroma_level <= CHROMA_MODE_1 && md_context_ptr->shut_chroma_comp == EB_FALSE,
#else
                md_context_ptr->chroma_level <= CHROMA_MODE_1,
#endif
                asm_type);
        }
        return return_error;
    }

    uint16_t capped_size = md_context_ptr->interpolation_filter_search_blk_size == 0 ? 4 :
                           md_context_ptr->interpolation_filter_search_blk_size == 1 ? 8 : 16 ;

#if REMOVE_UNPACK_REF
#if !PRE_BILINEAR_CLEAN_UP
        candidate_buffer_ptr->candidate_ptr->interp_filters = 0;
#endif
        if (!md_context_ptr->skip_interpolation_search) {
            if (md_context_ptr->blk_geom->bwidth > capped_size && md_context_ptr->blk_geom->bheight > capped_size)
                interpolation_filter_search(
                    picture_control_set_ptr,
                    candidate_buffer_ptr->prediction_ptr_temp,
                    md_context_ptr,
                    candidate_buffer_ptr,
                    mv_unit,
                    ref_pic_list0,
                    ref_pic_list1,
                    asm_type,
                    &rd,
                    &rs,
                    &skip_txfm_sb,
                    &skip_sse_sb);
        }

        av1_inter_prediction(
            picture_control_set_ptr,
            candidate_buffer_ptr->candidate_ptr->interp_filters,
            md_context_ptr->cu_ptr,
            candidate_buffer_ptr->candidate_ptr->ref_frame_type,
            &mv_unit,
            candidate_buffer_ptr->candidate_ptr->use_intrabc,
#if COMP_MODE
            candidate_buffer_ptr->candidate_ptr->compound_idx,
#endif
#if COMP_DIFF
            &candidate_buffer_ptr->candidate_ptr->interinter_comp,
#endif
#if II_ED
            &md_context_ptr->sb_ptr->tile_info,
            md_context_ptr->luma_recon_neighbor_array,
            md_context_ptr->cb_recon_neighbor_array,
            md_context_ptr->cr_recon_neighbor_array,
            candidate_ptr->is_interintra_used,
            candidate_ptr->interintra_mode,
            candidate_ptr->use_wedge_interintra,
            candidate_ptr->interintra_wedge_index,
#endif
            md_context_ptr->cu_origin_x,
            md_context_ptr->cu_origin_y,
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            ref_pic_list0,
            ref_pic_list1,
            candidate_buffer_ptr->prediction_ptr,
            md_context_ptr->blk_geom->origin_x,
            md_context_ptr->blk_geom->origin_y,
#if RE_FACTURE_PRED_KERNEL
            md_context_ptr->chroma_level <= CHROMA_MODE_1 && md_context_ptr->shut_chroma_comp == EB_FALSE,
#else
        md_context_ptr->chroma_level <= CHROMA_MODE_1,
#endif
        asm_type);
#else
    if (is16bit) {
        candidate_buffer_ptr->candidate_ptr->interp_filters = 0;
        if (!md_context_ptr->skip_interpolation_search) {
            if (md_context_ptr->blk_geom->bwidth > capped_size && md_context_ptr->blk_geom->bheight > capped_size)
                interpolation_filter_search_HBD(
                    picture_control_set_ptr,
                    candidate_buffer_ptr->predictionPtrTemp,
                    md_context_ptr,
                    candidate_buffer_ptr,
                    mv_unit,
                    ref_pic_list0,
                    ref_pic_list1,
                    asm_type,
                    &rd,
                    &rs,
                    &skip_txfm_sb,
                    &skip_sse_sb);
        }

        AV1InterPrediction10BitMD(
            candidate_buffer_ptr->candidate_ptr->interp_filters,
            picture_control_set_ptr,
            candidate_buffer_ptr->candidate_ptr->ref_frame_type,
            md_context_ptr,
            md_context_ptr->cu_ptr,
            &mv_unit,
            0,//use_intra_bc
            md_context_ptr->cu_origin_x,
            md_context_ptr->cu_origin_y,
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            ref_pic_list0,
            ref_pic_list1,
            candidate_buffer_ptr->prediction_ptr,
            md_context_ptr->blk_geom->origin_x,
            md_context_ptr->blk_geom->origin_y,
            md_context_ptr->chroma_level <= CHROMA_MODE_1,
            asm_type);
    } else {
        candidate_buffer_ptr->candidate_ptr->interp_filters = 0;
        if (!md_context_ptr->skip_interpolation_search) {
            if (md_context_ptr->blk_geom->bwidth > capped_size && md_context_ptr->blk_geom->bheight > capped_size)
                interpolation_filter_search(
                    picture_control_set_ptr,
                    candidate_buffer_ptr->predictionPtrTemp,
                    md_context_ptr,
                    candidate_buffer_ptr,
                    mv_unit,
                    ref_pic_list0,
                    ref_pic_list1,
                    asm_type,
                    &rd,
                    &rs,
                    &skip_txfm_sb,
                    &skip_sse_sb);
    }

        av1_inter_prediction(
            picture_control_set_ptr,
            candidate_buffer_ptr->candidate_ptr->interp_filters,
            md_context_ptr->cu_ptr,
            candidate_buffer_ptr->candidate_ptr->ref_frame_type,
            &mv_unit,
            candidate_buffer_ptr->candidate_ptr->use_intrabc,
            md_context_ptr->cu_origin_x,
            md_context_ptr->cu_origin_y,
            md_context_ptr->blk_geom->bwidth,
            md_context_ptr->blk_geom->bheight,
            ref_pic_list0,
            ref_pic_list1,
            candidate_buffer_ptr->prediction_ptr,
            md_context_ptr->blk_geom->origin_x,
            md_context_ptr->blk_geom->origin_y,
            md_context_ptr->chroma_level <= CHROMA_MODE_1,
            asm_type);
    }
#endif
    return return_error;
}

#if !UNPACK_REF_POST_EP
EbErrorType inter_prediction_context_ctor(
    InterPredictionContext **inter_prediction_context,
    EbColorFormat              color_format,
    uint16_t                   max_cu_width,
    uint16_t                   max_cu_height)

{
    EbErrorType              return_error = EB_ErrorNone;
    InterPredictionContext *context_ptr;
    EB_MALLOC(InterPredictionContext*, context_ptr, sizeof(InterPredictionContext), EB_N_PTR);

    (*inter_prediction_context) = context_ptr;

    return_error = motion_compensation_prediction_context_ctor(
        &context_ptr->mcp_context,
        color_format,
        max_cu_width,
        max_cu_height);

    if (return_error == EB_ErrorInsufficientResources)
        return EB_ErrorInsufficientResources;
    return EB_ErrorNone;
}

void RoundMvOnTheFly(
    int16_t *motionVector_x,
    int16_t *motionVector_y)
{
    *motionVector_x = (*motionVector_x + 2)&~0x03;
    *motionVector_y = (*motionVector_y + 2)&~0x03;

    return;
}
#endif

/***************************************************
*  PreLoad Reference Block  for 16bit mode
***************************************************/
void UnPackReferenceLumaBlock(
    EbPictureBufferDesc *refFramePic,
    uint32_t                 pos_x,
    uint32_t                 pos_y,
    uint32_t                 pu_width,
    uint32_t                 pu_height,
    EbPictureBufferDesc *dst,
    EbBool                sub_pred,
    EbAsm                 asm_type)
{
    pu_width += 4;
    pu_height += 4;
    uint32_t inPosx = (pos_x >> 2) - 2;
    uint32_t inPosy = (pos_y >> 2) - 2;
    uint16_t *ptr16 = (uint16_t *)refFramePic->buffer_y + inPosx + inPosy * refFramePic->stride_y;

    extract8_bitdata_safe_sub(
        ptr16,
        refFramePic->stride_y << sub_pred,
        dst->buffer_y,
        dst->stride_y << sub_pred,
        pu_width,
        pu_height >> sub_pred,
        sub_pred,
        asm_type
    );
}

/** choose_mvp_idx_v2 function is used to choose the best AMVP candidate.
    @param *candidate_ptr(output)
        candidate_ptr points to the prediction result.
    @param cu_ptr(input)
        pointer to the CU where the target PU belongs to.
    @param *pu_index(input)
        the index of the PU inside a CU
    @param ref0AMVPCandArray(input)
    @param ref0_num_available_amvp_cand(input)
    @param ref1AMVPCandArray(input)
    @param ref1NumAvailableAMVPCand(input)
 */
EbErrorType choose_mvp_idx_v2(
    ModeDecisionCandidate  *candidate_ptr,
    uint32_t                    cu_origin_x,
    uint32_t                    cu_origin_y,
    uint32_t                    pu_index,
    uint32_t                    tb_size,
    int16_t                   *ref0_amvp_cand_array_x,
    int16_t                   *ref0_amvp_cand_array_y,
    uint32_t                    ref0_num_available_amvp_cand,
    int16_t                   *ref1_amvp_cand_array_x,
    int16_t                   *ref1_amvp_cand_array_y,
    uint32_t                    ref1NumAvailableAMVPCand,
    PictureControlSet      *picture_control_set_ptr)
{
    EbErrorType  return_error = EB_ErrorNone;
    uint8_t         mvpRef0Idx;
    uint8_t         mvpRef1Idx;

    uint32_t        picture_width = ((SequenceControlSet*)picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr)->seq_header.max_frame_width;
    uint32_t        picture_height = ((SequenceControlSet*)picture_control_set_ptr->sequence_control_set_wrapper_ptr->object_ptr)->seq_header.max_frame_height;

    uint32_t   mvd0, mvd1;

    switch (candidate_ptr->prediction_direction[pu_index]) {
    case UNI_PRED_LIST_0:
        // Clip the input MV
        clip_mv(
            cu_origin_x,
            cu_origin_y,
            &candidate_ptr->motion_vector_xl0,
            &candidate_ptr->motion_vector_yl0,
            picture_width,
            picture_height,
            tb_size);

        // Choose the AMVP candidate
        switch (ref0_num_available_amvp_cand) {
        case 0:
        case 1:
            //mvpRef0Idx = 0;
            candidate_ptr->motion_vector_pred_idx[REF_LIST_0] = 0;
            candidate_ptr->motion_vector_pred_x[REF_LIST_0] = ref0_amvp_cand_array_x[0];
            candidate_ptr->motion_vector_pred_y[REF_LIST_0] = ref0_amvp_cand_array_y[0];
            break;
        case 2:

            mvd0 = EB_ABS_DIFF(ref0_amvp_cand_array_x[0], candidate_ptr->motion_vector_xl0) +
                EB_ABS_DIFF(ref0_amvp_cand_array_y[0], candidate_ptr->motion_vector_yl0);

            mvd1 = EB_ABS_DIFF(ref0_amvp_cand_array_x[1], candidate_ptr->motion_vector_xl0) +
                EB_ABS_DIFF(ref0_amvp_cand_array_y[1], candidate_ptr->motion_vector_yl0);

            mvpRef0Idx = ((mvd0) <= (mvd1)) ? 0 : 1;

            candidate_ptr->motion_vector_pred_idx[REF_LIST_0] = mvpRef0Idx;
            candidate_ptr->motion_vector_pred_x[REF_LIST_0] = ref0_amvp_cand_array_x[mvpRef0Idx];
            candidate_ptr->motion_vector_pred_y[REF_LIST_0] = ref0_amvp_cand_array_y[mvpRef0Idx];
            break;
        default:
            break;
        }

        break;

    case UNI_PRED_LIST_1:

        // Clip the input MV
        clip_mv(
            cu_origin_x,
            cu_origin_y,
            &candidate_ptr->motion_vector_xl1,
            &candidate_ptr->motion_vector_yl1,
            picture_width,
            picture_height,
            tb_size);

        // Choose the AMVP candidate
        switch (ref1NumAvailableAMVPCand) {
        case 0:
        case 1:
            //mvpRef1Idx = 0;
            candidate_ptr->motion_vector_pred_idx[REF_LIST_1] = 0;
            candidate_ptr->motion_vector_pred_x[REF_LIST_1] = ref1_amvp_cand_array_x[0];
            candidate_ptr->motion_vector_pred_y[REF_LIST_1] = ref1_amvp_cand_array_y[0];
            break;
        case 2:

            mvd0 = EB_ABS_DIFF(ref1_amvp_cand_array_x[0], candidate_ptr->motion_vector_xl1) +
                EB_ABS_DIFF(ref1_amvp_cand_array_y[0], candidate_ptr->motion_vector_yl1);

            mvd1 = EB_ABS_DIFF(ref1_amvp_cand_array_x[1], candidate_ptr->motion_vector_xl1) +
                EB_ABS_DIFF(ref1_amvp_cand_array_y[1], candidate_ptr->motion_vector_yl1);

            mvpRef1Idx = ((mvd0) <= (mvd1)) ? 0 : 1;

            candidate_ptr->motion_vector_pred_idx[REF_LIST_1] = mvpRef1Idx;
            candidate_ptr->motion_vector_pred_x[REF_LIST_1] = ref1_amvp_cand_array_x[mvpRef1Idx];
            candidate_ptr->motion_vector_pred_y[REF_LIST_1] = ref1_amvp_cand_array_y[mvpRef1Idx];
            break;
        default:
            break;
        }

        // MVP in ref_pic_list0
        //mvpRef0Idx = 0;
        //candidate_ptr->motion_vector_pred_idx[REF_LIST_0][pu_index] = mvpRef0Idx;
        //candidate_ptr->motion_vector_pred_x[REF_LIST_0][pu_index]  = 0;
        //candidate_ptr->motion_vector_pred_y[REF_LIST_0][pu_index]  = 0;

        break;

    case BI_PRED:

        // Choose the MVP in list0
        // Clip the input MV
        clip_mv(
            cu_origin_x,
            cu_origin_y,
            &candidate_ptr->motion_vector_xl0,
            &candidate_ptr->motion_vector_yl0,
            picture_width,
            picture_height,
            tb_size);

        // Choose the AMVP candidate
        switch (ref0_num_available_amvp_cand) {
        case 0:
        case 1:
            //mvpRef0Idx = 0;
            candidate_ptr->motion_vector_pred_idx[REF_LIST_0] = 0;
            candidate_ptr->motion_vector_pred_x[REF_LIST_0] = ref0_amvp_cand_array_x[0];
            candidate_ptr->motion_vector_pred_y[REF_LIST_0] = ref0_amvp_cand_array_y[0];
            break;
        case 2:

            mvd0 = EB_ABS_DIFF(ref0_amvp_cand_array_x[0], candidate_ptr->motion_vector_xl0) +
                EB_ABS_DIFF(ref0_amvp_cand_array_y[0], candidate_ptr->motion_vector_yl0);

            mvd1 = EB_ABS_DIFF(ref0_amvp_cand_array_x[1], candidate_ptr->motion_vector_xl0) +
                EB_ABS_DIFF(ref0_amvp_cand_array_y[1], candidate_ptr->motion_vector_yl0);

            mvpRef0Idx = ((mvd0) <= (mvd1)) ? 0 : 1;

            candidate_ptr->motion_vector_pred_idx[REF_LIST_0] = mvpRef0Idx;
            candidate_ptr->motion_vector_pred_x[REF_LIST_0] = ref0_amvp_cand_array_x[mvpRef0Idx];
            candidate_ptr->motion_vector_pred_y[REF_LIST_0] = ref0_amvp_cand_array_y[mvpRef0Idx];
            break;
        default:
            break;
        }

        // Choose the MVP in list1
        // Clip the input MV
        clip_mv(
            cu_origin_x,
            cu_origin_y,
            &candidate_ptr->motion_vector_xl1,
            &candidate_ptr->motion_vector_yl1,
            picture_width,
            picture_height,
            tb_size);

        // Choose the AMVP candidate
        switch (ref1NumAvailableAMVPCand) {
        case 0:
        case 1:
            //mvpRef1Idx = 0;
            candidate_ptr->motion_vector_pred_idx[REF_LIST_1] = 0;
            candidate_ptr->motion_vector_pred_x[REF_LIST_1] = ref1_amvp_cand_array_x[0];
            candidate_ptr->motion_vector_pred_y[REF_LIST_1] = ref1_amvp_cand_array_y[0];
            break;
        case 2:

            mvd0 = EB_ABS_DIFF(ref1_amvp_cand_array_x[0], candidate_ptr->motion_vector_xl1) +
                EB_ABS_DIFF(ref1_amvp_cand_array_y[0], candidate_ptr->motion_vector_yl1);

            mvd1 = EB_ABS_DIFF(ref1_amvp_cand_array_x[1], candidate_ptr->motion_vector_xl1) +
                EB_ABS_DIFF(ref1_amvp_cand_array_y[1], candidate_ptr->motion_vector_yl1);

            mvpRef1Idx = ((mvd0) <= (mvd1)) ? 0 : 1;

            candidate_ptr->motion_vector_pred_idx[REF_LIST_1] = mvpRef1Idx;
            candidate_ptr->motion_vector_pred_x[REF_LIST_1] = ref1_amvp_cand_array_x[mvpRef1Idx];
            candidate_ptr->motion_vector_pred_y[REF_LIST_1] = ref1_amvp_cand_array_y[mvpRef1Idx];
            break;
        default:
            break;
        }

        break;

    default:
        break;
    }

    return return_error;
}
