/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecStruct_h
#define EbDecStruct_h

#include "EbPictureBufferDesc.h"
#include "EbPictureControlSet.h"

#include "EbSvtAv1Dec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TO enable some debug checks for coeff producer and consumer */
#define SVT_DEC_COEFF_DEBUG    1

typedef struct DecoderModelInfo {
    /*!< Specifies the length of the decoder_buffer_delay and the
     * encoder_buffer_delay syntax elements, in bits*/
    uint8_t     buffer_delay_length_minus_1;

    /*!< Number of time units of a decoding clock operating at the frequency
     *time_scale Hz that corresponds to one increment of a clock tick counter*/
    uint32_t    num_units_in_decoding_tick;

    /*!<Specifies the length of the buffer_removal_time syntax element,in bits*/
    uint8_t     buffer_removal_time_length_minus_1;

    /*!< Specifies the length of the frame_presentation_time syntax element,
     * in bits*/
    uint8_t     frame_presentation_time_length_minus_1;
} DecoderModelInfo;

typedef struct OrderHintInfo {
    /*!<1: Indicates that tools based on the values of order hints may be used.
     *  0: Indicates that tools based on order hints are disabled */
    uint8_t             enable_order_hint;

    /*!< 1: Indicates that the distance weights process may be used for
     * inter prediction */
    uint8_t             enable_jnt_comp;

    /*!< 1: Indicates that the use_ref_frame_mvs syntax element may be present.
     *   0: Indicates that the use_ref_frame_mvs syntax element will not be
            present */
    uint8_t             enable_ref_frame_mvs;

    /*!< Used to compute OrderHintBits*/
    uint8_t             order_hint_bits;
} OrderHintInfo;

typedef struct SeqHeader {
    /*!< Specifies the features that can be used in the coded video sequence */
    EbAv1SeqProfile   seq_profile;

    /*!<1: Specifies that the coded video sequence contains only one coded frame
     *  0: Specifies that the coded video sequence contains one or more coded
           frames */
    uint8_t             still_picture;

    /*!< Specifies that the syntax elements not needed by a still picture are
     * omitted */
    uint8_t             reduced_still_picture_header;

    /*!< Timing Information structure*/
    EbTimingInfo          timing_info;

    /*!< Specifies whether decoder model information is present in the coded
     * video sequence */
    uint8_t             decoder_model_info_present_flag;

    /*!< Decoder Mode Information structure*/
    DecoderModelInfo    decoder_model_info;

    /*!< Specifies whether initial display delay information is present in the
     * coded video sequence.*/
    uint8_t             initial_display_delay_present_flag;

    /*!< Indicates the number of operating points minus 1 present in the coded video sequence*/
    uint8_t             operating_points_cnt_minus_1;

    /*!< Operating Point Param structure*/
    EbAv1OperatingPoint   operating_point_prms[MAX_NUM_OPERATING_POINTS];

    /*!< Specifies the number of bits used for transmitting the frame
     * width syntax elements */
    uint8_t             frame_width_bits;

    /*!< Specifies the number of bits used for transmitting the frame
     * height syntax elements*/
    uint8_t             frame_height_bits;

    /*!< Specifies the maximum frame width minus 1 for the frames represented
     * by this sequence header */
    uint16_t            max_frame_width;

    /*!< Specifies the maximum frame height minus 1 for the frames represented
     * by this sequence header */
    uint16_t            max_frame_height;

    /*!< Specifies whether frame id numbers are present in the coded video
     * sequence*/
    uint8_t             frame_id_numbers_present_flag;

    /*!< Specifies the number of bits minus 2 used to encode delta_frame_id
     * syntax elements*/
    uint8_t             delta_frame_id_length_minus_2;

    /*!<Used to calculate the number of bits used to encode the frame_id syntax
     * element.*/
    uint8_t             additional_frame_id_length_minus_1;

    /*!< 1: Indicates that superblocks contain 128x128 luma samples
     *   0: Indicates that superblocks contain 64x64 luma samples.*/
    uint8_t             use_128x128_superblock;

    /*!< superblock size */
    BlockSize          sb_size;

    /*!< superblock size in 4x4 MI unit */
    uint8_t             sb_mi_size;

    /*!< superblock size inlog2 unit */
    uint8_t             sb_size_log2;

    /*!< 1: Specifies that the use_filter_intra syntax element may be present.
     *   0: Specifies that the use_filter_intra syntax element will not be
     *       present*/
    uint8_t             enable_filter_intra;

    /*!< Specifies whether the intra edge filtering process should be enabled */
    uint8_t             enable_intra_edge_filter;

    /*!<1: Specifies that the mode info for inter blocks may contain the syntax
     *     element interintra.
     *  0: Specifies that the syntax element interintra will not be present */
    uint8_t             enable_interintra_compound;

    /*!<1: Specifies that the mode info for inter blocks may contain the syntax
     *     element compound_type
     *  0: Specifies that the syntax element compound_type will not be present*/
    uint8_t             enable_masked_compound;

    /*!<1: Indicates that the allow_warped_motion syntax element may be present
     *  0: Indicates that the allow_warped_motion syntax element will not be
     *     present*/
    uint8_t             enable_warped_motion;

    /*!< 1: Indicates that the inter prediction filter type may be specified
     *      independently in the horizontal and vertical directions.
     *   0: Indicates only one filter type may be specified, which is then used
     *      in both directions.*/
    uint8_t             enable_dual_filter;

    /*!< Order Hint Information structure */
    OrderHintInfo     order_hint_info;

    /*!<Equal to SELECT_SCREEN_CONTENT_TOOLS, indicates that the
     * allow_screen_content_tools syntax element will be present in the frame
     * header. Otherwise, seq_force_screen_content_tools contains the value for
     * allow_screen_content_tools*/
    uint8_t             seq_force_screen_content_tools;

    /*!< Equal to SELECT_INTEGER_MV indicates that the force_integer_mv syntax
     * element will be present in the frame header (providing
     * allow_screen_content_tools is equal to 1). Otherwise, seq_force_integer_mv
     * contains the value for force_integer_mv */
    uint8_t             seq_force_integer_mv;

    /*!< 1: Specifies that the use_superres syntax element will be present in
     *      the uncompressed header.
     *   0: Specifies that the use_superres syntax element will not be present*/
    uint8_t             enable_superres;

    /*!< 1: Specifies that cdef filtering may be enabled.
         0: specifies that cdef filtering is disabled */
    uint8_t             enable_cdef;

    /*!< 1: Specifies that loop restoration filtering may be enabled.
         0: Specifies that loop restoration filtering is disabled*/
    uint8_t             enable_restoration;

    /*!< Colour Configuration structure*/
    EbColorConfig         color_config;

    /*!< Specifies whether film grain parameters are present in the coded video sequence*/
    uint8_t             film_grain_params_present;
} SeqHeader;

/*!@file
 * @brief frame header syntax structures.
 */

/* Maximum number of tile rows and tile columns
   Conflicting with wrong values entered in EbDefinitions */

#define MAX_TILE_ROWS_AV1 64
#define MAX_TILE_COLS_AV1 64

enum {
    SEG_LVL_ALT_Q,       // Use alternate Quantizer
    SEG_LVL_ALT_LF_Y_V,  // Use alternate loop filter value on y plane vertical
    SEG_LVL_ALT_LF_Y_H,  // Use alternate loop filter value on y plane horizontal
    SEG_LVL_ALT_LF_U,    // Use alternate loop filter value on u plane
    SEG_LVL_ALT_LF_V,    // Use alternate loop filter value on v plane
    SEG_LVL_REF_FRAME,   // Optional Segment reference frame
    SEG_LVL_SKIP,        // Optional Segment (0,0) + skip mode
    SEG_LVL_GLOBALMV,
    SEG_LVL_MAX
} UENUM1BYTE(SEG_LVL_FEATURES);

typedef struct FrameSize {
    /*!< Width of the frame in luma samples */
    uint16_t    frame_width;

    /*!< Height of the frame in luma samples */
    uint16_t    frame_height;

    /*!< Render width of the frame in luma samples */
    uint16_t    render_width;

    /*!< Render height of the frame in luma samples */
    uint16_t    render_height;

    /*!< Denominator of a fraction that specifies the ratio between the
     * superblock width before and after upscaling*/
    uint8_t     superres_denominator;

    /*!< Width of Upscaled SuperRes */
    uint16_t    superres_upscaled_width;

    /*!< Height of Upscaled SuperRes */
    uint16_t    superres_upscaled_height;
} FrameSize;

typedef struct TilesInfo_s {
    /*!< Specifies the maximum width that can be used for a tile */
    uint16_t    max_tile_width_sb;

    /*!< Specifies the maximum height that can be used for a tile */
    uint16_t    max_tile_height_sb;

    /*!< Specifies minimum of base 2 logarithm of tiles column across the frame */
    uint8_t     min_log2_tile_cols;

    /*!< Specifies maximum of base 2 logarithm of tiles column across the frame */
    uint8_t     max_log2_tile_cols;

    /*!< Specifies maximum of base 2 logarithm of tiles row down the frame */
    uint8_t     max_log2_tile_rows;

    /*!< Specifies minimum of base 2 logarithm of tiles row down the frame */
    uint8_t     min_log2_tile_rows;

    /*!< Specifies minimum of base 2 logarithm of tiles */
    uint8_t     min_log2_tiles;

    /*!< 1: Indicates that the tiles are uniformly spaced across the frame
     *   0: Indicates that the tile sizes are coded*/
    uint8_t     uniform_tile_spacing_flag;

    /*!< Specifies the number of tiles across the frame */
    uint8_t     tile_cols;

    /*!< Specifies the number of tiles down the frame */
    uint8_t     tile_rows;

    /*!< Specifies the base 2 logarithm of the desired number of tiles across the frame */
    uint8_t tile_cols_log2;

    /*!< Specifies the base 2 logarithm of the desired number of tiles down the frame */
    uint8_t tile_rows_log2;

    /*!< Specifying the start column (in units of 4x4 luma samples) for each tile
     * across the image */
    uint16_t    tile_col_start_sb[MAX_TILE_COLS_AV1 + 1];

    /*!< Specifying the start row (in units of 4x4 luma samples) for each tile down the image */
    uint16_t    tile_row_start_sb[MAX_TILE_ROWS_AV1 + 1];

    /*!< Specifies which tile to use for the CDF update */
    uint16_t    context_update_tile_id;

    /*!< Used to compute TileSizeBytes */
    uint8_t     tile_size_bytes;
} TilesInfo;

typedef struct QuantizationParams {
    /*!< Indicates the base frame qindex */
    uint8_t     base_q_idx;

    /*!< Indicates the Y DC quantizer relative to base_q_idx */
    int8_t     delta_q_y_dc;

    /*!< Indicates the U DC quantizer relative to base_q_idx */
    int8_t     delta_q_u_dc;

    /*!< Indicates the V DC quantizer relative to base_q_idx */
    int8_t     delta_q_v_dc;

    /*!< Indicates the U AC quantizer relative to base_q_idx */
    int8_t     delta_q_u_ac;

    /*!< Indicates the V AC quantizer relative to base_q_idx */
    int8_t     delta_q_v_ac;

    /*!<Specifies that the quantizer matrix will be used to compute quantizers*/
    uint8_t     using_qmatrix;

    /*!< Specifies the level in the quantizer matrix that should be used for
     * luma plane decoding */
    uint8_t     qm_y;

    /*!< specifies the level in the quantizer matrix that should be used for
     * chroma U plane decoding*/
    uint8_t     qm_u;

    /*!< Specifies the level in the quantizer matrix that should be used for
     * chroma V plane decoding*/
    uint8_t     qm_v;
} QuantizationParams;

typedef struct SegmentationParams {
    /*!< 1: Indicates that this frame makes use of the segmentation tool
     *   0: Indicates that the frame does not use segmentation*/
    uint8_t     segmentation_enabled;

    /*!< 1: Indicates that the segmentation map are updated during the decoding
     *      of this frame
     *   0: Indicates that the segmentation map from the previous frame is used*/
    uint8_t     segmentation_update_map;

    /*!< 1: Indicates that the updates to the segmentation map are coded
     *      relative to the existing segmentation map
     *   0: Indicates that the new segmentation map is coded without reference
     *      to the existing segmentation map */
    uint8_t     segmentation_temporal_update;

    /*!<1: Indicates that new parameters are about to be specified for each segment
     *  0: Indicates that the segmentation parameters should keep their existing
     *     values*/
    uint8_t     segmentation_update_data;

    /*!< Specifies the feature data for a segment feature */
    int16_t     feature_data[MAX_SEGMENTS][SEG_LVL_MAX];

    /*!< Specifies the feature enabled for a segment feature */
    int16_t     feature_enabled[MAX_SEGMENTS][SEG_LVL_MAX];

    /*!< Specifies the feature enabled for a segment feature */
    int16_t     seg_qm_level[MAX_SEGMENTS][SEG_LVL_MAX];

    /*!< Specifies the highest numbered segment id that has some enabled feature*/
    uint8_t     last_active_seg_id;

    /*!< 1: Indicates that the segment id will be read before the skip syntax element
     *   0: Indicates that the skip syntax element will be read first */
    uint8_t     seg_id_pre_skip;
} SegmentationParams;

typedef struct DeltaQParams {
    /*!< Specifies whether quantizer index delta values are present */
    uint8_t     delta_q_present;

    /*!< Specifies the left shift which should be applied to decoded quantizer
     * index delta values*/
    uint8_t     delta_q_res;
} DeltaQParams;

typedef struct DeltaLFParams {
    /*!< Specifies whether loop filter delta values are present */
    uint8_t     delta_lf_present;

    /*!< Specifies the left shift which should be applied to decoded loop filter
     * delta values*/
    uint8_t     delta_lf_res;

    /*!< 1: Specifies that separate loop filter deltas are sent for horizontal
     *      luma edges, vertical luma edges, the U edges, and the V edges
     *   0: Specifies that the same loop filter delta is used for all edges */
    uint8_t     delta_lf_multi;
} DeltaLFParams;

typedef struct LoopFilterParams {
    /*!< An array containing loop filter strength values
         SEG_LVL_ALT_LF_Y_V = 1; SEG_LVL_ALT_LF_Y_H = 2;
         SEG_LVL_ALT_LF_U   = 3; SEG_LVL_ALT_LF_V   = 4;*/
    uint8_t     loop_filter_level[FRAME_LF_COUNT];

    /*!< Indicates the sharpness level.*/
    uint8_t     loop_filter_sharpness;

    /*!< 1: Indicates that the filter level depends on the mode and reference
     *      frame used to predict a block
     *   0: Indicates that the filter level does not depend on the mode and
     *      reference frame*/
    uint8_t     loop_filter_delta_enabled;

    /*!< 1: Indicates that additional syntax elements are present that specify
     *      which mode and reference frame deltas are to be updated
     *   0: Indicates that these syntax elements are not present*/
    uint8_t     loop_filter_delta_update;

    /*!< Contains the adjustment needed for the filter level based on the
     *   chosen reference frame*/
    int8_t      loop_filter_ref_deltas[REF_FRAMES];

    /*!< 1: Indicates that the syntax element loop_filter_mode_deltas is present
     *   0: Indicates that this syntax element is not present */
    int8_t      loop_filter_mode_deltas[REF_FRAMES];
} LoopFilterParams;

typedef struct CDEFParams {
    /*!< Controls the amount of damping in the deringing filter */
    uint8_t     cdef_damping;

    /*!< Specifies the number of bits needed to specify which CDEF filter to
     *     apply*/
    uint8_t     cdef_bits;

    /*!< Specify the strength of the primary and secondary filter of Y plane */
    uint8_t     cdef_y_strength[CDEF_MAX_STRENGTHS];

    /*!< Specify the strength of the primary and secondary filter of UV plane */
    uint8_t     cdef_uv_strength[CDEF_MAX_STRENGTHS];
} CDEFParams;

typedef struct LRParams {
    /*!< Specifies the type of restoration used for each plane */
    RestorationType     frame_restoration_type;

    /*!< Specifies the size of loop restoration units in units of samples in
     * the current plane */
    uint8_t             loop_restoration_size;
} LRParams;

typedef struct SkipModeParams {
    /*!< Flag to check skip mode allowed or not */
    uint8_t     skip_mode_allowed;

    /*!< 1: Specifies that the syntax element skip_mode will be present
     *   0: Specifies that skip_mode will not be used for this frame */
    uint8_t     skip_mode_present;

    /*!< ref_frame_idx_0 & ref_frame_idx_1 */
    uint8_t     skip_mode_frame[2];
} SkipModeParams;

typedef struct GlobalMotionParams {
    /*!< Specifies the transform type */
    TransformationType  gm_type[ALTREF_FRAME - LAST_FRAME + 1];

    /*!< Global motion parameter */
    int32_t             gm_params[ALTREF_FRAME - LAST_FRAME + 1][6];
} GlobalMotionParams;

typedef struct FilmGrainParams_s{
    /*!< 1: Specifies that film grain should be added to this frame
     *   0: Specifies that film grain should not be added */
    uint8_t     apply_grain;

    /*!< Specifies the starting value for the pseudo-random numbers used during
     * film grain synthesis*/
    uint16_t    grain_seed;

    /*!< 1: Indicates that a new set of parameters should be sent
     *   0: Indicates  that the previous set of parameters should be used*/
    uint8_t     update_grain;

    /*!< Specifies the number of points for the piece-wise linear scaling
     * function of the luma component */
    uint8_t     num_y_points;

    /*!< Represents the x (luma value) coordinate for the index point of the
     * piecewise linear scaling function for luma component */
    uint8_t     point_y_value[14];

    /*!< Represents the scaling (output) value for the index point of the
     * piecewise linear scaling function for luma component */
    uint8_t     point_y_scaling[14];

    /*!< Specifies that the chroma scaling is inferred from the luma scaling */
    uint8_t     chroma_scaling_from_luma;

    /*!< Specifies the number of points for the piece-wise linear scaling
     * function of the cb component */
    uint8_t     num_cb_points;

    /*!< Represents the x coordinate for the i-th point of the piece-wise linear
     * scaling function for cb component */
    uint8_t     point_cb_value[10];

    /*!< Represents the scaling (output) value for the index point of the
     * piecewise linear scaling function for cb component */
    uint8_t     point_cb_scaling[10];

    /*!< Specifies represents the number of points for the piece-wise linear
     * scaling function of the cr component */
    uint8_t     num_cr_points;

    /*!< Represents the x coordinate for the i-th point of the piece-wise
     * linear scaling function for cr component */
    uint8_t     point_cr_value[14];

    /*!< Represents the scaling (output) value for the i-th point of the
     * piecewise linear scaling function for cr component */
    uint8_t     point_cr_scaling[14];

    /*!<sSpecifies the number of auto-regressive coefficients for luma and chroma*/
    uint8_t     ar_coeff_lag;

    /*!< Specifies auto-regressive coefficients used for the Y plane */
    uint8_t     ar_coeffs_y[24];

    /*!< Specifies auto-regressive coefficients used for the U plane */
    uint8_t     ar_coeffs_cb[24];

    /*!< Specifies auto-regressive coefficients used for the V plane */
    uint8_t     ar_coeffs_cr[24];

    /*!< Specifies the range of the auto-regressive coefficients */
    uint8_t     ar_coeff_shift;

    /*!< Specifies how much the Gaussian random numbers should be scaled down
     * during the grain synthesis process*/
    uint8_t     grain_scale_shift;

    /*!< Represents a multiplier for the cb component used in derivation of the
     * input index to the cb component scaling function*/
    uint8_t     cb_mult;

    /*!< Represents a multiplier for the average luma component used in
     * derivation of the input index to the cb component scaling function */
    uint8_t     cb_luma_mult;

    /*!< Represents an offset used in derivation of the input index to the cb
     * component scaling function */
    uint16_t    cb_offset;

    /*!< Represents a multiplier for the cr component used in derivation of the
     * input index to the cr component scaling function */
    uint8_t     cr_mult;

    /*!< Represents a multiplier for the average luma component used in
     * derivation of the input index to the cr component scaling function*/
    uint8_t     cr_luma_mult;

    /*!< Represents an offset used in derivation of the input index to the cr
     * component scaling function */
    uint16_t    cr_offset;

    /*!< 1: Indicates that the overlap between film grain blocks shall be applied
     *   0: Indicates that the overlap between film grain blocks shall not be
     *      applied*/
    uint8_t     overlap_flag;

    /*!< 1: Indicates that clipping to the restricted (studio) range shall be
            applied to the sample values after adding the film grain
     *   0: Indicates that clipping to the full range shall be applied to the
            sample values after adding the film grain */
    uint8_t     clip_to_restricted_range;
} FilmGrainParams;

typedef struct FrameHeader {
    /*!< 1: Indicates the frame indexed by frame_to_show_map_idx is to be output.
         0: Indicates that further processing is required */
    uint8_t     show_existing_frame;

    /*!< Specifies the type of the frame */
    FrameType  frame_type;

    /*!< 1: Specifies that this frame should be immediately output once decoded
         0: Specifies that this frame should not be immediately output */
    uint8_t     show_frame;

    /*!< Specifies the presentation time of the frame in clock ticks DispCT
     * counted from the removal time of the last random access point for the
     * operating point that is being decoded */
    uint32_t    frame_presentation_time;

    /*!< 1: Specifies that the frame may be output using the show_existing_frame
     *      mechanism
     *   0: Specifies that this frame will not be output using the
            show_existing_frame mechanism */
    uint8_t     showable_frame;

    /*!< 1: Indicates that error resilient mode is enabled
     *   0: Indicates that error resilient mode is disabled */
    uint8_t     error_resilient_mode;

    /*!< Specifies whether the CDF update in the symbol decoding process should
     * be disabled */
    uint8_t     disable_cdf_update;

    /*!< 1: Indicates that intra blocks may use palette encoding
     *   0: Indicates that palette encoding is never used */
    uint8_t     allow_screen_content_tools;

    /*!< 1: Specifies that motion vectors will always be integers
     *   0: Specifies that motion vectors can contain fractional bits */
    uint8_t     force_integer_mv;

    /*!< Specifies the frame id number for the current frame */
    uint32_t    current_frame_id;

    /*!< Used to compute OrderHint */
    uint32_t    order_hint;

    /*!< Specifies which reference frame contains the CDF values and other
     * state that should be loaded at the start of the frame */
    uint8_t     primary_ref_frame;

    /*!< 1: Specifies that buffer_removal_time is present.
         0: Specifies that buffer_removal_time is not present */
    uint8_t     buffer_removal_time_present_flag;

    /*!< Specifies the frame removal time in units of DecCT clock ticks counted
     * from the removal time of the last random access point for operating
     * point opNum */
    uint32_t    buffer_removal_time[MAX_NUM_OPERATING_POINTS];

    /*!< Specifies the length of the buffer_removal_time syntax element */
    uint8_t     refresh_frame_flags;

    /*!< Specifies the expected output order hint for each reference frame */
    uint32_t    ref_order_hint[REF_FRAMES];

    /*!< Specifies the expected output order for each reference frame */
    uint32_t    order_hints[REF_FRAMES];

    /*!< 1: Indicates that intra block copy may be used in this frame.
     *   0: Indicates that intra block copy is not allowed in this frame */
    uint8_t     allow_intrabc;

    /*!< Specifies which reference frames are used by inter frames */
    uint8_t         ref_frame_idx[REF_FRAMES];

    /*!< An array which is indexed by a reference picture slot number
     *  1: Signifies that the corresponding reference picture slot is valid for
     *     use as a reference picture
     *  0: Signifies that the corresponding reference picture slot is not valid
     *     for use as a reference picture*/
    uint32_t ref_valid[REF_FRAMES];

    /*!< Frame Size structure */
    FrameSize       frame_size;

    /*!< 0: Specifies that motion vectors are specified to quarter pel precision
     *   1: Specifies that motion vectors are specified to eighth pel precision*/
    uint8_t         allow_high_precision_mv;

    /*!< Specifies the filter selection used for performing inter prediction */
    InterpFilter    interpolation_filter;

    /*!< 0: Specifies that only the SIMPLE motion mode will be used */
    uint8_t     is_motion_mode_switchable;

    /*!< 1: Specifies that motion vector information from a previous frame can
     * be used when decoding the current frame
     *   0: Specifies that this information will not be used */
    uint8_t     use_ref_frame_mvs;

    /*!< 1: Indicates that the end of frame CDF update is disabled
     *   0: Indicates that the end of frame CDF update is enabled */
    uint32_t    ref_frame_sign_bias[REF_FRAMES];

    /*!< 1: Indicates that the end of frame CDF update is disabled
     *   0: Indicates that the end of frame CDF update is enabled */
    uint8_t     disable_frame_end_update_cdf;

    /* Number of 4x4 block columns in the frame */
    uint32_t mi_cols;

    /* Number of 4x4 block rows in the frame */
    uint32_t mi_rows;

    /*!< Tile information */
    TilesInfo    tiles_info;

    /*!< Quantization Parameters */
    QuantizationParams      quantization_params;

    /*!< Segmentation Parameters */
    SegmentationParams      segmentation_params;

    /*!< Delta Quantization Parameters */
    DeltaQParams            delta_q_params;

    /*!< Delta Loop Filter Parameters */
    DeltaLFParams           delta_lf_params;

    /*!< Indicates that the frame is fully lossless at the coded resolution of
     * FrameWidth by FrameHeight */
    uint8_t                 coded_lossless;

    /*!< Indicates that the frame is fully lossless at the upscaled resolution*/
    uint8_t                 all_lossless;

    /*!< Indicates the flag to set coded_lossless variable */
    uint8_t                 lossless_array[MAX_SEGMENTS];

    /*!< Loop Filter Parameters */
    LoopFilterParams        loop_filter_params;

    /*!< Constrained Directional Enhancement Filter */
    CDEFParams              CDEF_params;

    /*!< Loop Restoration Parameters */
    LRParams                LR_params[MAX_MB_PLANE];

    /*!< Specifies how the transform size is determined */
    TxMode                 tx_mode;

    /*!< Reference Mode structure */
    ReferenceMode           reference_mode;

    /*!< Skip Mode Parameters*/
    SkipModeParams          skip_mode_params;

    /*!< 1: Indicates that the syntax element motion_mode may be present
     *   0: Indicates that the syntax element motion_mode will not be present*/
    uint8_t                 allow_warped_motion;

    /*!< 1, specifies that the frame is restricted to a reduced subset of the
     * full set of transform types */
    uint8_t                 reduced_tx_set;

    /*!< Global Motion Paramters */
    GlobalMotionParams      global_motion_params;

    /*!< Film Grain Parameters */
    FilmGrainParams         film_grain_params;

    /* Dequantization context */
    Dequants                dequants;

    /* This need to be moved to thread context */
    Dequants                *dequants_delta_q;

    /* Inverse Quantization Matrix */
    const QmVal          *giqmatrix[NUM_QM_LEVELS][3][TX_SIZES_ALL];
} FrameHeader;

#ifdef __cplusplus
    }
#endif
#endif // EbDecStruct_h
