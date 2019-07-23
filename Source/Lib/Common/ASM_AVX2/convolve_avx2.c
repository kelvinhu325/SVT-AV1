/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include "EbDefinitions.h"
#include <immintrin.h>
#include "convolve.h"
#include "aom_dsp_rtcd.h"
#include "convolve_avx2.h"
#include "EbInterPrediction.h"
#include "synonyms.h"

void av1_convolve_y_sr_avx2(const uint8_t *src, int32_t src_stride,
    uint8_t *dst, int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x, InterpFilterParams *filter_params_y,
    const int32_t subpel_x_qn, const int32_t subpel_y_qn,
    ConvolveParams *conv_params) {
    int32_t i, j;
    // right shift is F-1 because we are already dividing
    // filter co-efficients by 2
    const int32_t right_shift_bits = (FILTER_BITS - 1);
    const __m128i right_shift = _mm_cvtsi32_si128(right_shift_bits);
    const __m256i right_shift_const =
        _mm256_set1_epi16((1 << right_shift_bits) >> 1);

    assert(conv_params->round_0 <= FILTER_BITS);
    assert(((conv_params->round_0 + conv_params->round_1) <= (FILTER_BITS + 1)) ||
        ((conv_params->round_0 + conv_params->round_1) == (2 * FILTER_BITS)));

    (void)filter_params_x;
    (void)subpel_x_qn;
    (void)conv_params;
    __m256i coeffs[4], s[8];
    __m128i d[6];

    if (is_convolve_2tap(filter_params_y->filter_ptr)) {
        // vert_filt as 2 tap
        const int32_t fo_vert = 0;
        const uint8_t *const src_ptr = src - fo_vert * src_stride;

        prepare_coeffs_lowbd_2tap(filter_params_y, subpel_y_qn, coeffs);

        for (j = 0; j < w; j += 16) {
            const uint8_t *data = &src_ptr[j];
            d[0] = _mm_loadu_si128((__m128i *)(data + 0 * src_stride));

            for (i = 0; i < h; i += 2) {
                data = &src_ptr[i * src_stride + j];
                d[1] = _mm_loadu_si128((__m128i *)(data + 1 * src_stride));
                const __m256i src_45a = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(d[0]), _mm256_castsi128_si256(d[1]), 0x20);

                d[0] = _mm_loadu_si128((__m128i *)(data + 2 * src_stride));
                const __m256i src_56a = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(d[1]), _mm256_castsi128_si256(d[0]), 0x20);

                s[0] = _mm256_unpacklo_epi8(src_45a, src_56a);
                s[1] = _mm256_unpackhi_epi8(src_45a, src_56a);

                const __m256i res_lo = convolve_lowbd_2tap(s, coeffs);
                /* rounding code */
                // shift by F - 1
                const __m256i res_16b_lo = _mm256_sra_epi16(
                    _mm256_add_epi16(res_lo, right_shift_const), right_shift);
                // 8 bit conversion and saturation to uint8
                __m256i res_8b_lo = _mm256_packus_epi16(res_16b_lo, res_16b_lo);

                if (w - j > 8) {
                    const __m256i res_hi = convolve_lowbd_2tap(s + 1, coeffs);

                    /* rounding code */
                    // shift by F - 1
                    const __m256i res_16b_hi = _mm256_sra_epi16(
                        _mm256_add_epi16(res_hi, right_shift_const), right_shift);
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b_hi = _mm256_packus_epi16(res_16b_hi, res_16b_hi);

                    __m256i res_a = _mm256_unpacklo_epi64(res_8b_lo, res_8b_hi);

                    const __m128i res_0 = _mm256_castsi256_si128(res_a);
                    const __m128i res_1 = _mm256_extracti128_si256(res_a, 1);

                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res_0);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j + dst_stride],
                        res_1);
                }
                else {
                    const __m128i res_0 = _mm256_castsi256_si128(res_8b_lo);
                    const __m128i res_1 = _mm256_extracti128_si256(res_8b_lo, 1);
                    if (w - j > 4) {
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j], res_0);
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j + dst_stride],
                            res_1);
                    }
                    else if (w - j > 2) {
                        xx_storel_32(&dst[i * dst_stride + j], res_0);
                        xx_storel_32(&dst[i * dst_stride + j + dst_stride], res_1);
                    }
                    else {
                        __m128i *const p_0 = (__m128i *)&dst[i * dst_stride + j];
                        __m128i *const p_1 =
                            (__m128i *)&dst[i * dst_stride + j + dst_stride];
                        *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                        *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                    }
                }
            }
        }
    }
    else if (is_convolve_4tap(filter_params_y->filter_ptr)) {
        // vert_filt as 4 tap
        const int32_t fo_vert = 1;
        const uint8_t *const src_ptr = src - fo_vert * src_stride;

        prepare_coeffs_lowbd(filter_params_y, subpel_y_qn, coeffs);

        for (j = 0; j < w; j += 16) {
            const uint8_t *data = &src_ptr[j];
            d[0] = _mm_loadu_si128((__m128i *)(data + 0 * src_stride));
            d[1] = _mm_loadu_si128((__m128i *)(data + 1 * src_stride));
            d[2] = _mm_loadu_si128((__m128i *)(data + 2 * src_stride));
            d[3] = _mm_loadu_si128((__m128i *)(data + 3 * src_stride));
            d[4] = _mm_loadu_si128((__m128i *)(data + 4 * src_stride));

            // Load lines a and b. Line a to lower 128, line b to upper 128
            const __m256i src_01a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[0]), _mm256_castsi128_si256(d[1]), 0x20);

            const __m256i src_12a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[1]), _mm256_castsi128_si256(d[2]), 0x20);

            const __m256i src_23a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[2]), _mm256_castsi128_si256(d[3]), 0x20);

            const __m256i src_34a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[3]), _mm256_castsi128_si256(d[4]), 0x20);

            s[0] = _mm256_unpacklo_epi8(src_01a, src_12a);
            s[1] = _mm256_unpacklo_epi8(src_23a, src_34a);

            s[3] = _mm256_unpackhi_epi8(src_01a, src_12a);
            s[4] = _mm256_unpackhi_epi8(src_23a, src_34a);

            for (i = 0; i < h; i += 2) {
                data = &src_ptr[i * src_stride + j];
                d[5] = _mm_loadu_si128((__m128i *)(data + 5 * src_stride));
                const __m256i src_45a = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(d[4]), _mm256_castsi128_si256(d[5]), 0x20);

                d[4] = _mm_loadu_si128((__m128i *)(data + 6 * src_stride));
                const __m256i src_56a = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(d[5]), _mm256_castsi128_si256(d[4]), 0x20);

                s[2] = _mm256_unpacklo_epi8(src_45a, src_56a);
                s[5] = _mm256_unpackhi_epi8(src_45a, src_56a);

                const __m256i res_lo = convolve_lowbd_4tap(s, coeffs + 1);
                /* rounding code */
                // shift by F - 1
                const __m256i res_16b_lo = _mm256_sra_epi16(
                    _mm256_add_epi16(res_lo, right_shift_const), right_shift);
                // 8 bit conversion and saturation to uint8
                __m256i res_8b_lo = _mm256_packus_epi16(res_16b_lo, res_16b_lo);

                if (w - j > 8) {
                    const __m256i res_hi = convolve_lowbd_4tap(s + 3, coeffs + 1);

                    /* rounding code */
                    // shift by F - 1
                    const __m256i res_16b_hi = _mm256_sra_epi16(
                        _mm256_add_epi16(res_hi, right_shift_const), right_shift);
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b_hi = _mm256_packus_epi16(res_16b_hi, res_16b_hi);

                    __m256i res_a = _mm256_unpacklo_epi64(res_8b_lo, res_8b_hi);

                    const __m128i res_0 = _mm256_castsi256_si128(res_a);
                    const __m128i res_1 = _mm256_extracti128_si256(res_a, 1);

                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res_0);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j + dst_stride],
                        res_1);
                }
                else {
                    const __m128i res_0 = _mm256_castsi256_si128(res_8b_lo);
                    const __m128i res_1 = _mm256_extracti128_si256(res_8b_lo, 1);
                    if (w - j > 4) {
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j], res_0);
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j + dst_stride],
                            res_1);
                    }
                    else if (w - j > 2) {
                        xx_storel_32(&dst[i * dst_stride + j], res_0);
                        xx_storel_32(&dst[i * dst_stride + j + dst_stride], res_1);
                    }
                    else {
                        __m128i *const p_0 = (__m128i *)&dst[i * dst_stride + j];
                        __m128i *const p_1 =
                            (__m128i *)&dst[i * dst_stride + j + dst_stride];
                        *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                        *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                    }
                }
                s[0] = s[1];
                s[1] = s[2];

                s[3] = s[4];
                s[4] = s[5];
            }
        }
    }
    else {
        const int32_t fo_vert = filter_params_y->taps / 2 - 1;
        const uint8_t *const src_ptr = src - fo_vert * src_stride;

        prepare_coeffs_lowbd(filter_params_y, subpel_y_qn, coeffs);

        for (j = 0; j < w; j += 16) {
            const uint8_t *data = &src_ptr[j];
            __m256i src6;

            d[0] = _mm_loadu_si128((__m128i *)(data + 0 * src_stride));
            d[1] = _mm_loadu_si128((__m128i *)(data + 1 * src_stride));
            d[2] = _mm_loadu_si128((__m128i *)(data + 2 * src_stride));
            d[3] = _mm_loadu_si128((__m128i *)(data + 3 * src_stride));
            d[4] = _mm_loadu_si128((__m128i *)(data + 4 * src_stride));
            d[5] = _mm_loadu_si128((__m128i *)(data + 5 * src_stride));
            // Load lines a and b. Line a to lower 128, line b to upper 128
            const __m256i src_01a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[0]), _mm256_castsi128_si256(d[1]), 0x20);

            const __m256i src_12a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[1]), _mm256_castsi128_si256(d[2]), 0x20);

            const __m256i src_23a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[2]), _mm256_castsi128_si256(d[3]), 0x20);

            const __m256i src_34a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[3]), _mm256_castsi128_si256(d[4]), 0x20);

            const __m256i src_45a = _mm256_permute2x128_si256(
                _mm256_castsi128_si256(d[4]), _mm256_castsi128_si256(d[5]), 0x20);

            src6 = _mm256_castsi128_si256(
                _mm_loadu_si128((__m128i *)(data + 6 * src_stride)));
            const __m256i src_56a =
                _mm256_permute2x128_si256(_mm256_castsi128_si256(d[5]), src6, 0x20);

            s[0] = _mm256_unpacklo_epi8(src_01a, src_12a);
            s[1] = _mm256_unpacklo_epi8(src_23a, src_34a);
            s[2] = _mm256_unpacklo_epi8(src_45a, src_56a);

            s[4] = _mm256_unpackhi_epi8(src_01a, src_12a);
            s[5] = _mm256_unpackhi_epi8(src_23a, src_34a);
            s[6] = _mm256_unpackhi_epi8(src_45a, src_56a);

            for (i = 0; i < h; i += 2) {
                data = &src_ptr[i * src_stride + j];
                const __m256i src_67a = _mm256_permute2x128_si256(
                    src6,
                    _mm256_castsi128_si256(
                        _mm_loadu_si128((__m128i *)(data + 7 * src_stride))),
                    0x20);

                src6 = _mm256_castsi128_si256(
                    _mm_loadu_si128((__m128i *)(data + 8 * src_stride)));
                const __m256i src_78a = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(
                        _mm_loadu_si128((__m128i *)(data + 7 * src_stride))),
                    src6, 0x20);

                s[3] = _mm256_unpacklo_epi8(src_67a, src_78a);
                s[7] = _mm256_unpackhi_epi8(src_67a, src_78a);

                const __m256i res_lo = convolve_lowbd(s, coeffs);

                /* rounding code */
                // shift by F - 1
                const __m256i res_16b_lo = _mm256_sra_epi16(
                    _mm256_add_epi16(res_lo, right_shift_const), right_shift);
                // 8 bit conversion and saturation to uint8
                __m256i res_8b_lo = _mm256_packus_epi16(res_16b_lo, res_16b_lo);

                if (w - j > 8) {
                    const __m256i res_hi = convolve_lowbd(s + 4, coeffs);

                    /* rounding code */
                    // shift by F - 1
                    const __m256i res_16b_hi = _mm256_sra_epi16(
                        _mm256_add_epi16(res_hi, right_shift_const), right_shift);
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b_hi = _mm256_packus_epi16(res_16b_hi, res_16b_hi);

                    __m256i res_a = _mm256_unpacklo_epi64(res_8b_lo, res_8b_hi);

                    const __m128i res_0 = _mm256_castsi256_si128(res_a);
                    const __m128i res_1 = _mm256_extracti128_si256(res_a, 1);

                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res_0);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j + dst_stride],
                        res_1);
                }
                else {
                    const __m128i res_0 = _mm256_castsi256_si128(res_8b_lo);
                    const __m128i res_1 = _mm256_extracti128_si256(res_8b_lo, 1);
                    if (w - j > 4) {
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j], res_0);
                        _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j + dst_stride],
                            res_1);
                    }
                    else if (w - j > 2) {
                        xx_storel_32(&dst[i * dst_stride + j], res_0);
                        xx_storel_32(&dst[i * dst_stride + j + dst_stride], res_1);
                    }
                    else {
                        __m128i *const p_0 = (__m128i *)&dst[i * dst_stride + j];
                        __m128i *const p_1 =
                            (__m128i *)&dst[i * dst_stride + j + dst_stride];
                        *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                        *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                    }
                }
                s[0] = s[1];
                s[1] = s[2];
                s[2] = s[3];

                s[4] = s[5];
                s[5] = s[6];
                s[6] = s[7];
            }
        }
    }
}

void av1_convolve_x_sr_avx2(const uint8_t *src, int32_t src_stride,
    uint8_t *dst, int32_t dst_stride, int32_t w, int32_t h,
    InterpFilterParams *filter_params_x, InterpFilterParams *filter_params_y,
    const int32_t subpel_x_qn, const int32_t subpel_y_qn,
    ConvolveParams *conv_params) {
    const int32_t bits = FILTER_BITS - conv_params->round_0;

    const __m256i round_0_const =
        _mm256_set1_epi16((1 << (conv_params->round_0 - 1)) >> 1);
    const __m128i round_0_shift = _mm_cvtsi32_si128(conv_params->round_0 - 1);
    const __m256i round_const = _mm256_set1_epi16((1 << bits) >> 1);
    const __m128i round_shift = _mm_cvtsi32_si128(bits);
    int32_t i;
    (void)filter_params_y;
    (void)subpel_y_qn;

    assert(bits >= 0);
    assert((FILTER_BITS - conv_params->round_1) >= 0 ||
        ((conv_params->round_0 + conv_params->round_1) == 2 * FILTER_BITS));
    assert(conv_params->round_0 > 0);

    __m256i coeffs[4], filt[4];
    filt[0] = _mm256_load_si256((__m256i const *)filt1_global_avx2);
    filt[1] = _mm256_load_si256((__m256i const *)filt2_global_avx2);

    if (is_convolve_2tap(filter_params_x->filter_ptr)) {
        // horz_filt as 2 tap
        const int32_t fo_horiz = 0;
        const uint8_t *const src_ptr = src - fo_horiz;

        prepare_coeffs_lowbd_2tap(filter_params_x, subpel_x_qn, coeffs);

        if (w <= 8) {
            for (i = 0; i < h; i += 2) {
                const __m256i data = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(
                        _mm_loadu_si128((__m128i *)(&src_ptr[i * src_stride]))),
                    _mm256_castsi128_si256(_mm_loadu_si128(
                    (__m128i *)(&src_ptr[i * src_stride + src_stride]))),
                    0x20);

                __m256i res_16b = convolve_lowbd_x_2tap(data, coeffs, filt);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                    round_0_shift);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                    round_shift);

                /* rounding code */
                // 8 bit conversion and saturation to uint8
                __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                const __m128i res_0 = _mm256_castsi256_si128(res_8b);
                const __m128i res_1 = _mm256_extracti128_si256(res_8b, 1);

                if (w > 4) {
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride], res_0);
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride + dst_stride], res_1);
                }
                else if (w > 2) {
                    xx_storel_32(&dst[i * dst_stride], res_0);
                    xx_storel_32(&dst[i * dst_stride + dst_stride], res_1);
                }
                else {
                    __m128i *const p_0 = (__m128i *)&dst[i * dst_stride];
                    __m128i *const p_1 = (__m128i *)&dst[i * dst_stride + dst_stride];
                    *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                    *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                }
            }
        }
        else {
            for (i = 0; i < h; ++i) {
                for (int32_t j = 0; j < w; j += 16) {
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 8 9 10 11 12 13 14 15 16 17
                    // 18 19 20 21 22 23
                    const __m256i data = _mm256_inserti128_si256(
                        _mm256_loadu_si256((__m256i *)&src_ptr[(i * src_stride) + j]),
                        _mm_loadu_si128((__m128i *)&src_ptr[(i * src_stride) + (j + 8)]),
                        1);

                    __m256i res_16b = convolve_lowbd_x_2tap(data, coeffs, filt);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                        round_0_shift);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                        round_shift);

                    /* rounding code */
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                    // Store values into the destination buffer
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
                    res_8b = _mm256_permute4x64_epi64(res_8b, 216);
                    __m128i res = _mm256_castsi256_si128(res_8b);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res);
                }
            }
        }
    }
    else if (is_convolve_4tap(filter_params_x->filter_ptr)) {
        // horz_filt as 4 tap
        const int32_t fo_horiz = 1;
        const uint8_t *const src_ptr = src - fo_horiz;

        prepare_coeffs_lowbd(filter_params_x, subpel_x_qn, coeffs);

        if (w <= 8) {
            for (i = 0; i < h; i += 2) {
                const __m256i data = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(
                        _mm_loadu_si128((__m128i *)(&src_ptr[i * src_stride]))),
                    _mm256_castsi128_si256(_mm_loadu_si128(
                    (__m128i *)(&src_ptr[i * src_stride + src_stride]))),
                    0x20);

                __m256i res_16b = convolve_lowbd_x_4tap(data, coeffs + 1, filt);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                    round_0_shift);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                    round_shift);

                /* rounding code */
                // 8 bit conversion and saturation to uint8
                __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                const __m128i res_0 = _mm256_castsi256_si128(res_8b);
                const __m128i res_1 = _mm256_extracti128_si256(res_8b, 1);

                if (w > 4) {
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride], res_0);
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride + dst_stride], res_1);
                }
                else if (w > 2) {
                    xx_storel_32(&dst[i * dst_stride], res_0);
                    xx_storel_32(&dst[i * dst_stride + dst_stride], res_1);
                }
                else {
                    __m128i *const p_0 = (__m128i *)&dst[i * dst_stride];
                    __m128i *const p_1 = (__m128i *)&dst[i * dst_stride + dst_stride];
                    *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                    *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                }
            }
        }
        else {
            for (i = 0; i < h; ++i) {
                for (int32_t j = 0; j < w; j += 16) {
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 8 9 10 11 12 13 14 15 16 17
                    // 18 19 20 21 22 23
                    const __m256i data = _mm256_inserti128_si256(
                        _mm256_loadu_si256((__m256i *)&src_ptr[(i * src_stride) + j]),
                        _mm_loadu_si128((__m128i *)&src_ptr[(i * src_stride) + (j + 8)]),
                        1);

                    __m256i res_16b = convolve_lowbd_x_4tap(data, coeffs + 1, filt);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                        round_0_shift);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                        round_shift);

                    /* rounding code */
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                    // Store values into the destination buffer
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
                    res_8b = _mm256_permute4x64_epi64(res_8b, 216);
                    __m128i res = _mm256_castsi256_si128(res_8b);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res);
                }
            }
        }
    }
    else {
        const int32_t fo_horiz = filter_params_x->taps / 2 - 1;
        const uint8_t *const src_ptr = src - fo_horiz;

        prepare_coeffs_lowbd(filter_params_x, subpel_x_qn, coeffs);
        filt[2] = _mm256_load_si256((__m256i const *)filt3_global_avx2);
        filt[3] = _mm256_load_si256((__m256i const *)filt4_global_avx2);

        if (w <= 8) {
            for (i = 0; i < h; i += 2) {
                const __m256i data = _mm256_permute2x128_si256(
                    _mm256_castsi128_si256(
                        _mm_loadu_si128((__m128i *)(&src_ptr[i * src_stride]))),
                    _mm256_castsi128_si256(_mm_loadu_si128(
                    (__m128i *)(&src_ptr[i * src_stride + src_stride]))),
                    0x20);

                __m256i res_16b = convolve_lowbd_x(data, coeffs, filt);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                    round_0_shift);

                res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                    round_shift);

                /* rounding code */
                // 8 bit conversion and saturation to uint8
                __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                const __m128i res_0 = _mm256_castsi256_si128(res_8b);
                const __m128i res_1 = _mm256_extracti128_si256(res_8b, 1);
                if (w > 4) {
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride], res_0);
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride + dst_stride], res_1);
                }
                else if (w > 2) {
                    xx_storel_32(&dst[i * dst_stride], res_0);
                    xx_storel_32(&dst[i * dst_stride + dst_stride], res_1);
                }
                else {
                    __m128i *const p_0 = (__m128i *)&dst[i * dst_stride];
                    __m128i *const p_1 = (__m128i *)&dst[i * dst_stride + dst_stride];
                    *(uint16_t *)p_0 = _mm_cvtsi128_si32(res_0);
                    *(uint16_t *)p_1 = _mm_cvtsi128_si32(res_1);
                }
            }
        }
        else {
            for (i = 0; i < h; ++i) {
                for (int32_t j = 0; j < w; j += 16) {
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 8 9 10 11 12 13 14 15 16 17
                    // 18 19 20 21 22 23
                    const __m256i data = _mm256_inserti128_si256(
                        _mm256_loadu_si256((__m256i *)&src_ptr[(i * src_stride) + j]),
                        _mm_loadu_si128((__m128i *)&src_ptr[(i * src_stride) + (j + 8)]),
                        1);

                    __m256i res_16b = convolve_lowbd_x(data, coeffs, filt);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_0_const),
                        round_0_shift);

                    res_16b = _mm256_sra_epi16(_mm256_add_epi16(res_16b, round_const),
                        round_shift);

                    /* rounding code */
                    // 8 bit conversion and saturation to uint8
                    __m256i res_8b = _mm256_packus_epi16(res_16b, res_16b);

                    // Store values into the destination buffer
                    // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15
                    res_8b = _mm256_permute4x64_epi64(res_8b, 216);
                    __m128i res = _mm256_castsi256_si128(res_8b);
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], res);
                }
            }
        }
    }
}

#if COMP_AVX // TO be added as reconinter_avx2.c
// Loads and stores to do away with the tedium of casting the address
// to the right type.
static INLINE __m128i xx_loadl_32(const void *a) {
  int val;
  memcpy(&val, a, sizeof(val));
  return _mm_cvtsi32_si128(val);
}
static INLINE __m128i xx_load_128(const void *a) {
  return _mm_load_si128((const __m128i *)a);
}

static INLINE __m256i yy_loadu_256(const void *a) {
  return _mm256_loadu_si256((const __m256i *)a);
}


static INLINE void yy_storeu_256(void *const a, const __m256i v) {
  _mm256_storeu_si256((__m256i *)a, v);
}



static INLINE __m256i calc_mask_avx2(const __m256i mask_base, const __m256i s0,
                                     const __m256i s1) {
  const __m256i diff = _mm256_abs_epi16(_mm256_sub_epi16(s0, s1));
  return _mm256_abs_epi16(
      _mm256_add_epi16(mask_base, _mm256_srli_epi16(diff, 4)));
  // clamp(diff, 0, 64) can be skiped for diff is always in the range ( 38, 54)
}
void av1_build_compound_diffwtd_mask_avx2(uint8_t *mask,
                                          DIFFWTD_MASK_TYPE mask_type,
                                          const uint8_t *src0, int src0_stride,
                                          const uint8_t *src1, int src1_stride,
                                          int h, int w) {
  const int mb = (mask_type == DIFFWTD_38_INV) ? AOM_BLEND_A64_MAX_ALPHA : 0;
  const __m256i y_mask_base = _mm256_set1_epi16(38 - mb);
  int i = 0;
  if (4 == w) {
    do {
      const __m128i s0A = xx_loadl_32(src0);
      const __m128i s0B = xx_loadl_32(src0 + src0_stride);
      const __m128i s0C = xx_loadl_32(src0 + src0_stride * 2);
      const __m128i s0D = xx_loadl_32(src0 + src0_stride * 3);
      const __m128i s0AB = _mm_unpacklo_epi32(s0A, s0B);
      const __m128i s0CD = _mm_unpacklo_epi32(s0C, s0D);
      const __m128i s0ABCD = _mm_unpacklo_epi64(s0AB, s0CD);
      const __m256i s0ABCD_w = _mm256_cvtepu8_epi16(s0ABCD);

      const __m128i s1A = xx_loadl_32(src1);
      const __m128i s1B = xx_loadl_32(src1 + src1_stride);
      const __m128i s1C = xx_loadl_32(src1 + src1_stride * 2);
      const __m128i s1D = xx_loadl_32(src1 + src1_stride * 3);
      const __m128i s1AB = _mm_unpacklo_epi32(s1A, s1B);
      const __m128i s1CD = _mm_unpacklo_epi32(s1C, s1D);
      const __m128i s1ABCD = _mm_unpacklo_epi64(s1AB, s1CD);
      const __m256i s1ABCD_w = _mm256_cvtepu8_epi16(s1ABCD);
      const __m256i m16 = calc_mask_avx2(y_mask_base, s0ABCD_w, s1ABCD_w);
      const __m256i m8 = _mm256_packus_epi16(m16, _mm256_setzero_si256());
      const __m128i x_m8 =
          _mm256_castsi256_si128(_mm256_permute4x64_epi64(m8, 0xd8));
      xx_storeu_128(mask, x_m8);
      src0 += (src0_stride << 2);
      src1 += (src1_stride << 2);
      mask += 16;
      i += 4;
    } while (i < h);
  } else if (8 == w) {
    do {
      const __m128i s0A = xx_loadl_64(src0);
      const __m128i s0B = xx_loadl_64(src0 + src0_stride);
      const __m128i s0C = xx_loadl_64(src0 + src0_stride * 2);
      const __m128i s0D = xx_loadl_64(src0 + src0_stride * 3);
      const __m256i s0AC_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(s0A, s0C));
      const __m256i s0BD_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(s0B, s0D));
      const __m128i s1A = xx_loadl_64(src1);
      const __m128i s1B = xx_loadl_64(src1 + src1_stride);
      const __m128i s1C = xx_loadl_64(src1 + src1_stride * 2);
      const __m128i s1D = xx_loadl_64(src1 + src1_stride * 3);
      const __m256i s1AB_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(s1A, s1C));
      const __m256i s1CD_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(s1B, s1D));
      const __m256i m16AC = calc_mask_avx2(y_mask_base, s0AC_w, s1AB_w);
      const __m256i m16BD = calc_mask_avx2(y_mask_base, s0BD_w, s1CD_w);
      const __m256i m8 = _mm256_packus_epi16(m16AC, m16BD);
      yy_storeu_256(mask, m8);
      src0 += src0_stride << 2;
      src1 += src1_stride << 2;
      mask += 32;
      i += 4;
    } while (i < h);
  } else if (16 == w) {
    do {
      const __m128i s0A = xx_load_128(src0);
      const __m128i s0B = xx_load_128(src0 + src0_stride);
      const __m128i s1A = xx_load_128(src1);
      const __m128i s1B = xx_load_128(src1 + src1_stride);
      const __m256i s0AL = _mm256_cvtepu8_epi16(s0A);
      const __m256i s0BL = _mm256_cvtepu8_epi16(s0B);
      const __m256i s1AL = _mm256_cvtepu8_epi16(s1A);
      const __m256i s1BL = _mm256_cvtepu8_epi16(s1B);

      const __m256i m16AL = calc_mask_avx2(y_mask_base, s0AL, s1AL);
      const __m256i m16BL = calc_mask_avx2(y_mask_base, s0BL, s1BL);

      const __m256i m8 =
          _mm256_permute4x64_epi64(_mm256_packus_epi16(m16AL, m16BL), 0xd8);
      yy_storeu_256(mask, m8);
      src0 += src0_stride << 1;
      src1 += src1_stride << 1;
      mask += 32;
      i += 2;
    } while (i < h);
  } else {
    do {
      int j = 0;
      do {
        const __m256i s0 = yy_loadu_256(src0 + j);
        const __m256i s1 = yy_loadu_256(src1 + j);
        const __m256i s0L = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(s0));
        const __m256i s1L = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(s1));
        const __m256i s0H =
            _mm256_cvtepu8_epi16(_mm256_extracti128_si256(s0, 1));
        const __m256i s1H =
            _mm256_cvtepu8_epi16(_mm256_extracti128_si256(s1, 1));
        const __m256i m16L = calc_mask_avx2(y_mask_base, s0L, s1L);
        const __m256i m16H = calc_mask_avx2(y_mask_base, s0H, s1H);
        const __m256i m8 =
            _mm256_permute4x64_epi64(_mm256_packus_epi16(m16L, m16H), 0xd8);
        yy_storeu_256(mask + j, m8);
        j += 32;
      } while (j < w);
      src0 += src0_stride;
      src1 += src1_stride;
      mask += w;
      i += 1;
    } while (i < h);
  }
}
////////


// Some compilers don't have _mm256_set_m128i defined in immintrin.h. We
// therefore define an equivalent function using a different intrinsic.
// ([ hi ], [ lo ]) -> [ hi ][ lo ]
static INLINE __m256i yy_set_m128i(__m128i hi, __m128i lo) {
  return _mm256_insertf128_si256(_mm256_castsi128_si256(lo), hi, 1);
}
static INLINE __m256i yy_loadu2_128(const void *hi, const void *lo) {
  __m128i mhi = _mm_loadu_si128((__m128i *)(hi));
  __m128i mlo = _mm_loadu_si128((__m128i *)(lo));
  return yy_set_m128i(mhi, mlo);
}


static INLINE __m256i calc_mask_d16_avx2(const __m256i *data_src0,
                                         const __m256i *data_src1,
                                         const __m256i *round_const,
                                         const __m256i *mask_base_16,
                                         const __m256i *clip_diff, int round) {
  const __m256i diffa = _mm256_subs_epu16(*data_src0, *data_src1);
  const __m256i diffb = _mm256_subs_epu16(*data_src1, *data_src0);
  const __m256i diff = _mm256_max_epu16(diffa, diffb);
  const __m256i diff_round =
      _mm256_srli_epi16(_mm256_adds_epu16(diff, *round_const), round);
  const __m256i diff_factor = _mm256_srli_epi16(diff_round, DIFF_FACTOR_LOG2);
  const __m256i diff_mask = _mm256_adds_epi16(diff_factor, *mask_base_16);
  const __m256i diff_clamp = _mm256_min_epi16(diff_mask, *clip_diff);
  return diff_clamp;
}

static INLINE __m256i calc_mask_d16_inv_avx2(const __m256i *data_src0,
                                             const __m256i *data_src1,
                                             const __m256i *round_const,
                                             const __m256i *mask_base_16,
                                             const __m256i *clip_diff,
                                             int round) {
  const __m256i diffa = _mm256_subs_epu16(*data_src0, *data_src1);
  const __m256i diffb = _mm256_subs_epu16(*data_src1, *data_src0);
  const __m256i diff = _mm256_max_epu16(diffa, diffb);
  const __m256i diff_round =
      _mm256_srli_epi16(_mm256_adds_epu16(diff, *round_const), round);
  const __m256i diff_factor = _mm256_srli_epi16(diff_round, DIFF_FACTOR_LOG2);
  const __m256i diff_mask = _mm256_adds_epi16(diff_factor, *mask_base_16);
  const __m256i diff_clamp = _mm256_min_epi16(diff_mask, *clip_diff);
  const __m256i diff_const_16 = _mm256_sub_epi16(*clip_diff, diff_clamp);
  return diff_const_16;
}

static INLINE void build_compound_diffwtd_mask_d16_avx2(
    uint8_t *mask, const CONV_BUF_TYPE *src0, int src0_stride,
    const CONV_BUF_TYPE *src1, int src1_stride, int h, int w, int shift) {
  const int mask_base = 38;
  const __m256i _r = _mm256_set1_epi16((1 << shift) >> 1);
  const __m256i y38 = _mm256_set1_epi16(mask_base);
  const __m256i y64 = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  int i = 0;
  if (w == 4) {
    do {
      const __m128i s0A = xx_loadl_64(src0);
      const __m128i s0B = xx_loadl_64(src0 + src0_stride);
      const __m128i s0C = xx_loadl_64(src0 + src0_stride * 2);
      const __m128i s0D = xx_loadl_64(src0 + src0_stride * 3);
      const __m128i s1A = xx_loadl_64(src1);
      const __m128i s1B = xx_loadl_64(src1 + src1_stride);
      const __m128i s1C = xx_loadl_64(src1 + src1_stride * 2);
      const __m128i s1D = xx_loadl_64(src1 + src1_stride * 3);
      const __m256i s0 = yy_set_m128i(_mm_unpacklo_epi64(s0C, s0D),
                                      _mm_unpacklo_epi64(s0A, s0B));
      const __m256i s1 = yy_set_m128i(_mm_unpacklo_epi64(s1C, s1D),
                                      _mm_unpacklo_epi64(s1A, s1B));
      const __m256i m16 = calc_mask_d16_avx2(&s0, &s1, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16, _mm256_setzero_si256());
      xx_storeu_128(mask,
                    _mm256_castsi256_si128(_mm256_permute4x64_epi64(m8, 0xd8)));
      src0 += src0_stride << 2;
      src1 += src1_stride << 2;
      mask += 16;
      i += 4;
    } while (i < h);
  } else if (w == 8) {
    do {
      const __m256i s0AB = yy_loadu2_128(src0 + src0_stride, src0);
      const __m256i s0CD =
          yy_loadu2_128(src0 + src0_stride * 3, src0 + src0_stride * 2);
      const __m256i s1AB = yy_loadu2_128(src1 + src1_stride, src1);
      const __m256i s1CD =
          yy_loadu2_128(src1 + src1_stride * 3, src1 + src1_stride * 2);
      const __m256i m16AB =
          calc_mask_d16_avx2(&s0AB, &s1AB, &_r, &y38, &y64, shift);
      const __m256i m16CD =
          calc_mask_d16_avx2(&s0CD, &s1CD, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16AB, m16CD);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride << 2;
      src1 += src1_stride << 2;
      mask += 32;
      i += 4;
    } while (i < h);
  } else if (w == 16) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + src0_stride);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + src1_stride);
      const __m256i m16A =
          calc_mask_d16_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16A, m16B);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride << 1;
      src1 += src1_stride << 1;
      mask += 32;
      i += 2;
    } while (i < h);
  } else if (w == 32) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i m16A =
          calc_mask_d16_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16A, m16B);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 32;
      i += 1;
    } while (i < h);
  } else if (w == 64) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s0C = yy_loadu_256(src0 + 32);
      const __m256i s0D = yy_loadu_256(src0 + 48);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i s1C = yy_loadu_256(src1 + 32);
      const __m256i s1D = yy_loadu_256(src1 + 48);
      const __m256i m16A =
          calc_mask_d16_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m16C =
          calc_mask_d16_avx2(&s0C, &s1C, &_r, &y38, &y64, shift);
      const __m256i m16D =
          calc_mask_d16_avx2(&s0D, &s1D, &_r, &y38, &y64, shift);
      const __m256i m8AB = _mm256_packus_epi16(m16A, m16B);
      const __m256i m8CD = _mm256_packus_epi16(m16C, m16D);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8AB, 0xd8));
      yy_storeu_256(mask + 32, _mm256_permute4x64_epi64(m8CD, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 64;
      i += 1;
    } while (i < h);
  } else {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s0C = yy_loadu_256(src0 + 32);
      const __m256i s0D = yy_loadu_256(src0 + 48);
      const __m256i s0E = yy_loadu_256(src0 + 64);
      const __m256i s0F = yy_loadu_256(src0 + 80);
      const __m256i s0G = yy_loadu_256(src0 + 96);
      const __m256i s0H = yy_loadu_256(src0 + 112);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i s1C = yy_loadu_256(src1 + 32);
      const __m256i s1D = yy_loadu_256(src1 + 48);
      const __m256i s1E = yy_loadu_256(src1 + 64);
      const __m256i s1F = yy_loadu_256(src1 + 80);
      const __m256i s1G = yy_loadu_256(src1 + 96);
      const __m256i s1H = yy_loadu_256(src1 + 112);
      const __m256i m16A =
          calc_mask_d16_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m16C =
          calc_mask_d16_avx2(&s0C, &s1C, &_r, &y38, &y64, shift);
      const __m256i m16D =
          calc_mask_d16_avx2(&s0D, &s1D, &_r, &y38, &y64, shift);
      const __m256i m16E =
          calc_mask_d16_avx2(&s0E, &s1E, &_r, &y38, &y64, shift);
      const __m256i m16F =
          calc_mask_d16_avx2(&s0F, &s1F, &_r, &y38, &y64, shift);
      const __m256i m16G =
          calc_mask_d16_avx2(&s0G, &s1G, &_r, &y38, &y64, shift);
      const __m256i m16H =
          calc_mask_d16_avx2(&s0H, &s1H, &_r, &y38, &y64, shift);
      const __m256i m8AB = _mm256_packus_epi16(m16A, m16B);
      const __m256i m8CD = _mm256_packus_epi16(m16C, m16D);
      const __m256i m8EF = _mm256_packus_epi16(m16E, m16F);
      const __m256i m8GH = _mm256_packus_epi16(m16G, m16H);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8AB, 0xd8));
      yy_storeu_256(mask + 32, _mm256_permute4x64_epi64(m8CD, 0xd8));
      yy_storeu_256(mask + 64, _mm256_permute4x64_epi64(m8EF, 0xd8));
      yy_storeu_256(mask + 96, _mm256_permute4x64_epi64(m8GH, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 128;
      i += 1;
    } while (i < h);
  }
}
static INLINE void build_compound_diffwtd_mask_d16_inv_avx2(
    uint8_t *mask, const CONV_BUF_TYPE *src0, int src0_stride,
    const CONV_BUF_TYPE *src1, int src1_stride, int h, int w, int shift) {
  const int mask_base = 38;
  const __m256i _r = _mm256_set1_epi16((1 << shift) >> 1);
  const __m256i y38 = _mm256_set1_epi16(mask_base);
  const __m256i y64 = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  int i = 0;
  if (w == 4) {
    do {
      const __m128i s0A = xx_loadl_64(src0);
      const __m128i s0B = xx_loadl_64(src0 + src0_stride);
      const __m128i s0C = xx_loadl_64(src0 + src0_stride * 2);
      const __m128i s0D = xx_loadl_64(src0 + src0_stride * 3);
      const __m128i s1A = xx_loadl_64(src1);
      const __m128i s1B = xx_loadl_64(src1 + src1_stride);
      const __m128i s1C = xx_loadl_64(src1 + src1_stride * 2);
      const __m128i s1D = xx_loadl_64(src1 + src1_stride * 3);
      const __m256i s0 = yy_set_m128i(_mm_unpacklo_epi64(s0C, s0D),
                                      _mm_unpacklo_epi64(s0A, s0B));
      const __m256i s1 = yy_set_m128i(_mm_unpacklo_epi64(s1C, s1D),
                                      _mm_unpacklo_epi64(s1A, s1B));
      const __m256i m16 =
          calc_mask_d16_inv_avx2(&s0, &s1, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16, _mm256_setzero_si256());
      xx_storeu_128(mask,
                    _mm256_castsi256_si128(_mm256_permute4x64_epi64(m8, 0xd8)));
      src0 += src0_stride << 2;
      src1 += src1_stride << 2;
      mask += 16;
      i += 4;
    } while (i < h);
  } else if (w == 8) {
    do {
      const __m256i s0AB = yy_loadu2_128(src0 + src0_stride, src0);
      const __m256i s0CD =
          yy_loadu2_128(src0 + src0_stride * 3, src0 + src0_stride * 2);
      const __m256i s1AB = yy_loadu2_128(src1 + src1_stride, src1);
      const __m256i s1CD =
          yy_loadu2_128(src1 + src1_stride * 3, src1 + src1_stride * 2);
      const __m256i m16AB =
          calc_mask_d16_inv_avx2(&s0AB, &s1AB, &_r, &y38, &y64, shift);
      const __m256i m16CD =
          calc_mask_d16_inv_avx2(&s0CD, &s1CD, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16AB, m16CD);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride << 2;
      src1 += src1_stride << 2;
      mask += 32;
      i += 4;
    } while (i < h);
  } else if (w == 16) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + src0_stride);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + src1_stride);
      const __m256i m16A =
          calc_mask_d16_inv_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_inv_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16A, m16B);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride << 1;
      src1 += src1_stride << 1;
      mask += 32;
      i += 2;
    } while (i < h);
  } else if (w == 32) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i m16A =
          calc_mask_d16_inv_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_inv_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m8 = _mm256_packus_epi16(m16A, m16B);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 32;
      i += 1;
    } while (i < h);
  } else if (w == 64) {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s0C = yy_loadu_256(src0 + 32);
      const __m256i s0D = yy_loadu_256(src0 + 48);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i s1C = yy_loadu_256(src1 + 32);
      const __m256i s1D = yy_loadu_256(src1 + 48);
      const __m256i m16A =
          calc_mask_d16_inv_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_inv_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m16C =
          calc_mask_d16_inv_avx2(&s0C, &s1C, &_r, &y38, &y64, shift);
      const __m256i m16D =
          calc_mask_d16_inv_avx2(&s0D, &s1D, &_r, &y38, &y64, shift);
      const __m256i m8AB = _mm256_packus_epi16(m16A, m16B);
      const __m256i m8CD = _mm256_packus_epi16(m16C, m16D);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8AB, 0xd8));
      yy_storeu_256(mask + 32, _mm256_permute4x64_epi64(m8CD, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 64;
      i += 1;
    } while (i < h);
  } else {
    do {
      const __m256i s0A = yy_loadu_256(src0);
      const __m256i s0B = yy_loadu_256(src0 + 16);
      const __m256i s0C = yy_loadu_256(src0 + 32);
      const __m256i s0D = yy_loadu_256(src0 + 48);
      const __m256i s0E = yy_loadu_256(src0 + 64);
      const __m256i s0F = yy_loadu_256(src0 + 80);
      const __m256i s0G = yy_loadu_256(src0 + 96);
      const __m256i s0H = yy_loadu_256(src0 + 112);
      const __m256i s1A = yy_loadu_256(src1);
      const __m256i s1B = yy_loadu_256(src1 + 16);
      const __m256i s1C = yy_loadu_256(src1 + 32);
      const __m256i s1D = yy_loadu_256(src1 + 48);
      const __m256i s1E = yy_loadu_256(src1 + 64);
      const __m256i s1F = yy_loadu_256(src1 + 80);
      const __m256i s1G = yy_loadu_256(src1 + 96);
      const __m256i s1H = yy_loadu_256(src1 + 112);
      const __m256i m16A =
          calc_mask_d16_inv_avx2(&s0A, &s1A, &_r, &y38, &y64, shift);
      const __m256i m16B =
          calc_mask_d16_inv_avx2(&s0B, &s1B, &_r, &y38, &y64, shift);
      const __m256i m16C =
          calc_mask_d16_inv_avx2(&s0C, &s1C, &_r, &y38, &y64, shift);
      const __m256i m16D =
          calc_mask_d16_inv_avx2(&s0D, &s1D, &_r, &y38, &y64, shift);
      const __m256i m16E =
          calc_mask_d16_inv_avx2(&s0E, &s1E, &_r, &y38, &y64, shift);
      const __m256i m16F =
          calc_mask_d16_inv_avx2(&s0F, &s1F, &_r, &y38, &y64, shift);
      const __m256i m16G =
          calc_mask_d16_inv_avx2(&s0G, &s1G, &_r, &y38, &y64, shift);
      const __m256i m16H =
          calc_mask_d16_inv_avx2(&s0H, &s1H, &_r, &y38, &y64, shift);
      const __m256i m8AB = _mm256_packus_epi16(m16A, m16B);
      const __m256i m8CD = _mm256_packus_epi16(m16C, m16D);
      const __m256i m8EF = _mm256_packus_epi16(m16E, m16F);
      const __m256i m8GH = _mm256_packus_epi16(m16G, m16H);
      yy_storeu_256(mask, _mm256_permute4x64_epi64(m8AB, 0xd8));
      yy_storeu_256(mask + 32, _mm256_permute4x64_epi64(m8CD, 0xd8));
      yy_storeu_256(mask + 64, _mm256_permute4x64_epi64(m8EF, 0xd8));
      yy_storeu_256(mask + 96, _mm256_permute4x64_epi64(m8GH, 0xd8));
      src0 += src0_stride;
      src1 += src1_stride;
      mask += 128;
      i += 1;
    } while (i < h);
  }
}
void av1_build_compound_diffwtd_mask_d16_avx2(
    uint8_t *mask, DIFFWTD_MASK_TYPE mask_type, const CONV_BUF_TYPE *src0,
    int src0_stride, const CONV_BUF_TYPE *src1, int src1_stride, int h, int w,
    ConvolveParams *conv_params, int bd) {
  const int shift =
      2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1 + (bd - 8);
  // When rounding constant is added, there is a possibility of overflow.
  // However that much precision is not required. Code should very well work for
  // other values of DIFF_FACTOR_LOG2 and AOM_BLEND_A64_MAX_ALPHA as well. But
  // there is a possibility of corner case bugs.
  assert(DIFF_FACTOR_LOG2 == 4);
  assert(AOM_BLEND_A64_MAX_ALPHA == 64);

  if (mask_type == DIFFWTD_38) {
    build_compound_diffwtd_mask_d16_avx2(mask, src0, src0_stride, src1,
                                         src1_stride, h, w, shift);
  } else {
    build_compound_diffwtd_mask_d16_inv_avx2(mask, src0, src0_stride, src1,
                                             src1_stride, h, w, shift);
  }
}

#endif

#if COMP_AVX // TO be added as wedge_utils_avx2.c
#define MAX_MASK_VALUE (1 << WEDGE_WEIGHT_BITS)

// The _mm256_set1_epi64x() intrinsic is undefined for some Visual Studio
// compilers. The following function is equivalent to _mm256_set1_epi64x()
// acting on a 32-bit integer.
static INLINE __m256i yy_set1_64_from_32i(int32_t a) {
#if defined(_MSC_VER) && defined(_M_IX86) && _MSC_VER < 1900
  return _mm256_set_epi32(0, a, 0, a, 0, a, 0, a);
#else
  return _mm256_set1_epi64x((uint32_t)a);
#endif
}
/**
 * See av1_wedge_sse_from_residuals_c
 */
uint64_t av1_wedge_sse_from_residuals_avx2(const int16_t *r1, const int16_t *d,
                                           const uint8_t *m, int N) {
  int n = -N;

  uint64_t csse;

  const __m256i v_mask_max_w = _mm256_set1_epi16(MAX_MASK_VALUE);
  const __m256i v_zext_q = yy_set1_64_from_32i(0xffffffff);

  __m256i v_acc0_q = _mm256_setzero_si256();

  assert(N % 64 == 0);

  r1 += N;
  d += N;
  m += N;

  do {
    const __m256i v_r0_w = _mm256_lddqu_si256((__m256i *)(r1 + n));
    const __m256i v_d0_w = _mm256_lddqu_si256((__m256i *)(d + n));
    const __m128i v_m01_b = _mm_lddqu_si128((__m128i *)(m + n));

    const __m256i v_rd0l_w = _mm256_unpacklo_epi16(v_d0_w, v_r0_w);
    const __m256i v_rd0h_w = _mm256_unpackhi_epi16(v_d0_w, v_r0_w);
    const __m256i v_m0_w = _mm256_cvtepu8_epi16(v_m01_b);

    const __m256i v_m0l_w = _mm256_unpacklo_epi16(v_m0_w, v_mask_max_w);
    const __m256i v_m0h_w = _mm256_unpackhi_epi16(v_m0_w, v_mask_max_w);

    const __m256i v_t0l_d = _mm256_madd_epi16(v_rd0l_w, v_m0l_w);
    const __m256i v_t0h_d = _mm256_madd_epi16(v_rd0h_w, v_m0h_w);

    const __m256i v_t0_w = _mm256_packs_epi32(v_t0l_d, v_t0h_d);

    const __m256i v_sq0_d = _mm256_madd_epi16(v_t0_w, v_t0_w);

    const __m256i v_sum0_q = _mm256_add_epi64(
        _mm256_and_si256(v_sq0_d, v_zext_q), _mm256_srli_epi64(v_sq0_d, 32));

    v_acc0_q = _mm256_add_epi64(v_acc0_q, v_sum0_q);

    n += 16;
  } while (n);

  v_acc0_q = _mm256_add_epi64(v_acc0_q, _mm256_srli_si256(v_acc0_q, 8));
  __m128i v_acc_q_0 = _mm256_castsi256_si128(v_acc0_q);
  __m128i v_acc_q_1 = _mm256_extracti128_si256(v_acc0_q, 1);
  v_acc_q_0 = _mm_add_epi64(v_acc_q_0, v_acc_q_1);
#if ARCH_X86_64
  csse = (uint64_t)_mm_extract_epi64(v_acc_q_0, 0);
#else
  xx_storel_64(&csse, v_acc_q_0);
#endif

  return ROUND_POWER_OF_TWO(csse, 2 * WEDGE_WEIGHT_BITS);
}
#endif

#if COMP_AVX // TO be added as subtract_avx2.c

static INLINE void subtract32_avx2(int16_t *diff_ptr, const uint8_t *src_ptr,
                                   const uint8_t *pred_ptr) {
  __m256i s = _mm256_lddqu_si256((__m256i *)(src_ptr));
  __m256i p = _mm256_lddqu_si256((__m256i *)(pred_ptr));
  __m256i s_0 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(s));
  __m256i s_1 = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(s, 1));
  __m256i p_0 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(p));
  __m256i p_1 = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(p, 1));
  const __m256i d_0 = _mm256_sub_epi16(s_0, p_0);
  const __m256i d_1 = _mm256_sub_epi16(s_1, p_1);
  _mm256_storeu_si256((__m256i *)(diff_ptr), d_0);
  _mm256_storeu_si256((__m256i *)(diff_ptr + 16), d_1);
}


static INLINE void aom_subtract_block_16xn_avx2(
    int rows, int16_t *diff_ptr, ptrdiff_t diff_stride, const uint8_t *src_ptr,
    ptrdiff_t src_stride, const uint8_t *pred_ptr, ptrdiff_t pred_stride) {
  for (int32_t j = 0; j < rows; ++j) {
    __m128i s = _mm_lddqu_si128((__m128i *)(src_ptr));
    __m128i p = _mm_lddqu_si128((__m128i *)(pred_ptr));
    __m256i s_0 = _mm256_cvtepu8_epi16(s);
    __m256i p_0 = _mm256_cvtepu8_epi16(p);
    const __m256i d_0 = _mm256_sub_epi16(s_0, p_0);
    _mm256_storeu_si256((__m256i *)(diff_ptr), d_0);
    src_ptr += src_stride;
    pred_ptr += pred_stride;
    diff_ptr += diff_stride;
  }
}

static INLINE void aom_subtract_block_32xn_avx2(
    int rows, int16_t *diff_ptr, ptrdiff_t diff_stride, const uint8_t *src_ptr,
    ptrdiff_t src_stride, const uint8_t *pred_ptr, ptrdiff_t pred_stride) {
  for (int32_t j = 0; j < rows; ++j) {
    subtract32_avx2(diff_ptr, src_ptr, pred_ptr);
    src_ptr += src_stride;
    pred_ptr += pred_stride;
    diff_ptr += diff_stride;
  }
}
static INLINE void aom_subtract_block_64xn_avx2(
    int rows, int16_t *diff_ptr, ptrdiff_t diff_stride, const uint8_t *src_ptr,
    ptrdiff_t src_stride, const uint8_t *pred_ptr, ptrdiff_t pred_stride) {
  for (int32_t j = 0; j < rows; ++j) {
    subtract32_avx2(diff_ptr, src_ptr, pred_ptr);
    subtract32_avx2(diff_ptr + 32, src_ptr + 32, pred_ptr + 32);
    src_ptr += src_stride;
    pred_ptr += pred_stride;
    diff_ptr += diff_stride;
  }
}
static INLINE void aom_subtract_block_128xn_avx2(
    int rows, int16_t *diff_ptr, ptrdiff_t diff_stride, const uint8_t *src_ptr,
    ptrdiff_t src_stride, const uint8_t *pred_ptr, ptrdiff_t pred_stride) {
  for (int32_t j = 0; j < rows; ++j) {
    subtract32_avx2(diff_ptr, src_ptr, pred_ptr);
    subtract32_avx2(diff_ptr + 32, src_ptr + 32, pred_ptr + 32);
    subtract32_avx2(diff_ptr + 64, src_ptr + 64, pred_ptr + 64);
    subtract32_avx2(diff_ptr + 96, src_ptr + 96, pred_ptr + 96);
    src_ptr += src_stride;
    pred_ptr += pred_stride;
    diff_ptr += diff_stride;
  }
}
void aom_subtract_block_avx2(int rows, int cols, int16_t *diff_ptr,
                             ptrdiff_t diff_stride, const uint8_t *src_ptr,
                             ptrdiff_t src_stride, const uint8_t *pred_ptr,
                             ptrdiff_t pred_stride) {
  switch (cols) {
    case 16:
      aom_subtract_block_16xn_avx2(rows, diff_ptr, diff_stride, src_ptr,
                                   src_stride, pred_ptr, pred_stride);
      break;
    case 32:
      aom_subtract_block_32xn_avx2(rows, diff_ptr, diff_stride, src_ptr,
                                   src_stride, pred_ptr, pred_stride);
      break;
    case 64:
      aom_subtract_block_64xn_avx2(rows, diff_ptr, diff_stride, src_ptr,
                                   src_stride, pred_ptr, pred_stride);
      break;
    case 128:
      aom_subtract_block_128xn_avx2(rows, diff_ptr, diff_stride, src_ptr,
                                    src_stride, pred_ptr, pred_stride);
      break;
    default:
      aom_subtract_block_sse2(rows, cols, diff_ptr, diff_stride, src_ptr,
                              src_stride, pred_ptr, pred_stride);
      break;
  }
}


#endif
#if COMP_AVX // TO be added as sse_avx2.c
static INLINE void sse_w4x4_avx2(const uint8_t *a, int a_stride,
                                 const uint8_t *b, int b_stride, __m256i *sum) {
  const __m128i v_a0 = xx_loadl_32(a);
  const __m128i v_a1 = xx_loadl_32(a + a_stride);
  const __m128i v_a2 = xx_loadl_32(a + a_stride * 2);
  const __m128i v_a3 = xx_loadl_32(a + a_stride * 3);
  const __m128i v_b0 = xx_loadl_32(b);
  const __m128i v_b1 = xx_loadl_32(b + b_stride);
  const __m128i v_b2 = xx_loadl_32(b + b_stride * 2);
  const __m128i v_b3 = xx_loadl_32(b + b_stride * 3);
  const __m128i v_a0123 = _mm_unpacklo_epi64(_mm_unpacklo_epi32(v_a0, v_a1),
                                             _mm_unpacklo_epi32(v_a2, v_a3));
  const __m128i v_b0123 = _mm_unpacklo_epi64(_mm_unpacklo_epi32(v_b0, v_b1),
                                             _mm_unpacklo_epi32(v_b2, v_b3));
  const __m256i v_a_w = _mm256_cvtepu8_epi16(v_a0123);
  const __m256i v_b_w = _mm256_cvtepu8_epi16(v_b0123);
  const __m256i v_d_w = _mm256_sub_epi16(v_a_w, v_b_w);
  *sum = _mm256_add_epi32(*sum, _mm256_madd_epi16(v_d_w, v_d_w));
}


static INLINE void sse_w8x2_avx2(const uint8_t *a, int a_stride,
                                 const uint8_t *b, int b_stride, __m256i *sum) {
  const __m128i v_a0 = xx_loadl_64(a);
  const __m128i v_a1 = xx_loadl_64(a + a_stride);
  const __m128i v_b0 = xx_loadl_64(b);
  const __m128i v_b1 = xx_loadl_64(b + b_stride);
  const __m256i v_a_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(v_a0, v_a1));
  const __m256i v_b_w = _mm256_cvtepu8_epi16(_mm_unpacklo_epi64(v_b0, v_b1));
  const __m256i v_d_w = _mm256_sub_epi16(v_a_w, v_b_w);
  *sum = _mm256_add_epi32(*sum, _mm256_madd_epi16(v_d_w, v_d_w));
}

static INLINE void sse_w32_avx2(__m256i *sum, const uint8_t *a,
                                const uint8_t *b) {
  const __m256i v_a0 = yy_loadu_256(a);
  const __m256i v_b0 = yy_loadu_256(b);
  const __m256i zero = _mm256_setzero_si256();
  const __m256i v_a00_w = _mm256_unpacklo_epi8(v_a0, zero);
  const __m256i v_a01_w = _mm256_unpackhi_epi8(v_a0, zero);
  const __m256i v_b00_w = _mm256_unpacklo_epi8(v_b0, zero);
  const __m256i v_b01_w = _mm256_unpackhi_epi8(v_b0, zero);
  const __m256i v_d00_w = _mm256_sub_epi16(v_a00_w, v_b00_w);
  const __m256i v_d01_w = _mm256_sub_epi16(v_a01_w, v_b01_w);
  *sum = _mm256_add_epi32(*sum, _mm256_madd_epi16(v_d00_w, v_d00_w));
  *sum = _mm256_add_epi32(*sum, _mm256_madd_epi16(v_d01_w, v_d01_w));
}

static INLINE int64_t summary_all_avx2(const __m256i *sum_all) {
  int64_t sum;
  __m256i zero = _mm256_setzero_si256();
  const __m256i sum0_4x64 = _mm256_unpacklo_epi32(*sum_all, zero);
  const __m256i sum1_4x64 = _mm256_unpackhi_epi32(*sum_all, zero);
  const __m256i sum_4x64 = _mm256_add_epi64(sum0_4x64, sum1_4x64);
  const __m128i sum_2x64 = _mm_add_epi64(_mm256_castsi256_si128(sum_4x64),
                                         _mm256_extracti128_si256(sum_4x64, 1));
  const __m128i sum_1x64 = _mm_add_epi64(sum_2x64, _mm_srli_si128(sum_2x64, 8));
  xx_storel_64(&sum, sum_1x64);
  return sum;
}
int64_t aom_sse_avx2(const uint8_t *a, int a_stride, const uint8_t *b,
                     int b_stride, int width, int height) {
  int32_t y = 0;
  int64_t sse = 0;
  __m256i sum = _mm256_setzero_si256();
  __m256i zero = _mm256_setzero_si256();
  switch (width) {
    case 4:
      do {
        sse_w4x4_avx2(a, a_stride, b, b_stride, &sum);
        a += a_stride << 2;
        b += b_stride << 2;
        y += 4;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    case 8:
      do {
        sse_w8x2_avx2(a, a_stride, b, b_stride, &sum);
        a += a_stride << 1;
        b += b_stride << 1;
        y += 2;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    case 16:
      do {
        const __m128i v_a0 = xx_loadu_128(a);
        const __m128i v_a1 = xx_loadu_128(a + a_stride);
        const __m128i v_b0 = xx_loadu_128(b);
        const __m128i v_b1 = xx_loadu_128(b + b_stride);
        const __m256i v_a =
            _mm256_insertf128_si256(_mm256_castsi128_si256(v_a0), v_a1, 0x01);
        const __m256i v_b =
            _mm256_insertf128_si256(_mm256_castsi128_si256(v_b0), v_b1, 0x01);
        const __m256i v_al = _mm256_unpacklo_epi8(v_a, zero);
        const __m256i v_au = _mm256_unpackhi_epi8(v_a, zero);
        const __m256i v_bl = _mm256_unpacklo_epi8(v_b, zero);
        const __m256i v_bu = _mm256_unpackhi_epi8(v_b, zero);
        const __m256i v_asub = _mm256_sub_epi16(v_al, v_bl);
        const __m256i v_bsub = _mm256_sub_epi16(v_au, v_bu);
        const __m256i temp =
            _mm256_add_epi32(_mm256_madd_epi16(v_asub, v_asub),
                             _mm256_madd_epi16(v_bsub, v_bsub));
        sum = _mm256_add_epi32(sum, temp);
        a += a_stride << 1;
        b += b_stride << 1;
        y += 2;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    case 32:
      do {
        sse_w32_avx2(&sum, a, b);
        a += a_stride;
        b += b_stride;
        y += 1;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    case 64:
      do {
        sse_w32_avx2(&sum, a, b);
        sse_w32_avx2(&sum, a + 32, b + 32);
        a += a_stride;
        b += b_stride;
        y += 1;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    case 128:
      do {
        sse_w32_avx2(&sum, a, b);
        sse_w32_avx2(&sum, a + 32, b + 32);
        sse_w32_avx2(&sum, a + 64, b + 64);
        sse_w32_avx2(&sum, a + 96, b + 96);
        a += a_stride;
        b += b_stride;
        y += 1;
      } while (y < height);
      sse = summary_all_avx2(&sum);
      break;
    default:
      if ((width & 0x07) == 0) {
        do {
          int i = 0;
          do {
            sse_w8x2_avx2(a + i, a_stride, b + i, b_stride, &sum);
            i += 8;
          } while (i < width);
          a += a_stride << 1;
          b += b_stride << 1;
          y += 2;
        } while (y < height);
      } else {
        do {
          int i = 0;
          do {
            sse_w8x2_avx2(a + i, a_stride, b + i, b_stride, &sum);
            const uint8_t *a2 = a + i + (a_stride << 1);
            const uint8_t *b2 = b + i + (b_stride << 1);
            sse_w8x2_avx2(a2, a_stride, b2, b_stride, &sum);
            i += 8;
          } while (i + 4 < width);
          sse_w4x4_avx2(a + i, a_stride, b + i, b_stride, &sum);
          a += a_stride << 2;
          b += b_stride << 2;
          y += 4;
        } while (y < height);
      }
      sse = summary_all_avx2(&sum);
      break;
  }

  return sse;
}

#endif


#if COMP_AVX // to be added as blend_mask_sse4.h


static INLINE void blend_a64_d16_mask_w4_sse41(
    uint8_t *dst, const CONV_BUF_TYPE *src0, const CONV_BUF_TYPE *src1,
    const __m128i *m, const __m128i *v_round_offset, const __m128i *v_maxval,
    int shift) {
  const __m128i max_minus_m = _mm_sub_epi16(*v_maxval, *m);
  const __m128i s0 = xx_loadl_64(src0);
  const __m128i s1 = xx_loadl_64(src1);
  const __m128i s0_s1 = _mm_unpacklo_epi16(s0, s1);
  const __m128i m_max_minus_m = _mm_unpacklo_epi16(*m, max_minus_m);
  const __m128i res_a = _mm_madd_epi16(s0_s1, m_max_minus_m);
  const __m128i res_c = _mm_sub_epi32(res_a, *v_round_offset);
  const __m128i res_d = _mm_srai_epi32(res_c, shift);
  const __m128i res_e = _mm_packs_epi32(res_d, res_d);
  const __m128i res = _mm_packus_epi16(res_e, res_e);

  xx_storel_32(dst, res);
}

static INLINE void blend_a64_d16_mask_w8_sse41(
    uint8_t *dst, const CONV_BUF_TYPE *src0, const CONV_BUF_TYPE *src1,
    const __m128i *m, const __m128i *v_round_offset, const __m128i *v_maxval,
    int shift) {
  const __m128i max_minus_m = _mm_sub_epi16(*v_maxval, *m);
  const __m128i s0 = xx_loadu_128(src0);
  const __m128i s1 = xx_loadu_128(src1);
  __m128i res_lo = _mm_madd_epi16(_mm_unpacklo_epi16(s0, s1),
                                  _mm_unpacklo_epi16(*m, max_minus_m));
  __m128i res_hi = _mm_madd_epi16(_mm_unpackhi_epi16(s0, s1),
                                  _mm_unpackhi_epi16(*m, max_minus_m));
  res_lo = _mm_srai_epi32(_mm_sub_epi32(res_lo, *v_round_offset), shift);
  res_hi = _mm_srai_epi32(_mm_sub_epi32(res_hi, *v_round_offset), shift);
  const __m128i res_e = _mm_packs_epi32(res_lo, res_hi);
  const __m128i res = _mm_packus_epi16(res_e, res_e);

  _mm_storel_epi64((__m128i *)(dst), res);
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw0_subh0_w4_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  for (int i = 0; i < h; ++i) {
    const __m128i m0 = xx_loadl_32(mask);
    const __m128i m = _mm_cvtepu8_epi16(m0);

    blend_a64_d16_mask_w4_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw0_subh0_w8_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  for (int i = 0; i < h; ++i) {
    const __m128i m0 = xx_loadl_64(mask);
    const __m128i m = _mm_cvtepu8_epi16(m0);
    blend_a64_d16_mask_w8_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw1_subh1_w4_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i one_b = _mm_set1_epi8(1);
  const __m128i two_w = _mm_set1_epi16(2);
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadl_64(mask);
    const __m128i m_i1 = xx_loadl_64(mask + mask_stride);
    const __m128i m_ac = _mm_adds_epu8(m_i0, m_i1);
    const __m128i m_acbd = _mm_maddubs_epi16(m_ac, one_b);
    const __m128i m_acbd_2 = _mm_add_epi16(m_acbd, two_w);
    const __m128i m = _mm_srli_epi16(m_acbd_2, 2);

    blend_a64_d16_mask_w4_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw1_subh1_w8_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i one_b = _mm_set1_epi8(1);
  const __m128i two_w = _mm_set1_epi16(2);
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadu_128(mask);
    const __m128i m_i1 = xx_loadu_128(mask + mask_stride);
    const __m128i m_ac = _mm_adds_epu8(m_i0, m_i1);
    const __m128i m_acbd = _mm_maddubs_epi16(m_ac, one_b);
    const __m128i m_acbd_2 = _mm_add_epi16(m_acbd, two_w);
    const __m128i m = _mm_srli_epi16(m_acbd_2, 2);

    blend_a64_d16_mask_w8_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw1_subh0_w4_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i one_b = _mm_set1_epi8(1);
  const __m128i zeros = _mm_setzero_si128();
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadl_64(mask);
    const __m128i m_ac = _mm_maddubs_epi16(m_i0, one_b);
    const __m128i m = _mm_avg_epu16(m_ac, zeros);

    blend_a64_d16_mask_w4_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw1_subh0_w8_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i one_b = _mm_set1_epi8(1);
  const __m128i zeros = _mm_setzero_si128();
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadu_128(mask);
    const __m128i m_ac = _mm_maddubs_epi16(m_i0, one_b);
    const __m128i m = _mm_avg_epu16(m_ac, zeros);

    blend_a64_d16_mask_w8_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}
static INLINE void aom_lowbd_blend_a64_d16_mask_subw0_subh1_w4_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i zeros = _mm_setzero_si128();
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadl_64(mask);
    const __m128i m_i1 = xx_loadl_64(mask + mask_stride);
    const __m128i m_ac = _mm_adds_epu8(m_i0, m_i1);
    const __m128i m = _mm_cvtepu8_epi16(_mm_avg_epu8(m_ac, zeros));

    blend_a64_d16_mask_w4_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void aom_lowbd_blend_a64_d16_mask_subw0_subh1_w8_sse4_1(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m128i *round_offset, int shift) {
  const __m128i v_maxval = _mm_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i zeros = _mm_setzero_si128();
  for (int i = 0; i < h; ++i) {
    const __m128i m_i0 = xx_loadl_64(mask);
    const __m128i m_i1 = xx_loadl_64(mask + mask_stride);
    const __m128i m_ac = _mm_adds_epu8(m_i0, m_i1);
    const __m128i m = _mm_cvtepu8_epi16(_mm_avg_epu8(m_ac, zeros));

    blend_a64_d16_mask_w8_sse41(dst, src0, src1, &m, round_offset, &v_maxval,
                                shift);
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}


#endif

#if COMP_AVX // to be added as blend_a64_mask_avx2.c
static INLINE void blend_a64_d16_mask_w16_avx2(
    uint8_t *dst, const CONV_BUF_TYPE *src0, const CONV_BUF_TYPE *src1,
    const __m256i *m0, const __m256i *v_round_offset, const __m256i *v_maxval,
    int shift) {
  const __m256i max_minus_m0 = _mm256_sub_epi16(*v_maxval, *m0);
  const __m256i s0_0 = yy_loadu_256(src0);
  const __m256i s1_0 = yy_loadu_256(src1);
  __m256i res0_lo = _mm256_madd_epi16(_mm256_unpacklo_epi16(s0_0, s1_0),
                                      _mm256_unpacklo_epi16(*m0, max_minus_m0));
  __m256i res0_hi = _mm256_madd_epi16(_mm256_unpackhi_epi16(s0_0, s1_0),
                                      _mm256_unpackhi_epi16(*m0, max_minus_m0));
  res0_lo =
      _mm256_srai_epi32(_mm256_sub_epi32(res0_lo, *v_round_offset), shift);
  res0_hi =
      _mm256_srai_epi32(_mm256_sub_epi32(res0_hi, *v_round_offset), shift);
  const __m256i res0 = _mm256_packs_epi32(res0_lo, res0_hi);
  __m256i res = _mm256_packus_epi16(res0, res0);
  res = _mm256_permute4x64_epi64(res, 0xd8);
  _mm_storeu_si128((__m128i *)(dst), _mm256_castsi256_si128(res));
}

static INLINE void blend_a64_d16_mask_w32_avx2(
    uint8_t *dst, const CONV_BUF_TYPE *src0, const CONV_BUF_TYPE *src1,
    const __m256i *m0, const __m256i *m1, const __m256i *v_round_offset,
    const __m256i *v_maxval, int shift) {
  const __m256i max_minus_m0 = _mm256_sub_epi16(*v_maxval, *m0);
  const __m256i max_minus_m1 = _mm256_sub_epi16(*v_maxval, *m1);
  const __m256i s0_0 = yy_loadu_256(src0);
  const __m256i s0_1 = yy_loadu_256(src0 + 16);
  const __m256i s1_0 = yy_loadu_256(src1);
  const __m256i s1_1 = yy_loadu_256(src1 + 16);
  __m256i res0_lo = _mm256_madd_epi16(_mm256_unpacklo_epi16(s0_0, s1_0),
                                      _mm256_unpacklo_epi16(*m0, max_minus_m0));
  __m256i res0_hi = _mm256_madd_epi16(_mm256_unpackhi_epi16(s0_0, s1_0),
                                      _mm256_unpackhi_epi16(*m0, max_minus_m0));
  __m256i res1_lo = _mm256_madd_epi16(_mm256_unpacklo_epi16(s0_1, s1_1),
                                      _mm256_unpacklo_epi16(*m1, max_minus_m1));
  __m256i res1_hi = _mm256_madd_epi16(_mm256_unpackhi_epi16(s0_1, s1_1),
                                      _mm256_unpackhi_epi16(*m1, max_minus_m1));
  res0_lo =
      _mm256_srai_epi32(_mm256_sub_epi32(res0_lo, *v_round_offset), shift);
  res0_hi =
      _mm256_srai_epi32(_mm256_sub_epi32(res0_hi, *v_round_offset), shift);
  res1_lo =
      _mm256_srai_epi32(_mm256_sub_epi32(res1_lo, *v_round_offset), shift);
  res1_hi =
      _mm256_srai_epi32(_mm256_sub_epi32(res1_hi, *v_round_offset), shift);
  const __m256i res0 = _mm256_packs_epi32(res0_lo, res0_hi);
  const __m256i res1 = _mm256_packs_epi32(res1_lo, res1_hi);
  __m256i res = _mm256_packus_epi16(res0, res1);
  res = _mm256_permute4x64_epi64(res, 0xd8);
  _mm256_storeu_si256((__m256i *)(dst), res);
}

static INLINE void lowbd_blend_a64_d16_mask_subw0_subh0_w16_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  for (int i = 0; i < h; ++i) {
    const __m128i m = xx_loadu_128(mask);
    const __m256i m0 = _mm256_cvtepu8_epi16(m);

    blend_a64_d16_mask_w16_avx2(dst, src0, src1, &m0, round_offset, &v_maxval,
                                shift);
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw0_subh0_w32_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 32) {
      const __m256i m = yy_loadu_256(mask + j);
      const __m256i m0 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(m));
      const __m256i m1 = _mm256_cvtepu8_epi16(_mm256_extracti128_si256(m, 1));

      blend_a64_d16_mask_w32_avx2(dst + j, src0 + j, src1 + j, &m0, &m1,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw1_subh1_w16_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m256i one_b = _mm256_set1_epi8(1);
  const __m256i two_w = _mm256_set1_epi16(2);
  for (int i = 0; i < h; ++i) {
    const __m256i m_i00 = yy_loadu_256(mask);
    const __m256i m_i10 = yy_loadu_256(mask + mask_stride);

    const __m256i m0_ac = _mm256_adds_epu8(m_i00, m_i10);
    const __m256i m0_acbd = _mm256_maddubs_epi16(m0_ac, one_b);
    const __m256i m0 = _mm256_srli_epi16(_mm256_add_epi16(m0_acbd, two_w), 2);

    blend_a64_d16_mask_w16_avx2(dst, src0, src1, &m0, round_offset, &v_maxval,
                                shift);
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw1_subh1_w32_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m256i one_b = _mm256_set1_epi8(1);
  const __m256i two_w = _mm256_set1_epi16(2);
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 32) {
      const __m256i m_i00 = yy_loadu_256(mask + 2 * j);
      const __m256i m_i01 = yy_loadu_256(mask + 2 * j + 32);
      const __m256i m_i10 = yy_loadu_256(mask + mask_stride + 2 * j);
      const __m256i m_i11 = yy_loadu_256(mask + mask_stride + 2 * j + 32);

      const __m256i m0_ac = _mm256_adds_epu8(m_i00, m_i10);
      const __m256i m1_ac = _mm256_adds_epu8(m_i01, m_i11);
      const __m256i m0_acbd = _mm256_maddubs_epi16(m0_ac, one_b);
      const __m256i m1_acbd = _mm256_maddubs_epi16(m1_ac, one_b);
      const __m256i m0 = _mm256_srli_epi16(_mm256_add_epi16(m0_acbd, two_w), 2);
      const __m256i m1 = _mm256_srli_epi16(_mm256_add_epi16(m1_acbd, two_w), 2);

      blend_a64_d16_mask_w32_avx2(dst + j, src0 + j, src1 + j, &m0, &m1,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw1_subh0_w16_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m256i one_b = _mm256_set1_epi8(1);
  const __m256i zeros = _mm256_setzero_si256();
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 16) {
      const __m256i m_i00 = yy_loadu_256(mask + 2 * j);
      const __m256i m0_ac = _mm256_maddubs_epi16(m_i00, one_b);
      const __m256i m0 = _mm256_avg_epu16(m0_ac, zeros);

      blend_a64_d16_mask_w16_avx2(dst + j, src0 + j, src1 + j, &m0,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw1_subh0_w32_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m256i one_b = _mm256_set1_epi8(1);
  const __m256i zeros = _mm256_setzero_si256();
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 32) {
      const __m256i m_i00 = yy_loadu_256(mask + 2 * j);
      const __m256i m_i01 = yy_loadu_256(mask + 2 * j + 32);
      const __m256i m0_ac = _mm256_maddubs_epi16(m_i00, one_b);
      const __m256i m1_ac = _mm256_maddubs_epi16(m_i01, one_b);
      const __m256i m0 = _mm256_avg_epu16(m0_ac, zeros);
      const __m256i m1 = _mm256_avg_epu16(m1_ac, zeros);

      blend_a64_d16_mask_w32_avx2(dst + j, src0 + j, src1 + j, &m0, &m1,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw0_subh1_w16_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m128i zeros = _mm_setzero_si128();
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 16) {
      const __m128i m_i00 = xx_loadu_128(mask + j);
      const __m128i m_i10 = xx_loadu_128(mask + mask_stride + j);

      const __m128i m_ac = _mm_avg_epu8(_mm_adds_epu8(m_i00, m_i10), zeros);
      const __m256i m0 = _mm256_cvtepu8_epi16(m_ac);

      blend_a64_d16_mask_w16_avx2(dst + j, src0 + j, src1 + j, &m0,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}

static INLINE void lowbd_blend_a64_d16_mask_subw0_subh1_w32_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int h, int w,
    const __m256i *round_offset, int shift) {
  const __m256i v_maxval = _mm256_set1_epi16(AOM_BLEND_A64_MAX_ALPHA);
  const __m256i zeros = _mm256_setzero_si256();
  for (int i = 0; i < h; ++i) {
    for (int j = 0; j < w; j += 32) {
      const __m256i m_i00 = yy_loadu_256(mask + j);
      const __m256i m_i10 = yy_loadu_256(mask + mask_stride + j);

      const __m256i m_ac =
          _mm256_avg_epu8(_mm256_adds_epu8(m_i00, m_i10), zeros);
      const __m256i m0 = _mm256_cvtepu8_epi16(_mm256_castsi256_si128(m_ac));
      const __m256i m1 =
          _mm256_cvtepu8_epi16(_mm256_extracti128_si256(m_ac, 1));

      blend_a64_d16_mask_w32_avx2(dst + j, src0 + j, src1 + j, &m0, &m1,
                                  round_offset, &v_maxval, shift);
    }
    mask += mask_stride << 1;
    dst += dst_stride;
    src0 += src0_stride;
    src1 += src1_stride;
  }
}
#define IS_POWER_OF_TWO(x) (((x) & ((x)-1)) == 0)

void aom_lowbd_blend_a64_d16_mask_avx2(
    uint8_t *dst, uint32_t dst_stride, const CONV_BUF_TYPE *src0,
    uint32_t src0_stride, const CONV_BUF_TYPE *src1, uint32_t src1_stride,
    const uint8_t *mask, uint32_t mask_stride, int w, int h, int subw, int subh,
    ConvolveParams *conv_params) {
  const int bd = 8;
  const int round_bits =
      2 * FILTER_BITS - conv_params->round_0 - conv_params->round_1;

  const int round_offset =
      ((1 << (round_bits + bd)) + (1 << (round_bits + bd - 1)) -
       (1 << (round_bits - 1)))
      << AOM_BLEND_A64_ROUND_BITS;

  const int shift = round_bits + AOM_BLEND_A64_ROUND_BITS;
  assert(IMPLIES((void *)src0 == dst, src0_stride == dst_stride));
  assert(IMPLIES((void *)src1 == dst, src1_stride == dst_stride));

  assert(h >= 4);
  assert(w >= 4);
  assert(IS_POWER_OF_TWO(h));
  assert(IS_POWER_OF_TWO(w));
  const __m128i v_round_offset = _mm_set1_epi32(round_offset);
  const __m256i y_round_offset = _mm256_set1_epi32(round_offset);

  if (subw == 0 && subh == 0) {
    switch (w) {
      case 4:
        aom_lowbd_blend_a64_d16_mask_subw0_subh0_w4_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 8:
        aom_lowbd_blend_a64_d16_mask_subw0_subh0_w8_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 16:
        lowbd_blend_a64_d16_mask_subw0_subh0_w16_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &y_round_offset, shift);
        break;
      default:
        lowbd_blend_a64_d16_mask_subw0_subh0_w32_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
    }
  } else if (subw == 1 && subh == 1) {
    switch (w) {
      case 4:
        aom_lowbd_blend_a64_d16_mask_subw1_subh1_w4_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 8:
        aom_lowbd_blend_a64_d16_mask_subw1_subh1_w8_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 16:
        lowbd_blend_a64_d16_mask_subw1_subh1_w16_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &y_round_offset, shift);
        break;
      default:
        lowbd_blend_a64_d16_mask_subw1_subh1_w32_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
    }
  } else if (subw == 1 && subh == 0) {
    switch (w) {
      case 4:
        aom_lowbd_blend_a64_d16_mask_subw1_subh0_w4_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 8:
        aom_lowbd_blend_a64_d16_mask_subw1_subh0_w8_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 16:
        lowbd_blend_a64_d16_mask_subw1_subh0_w16_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
      default:
        lowbd_blend_a64_d16_mask_subw1_subh0_w32_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
    }
  } else {
    switch (w) {
      case 4:
        aom_lowbd_blend_a64_d16_mask_subw0_subh1_w4_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 8:
        aom_lowbd_blend_a64_d16_mask_subw0_subh1_w8_sse4_1(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, &v_round_offset, shift);
        break;
      case 16:
        lowbd_blend_a64_d16_mask_subw0_subh1_w16_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
      default:
        lowbd_blend_a64_d16_mask_subw0_subh1_w32_avx2(
            dst, dst_stride, src0, src0_stride, src1, src1_stride, mask,
            mask_stride, h, w, &y_round_offset, shift);
        break;
    }
  }
}



#endif

//
//#if COMP_AVX // TO be added as  wedge_utils_avx2.c
//
//static INLINE void xx_store_128(void *const a, const __m128i v) {
//  _mm_store_si128((__m128i *)a, v);
//}
//
//
//// Negate under mask
//static INLINE __m128i negm_epi16(__m128i v_v_w, __m128i v_mask_w) {
//  return _mm_sub_epi16(_mm_xor_si128(v_v_w, v_mask_w), v_mask_w);
//}
//
//
//
//#endif

#if COMP_AVX // sum_squares_sse2.c

static INLINE uint64_t xx_cvtsi128_si64(__m128i a) {
#if ARCH_X86_64
    return (uint64_t)_mm_cvtsi128_si64(a);
#else
    {
        uint64_t tmp;
        _mm_storel_epi64((__m128i *)&tmp, a);
        return tmp;
    }
#endif

}
static uint64_t aom_sum_squares_i16_64n_sse2(const int16_t *src, uint32_t n) {
  const __m128i v_zext_mask_q = xx_set1_64_from_32i(0xffffffff);
  __m128i v_acc0_q = _mm_setzero_si128();
  __m128i v_acc1_q = _mm_setzero_si128();

  const int16_t *const end = src + n;

  assert(n % 64 == 0);

  while (src < end) {
    const __m128i v_val_0_w = xx_load_128(src);
    const __m128i v_val_1_w = xx_load_128(src + 8);
    const __m128i v_val_2_w = xx_load_128(src + 16);
    const __m128i v_val_3_w = xx_load_128(src + 24);
    const __m128i v_val_4_w = xx_load_128(src + 32);
    const __m128i v_val_5_w = xx_load_128(src + 40);
    const __m128i v_val_6_w = xx_load_128(src + 48);
    const __m128i v_val_7_w = xx_load_128(src + 56);

    const __m128i v_sq_0_d = _mm_madd_epi16(v_val_0_w, v_val_0_w);
    const __m128i v_sq_1_d = _mm_madd_epi16(v_val_1_w, v_val_1_w);
    const __m128i v_sq_2_d = _mm_madd_epi16(v_val_2_w, v_val_2_w);
    const __m128i v_sq_3_d = _mm_madd_epi16(v_val_3_w, v_val_3_w);
    const __m128i v_sq_4_d = _mm_madd_epi16(v_val_4_w, v_val_4_w);
    const __m128i v_sq_5_d = _mm_madd_epi16(v_val_5_w, v_val_5_w);
    const __m128i v_sq_6_d = _mm_madd_epi16(v_val_6_w, v_val_6_w);
    const __m128i v_sq_7_d = _mm_madd_epi16(v_val_7_w, v_val_7_w);

    const __m128i v_sum_01_d = _mm_add_epi32(v_sq_0_d, v_sq_1_d);
    const __m128i v_sum_23_d = _mm_add_epi32(v_sq_2_d, v_sq_3_d);
    const __m128i v_sum_45_d = _mm_add_epi32(v_sq_4_d, v_sq_5_d);
    const __m128i v_sum_67_d = _mm_add_epi32(v_sq_6_d, v_sq_7_d);

    const __m128i v_sum_0123_d = _mm_add_epi32(v_sum_01_d, v_sum_23_d);
    const __m128i v_sum_4567_d = _mm_add_epi32(v_sum_45_d, v_sum_67_d);

    const __m128i v_sum_d = _mm_add_epi32(v_sum_0123_d, v_sum_4567_d);

    v_acc0_q = _mm_add_epi64(v_acc0_q, _mm_and_si128(v_sum_d, v_zext_mask_q));
    v_acc1_q = _mm_add_epi64(v_acc1_q, _mm_srli_epi64(v_sum_d, 32));

    src += 64;
  }

  v_acc0_q = _mm_add_epi64(v_acc0_q, v_acc1_q);
  v_acc0_q = _mm_add_epi64(v_acc0_q, _mm_srli_si128(v_acc0_q, 8));
  return xx_cvtsi128_si64(v_acc0_q);
}

uint64_t aom_sum_squares_i16_sse2(const int16_t *src, uint32_t n) {
  if (n % 64 == 0) {
    return aom_sum_squares_i16_64n_sse2(src, n);
  } else if (n > 64) {
    int k = n & ~(64 - 1);
    return aom_sum_squares_i16_64n_sse2(src, k) +
           aom_sum_squares_i16_c(src + k, n - k);
  } else {
    return aom_sum_squares_i16_c(src, n);
  }
}


#endif

#if COMP_AVX // wedge_utils_avx2.c

/**
 * See av1_wedge_sign_from_residuals_c
 */
int8_t av1_wedge_sign_from_residuals_avx2(const int16_t *ds, const uint8_t *m,
                                          int N, int64_t limit) {
  int64_t acc;
  __m256i v_acc0_d = _mm256_setzero_si256();

  // Input size limited to 8192 by the use of 32 bit accumulators and m
  // being between [0, 64]. Overflow might happen at larger sizes,
  // though it is practically impossible on real video input.
  assert(N < 8192);
  assert(N % 64 == 0);

  do {
    const __m256i v_m01_b = _mm256_lddqu_si256((__m256i *)(m));
    const __m256i v_m23_b = _mm256_lddqu_si256((__m256i *)(m + 32));

    const __m256i v_d0_w = _mm256_lddqu_si256((__m256i *)(ds));
    const __m256i v_d1_w = _mm256_lddqu_si256((__m256i *)(ds + 16));
    const __m256i v_d2_w = _mm256_lddqu_si256((__m256i *)(ds + 32));
    const __m256i v_d3_w = _mm256_lddqu_si256((__m256i *)(ds + 48));

    const __m256i v_m0_w =
        _mm256_cvtepu8_epi16(_mm256_castsi256_si128(v_m01_b));
    const __m256i v_m1_w =
        _mm256_cvtepu8_epi16(_mm256_extracti128_si256(v_m01_b, 1));
    const __m256i v_m2_w =
        _mm256_cvtepu8_epi16(_mm256_castsi256_si128(v_m23_b));
    const __m256i v_m3_w =
        _mm256_cvtepu8_epi16(_mm256_extracti128_si256(v_m23_b, 1));

    const __m256i v_p0_d = _mm256_madd_epi16(v_d0_w, v_m0_w);
    const __m256i v_p1_d = _mm256_madd_epi16(v_d1_w, v_m1_w);
    const __m256i v_p2_d = _mm256_madd_epi16(v_d2_w, v_m2_w);
    const __m256i v_p3_d = _mm256_madd_epi16(v_d3_w, v_m3_w);

    const __m256i v_p01_d = _mm256_add_epi32(v_p0_d, v_p1_d);
    const __m256i v_p23_d = _mm256_add_epi32(v_p2_d, v_p3_d);

    const __m256i v_p0123_d = _mm256_add_epi32(v_p01_d, v_p23_d);

    v_acc0_d = _mm256_add_epi32(v_acc0_d, v_p0123_d);

    ds += 64;
    m += 64;

    N -= 64;
  } while (N);

  __m256i v_sign_d = _mm256_srai_epi32(v_acc0_d, 31);
  v_acc0_d = _mm256_add_epi64(_mm256_unpacklo_epi32(v_acc0_d, v_sign_d),
                              _mm256_unpackhi_epi32(v_acc0_d, v_sign_d));

  __m256i v_acc_q = _mm256_add_epi64(v_acc0_d, _mm256_srli_si256(v_acc0_d, 8));

  __m128i v_acc_q_0 = _mm256_castsi256_si128(v_acc_q);
  __m128i v_acc_q_1 = _mm256_extracti128_si256(v_acc_q, 1);
  v_acc_q_0 = _mm_add_epi64(v_acc_q_0, v_acc_q_1);

#if ARCH_X86_64
  acc = (uint64_t)_mm_extract_epi64(v_acc_q_0, 0);
#else
  xx_storel_64(&acc, v_acc_q_0);
#endif

  return acc > limit;
}


/**
 * av1_wedge_compute_delta_squares_c
 */
void av1_wedge_compute_delta_squares_avx2(int16_t *d, const int16_t *a,
                                          const int16_t *b, int N) {
  const __m256i v_neg_w = _mm256_set1_epi32(0xffff0001);

  assert(N % 64 == 0);

  do {
    const __m256i v_a0_w = _mm256_lddqu_si256((__m256i *)(a));
    const __m256i v_b0_w = _mm256_lddqu_si256((__m256i *)(b));
    const __m256i v_a1_w = _mm256_lddqu_si256((__m256i *)(a + 16));
    const __m256i v_b1_w = _mm256_lddqu_si256((__m256i *)(b + 16));
    const __m256i v_a2_w = _mm256_lddqu_si256((__m256i *)(a + 32));
    const __m256i v_b2_w = _mm256_lddqu_si256((__m256i *)(b + 32));
    const __m256i v_a3_w = _mm256_lddqu_si256((__m256i *)(a + 48));
    const __m256i v_b3_w = _mm256_lddqu_si256((__m256i *)(b + 48));

    const __m256i v_ab0l_w = _mm256_unpacklo_epi16(v_a0_w, v_b0_w);
    const __m256i v_ab0h_w = _mm256_unpackhi_epi16(v_a0_w, v_b0_w);
    const __m256i v_ab1l_w = _mm256_unpacklo_epi16(v_a1_w, v_b1_w);
    const __m256i v_ab1h_w = _mm256_unpackhi_epi16(v_a1_w, v_b1_w);
    const __m256i v_ab2l_w = _mm256_unpacklo_epi16(v_a2_w, v_b2_w);
    const __m256i v_ab2h_w = _mm256_unpackhi_epi16(v_a2_w, v_b2_w);
    const __m256i v_ab3l_w = _mm256_unpacklo_epi16(v_a3_w, v_b3_w);
    const __m256i v_ab3h_w = _mm256_unpackhi_epi16(v_a3_w, v_b3_w);

    // Negate top word of pairs
    const __m256i v_abl0n_w = _mm256_sign_epi16(v_ab0l_w, v_neg_w);
    const __m256i v_abh0n_w = _mm256_sign_epi16(v_ab0h_w, v_neg_w);
    const __m256i v_abl1n_w = _mm256_sign_epi16(v_ab1l_w, v_neg_w);
    const __m256i v_abh1n_w = _mm256_sign_epi16(v_ab1h_w, v_neg_w);
    const __m256i v_abl2n_w = _mm256_sign_epi16(v_ab2l_w, v_neg_w);
    const __m256i v_abh2n_w = _mm256_sign_epi16(v_ab2h_w, v_neg_w);
    const __m256i v_abl3n_w = _mm256_sign_epi16(v_ab3l_w, v_neg_w);
    const __m256i v_abh3n_w = _mm256_sign_epi16(v_ab3h_w, v_neg_w);

    const __m256i v_r0l_w = _mm256_madd_epi16(v_ab0l_w, v_abl0n_w);
    const __m256i v_r0h_w = _mm256_madd_epi16(v_ab0h_w, v_abh0n_w);
    const __m256i v_r1l_w = _mm256_madd_epi16(v_ab1l_w, v_abl1n_w);
    const __m256i v_r1h_w = _mm256_madd_epi16(v_ab1h_w, v_abh1n_w);
    const __m256i v_r2l_w = _mm256_madd_epi16(v_ab2l_w, v_abl2n_w);
    const __m256i v_r2h_w = _mm256_madd_epi16(v_ab2h_w, v_abh2n_w);
    const __m256i v_r3l_w = _mm256_madd_epi16(v_ab3l_w, v_abl3n_w);
    const __m256i v_r3h_w = _mm256_madd_epi16(v_ab3h_w, v_abh3n_w);

    const __m256i v_r0_w = _mm256_packs_epi32(v_r0l_w, v_r0h_w);
    const __m256i v_r1_w = _mm256_packs_epi32(v_r1l_w, v_r1h_w);
    const __m256i v_r2_w = _mm256_packs_epi32(v_r2l_w, v_r2h_w);
    const __m256i v_r3_w = _mm256_packs_epi32(v_r3l_w, v_r3h_w);

    _mm256_storeu_si256((__m256i *)(d), v_r0_w);
    _mm256_storeu_si256((__m256i *)(d + 16), v_r1_w);
    _mm256_storeu_si256((__m256i *)(d + 32), v_r2_w);
    _mm256_storeu_si256((__m256i *)(d + 48), v_r3_w);

    a += 64;
    b += 64;
    d += 64;
    N -= 64;
  } while (N);
}
#endif
