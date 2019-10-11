/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbEncDecProcess_h
#define EbEncDecProcess_h

#include "EbDefinitions.h"
#include "EbSyntaxElements.h"
#include "EbModeDecisionProcess.h"
#include "EbSystemResourceManager.h"
#include "EbPictureBufferDesc.h"
#include "EbModeDecision.h"
#include "EbInterPrediction.h"
#include "EbEntropyCoding.h"
#include "EbTransQuantBuffers.h"
#include "EbReferenceObject.h"
#include "EbNeighborArrays.h"
#include "EbCodingUnit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_STRIDE   4

    typedef struct EbPmCand
    {
        int16_t        tr_coeff[4 * 4];
        int16_t        qu_coeff[4 * 4];
        int16_t        iq_coeff[4 * 4];
        uint8_t        masking_level;
        uint64_t       cost;
        uint32_t       nz_coeff;
    } EbPmCand;

    /**************************************
     * Enc Dec Context
     **************************************/
    typedef struct EncDecContext
    {
        EbFifo                              *mode_decision_input_fifo_ptr;
        EbFifo                              *enc_dec_output_fifo_ptr;
        EbFifo                              *enc_dec_feedback_fifo_ptr;
        EbFifo                              *picture_demux_output_fifo_ptr;   // to picture-manager
        int16_t                             *transform_inner_array_ptr;
        MdRateEstimationContext             *md_rate_estimation_ptr;
        ModeDecisionContext                 *md_context;
        const BlockGeom                     *blk_geom;

        // TMVP
        EbReferenceObject                   *reference_object_write_ptr;

        // MCP Context
        MotionCompensationPredictionContext *mcp_context;
        SsMeContext                         *ss_mecontext;

        // Intra Reference Samples
        IntraReferenceSamples               *intra_ref_ptr;
        IntraReference16bitSamples          *intra_ref_ptr16;  //We need a different buffer for ENC pass then the MD one.

        // Coding Unit Workspace---------------------------
        EbPictureBufferDesc                 *residual_buffer;
        EbPictureBufferDesc                 *transform_buffer;
        EbPictureBufferDesc                 *input_samples;
        EbPictureBufferDesc                 *input_sample16bit_buffer;
        // temporary buffers for decision making of LF (LPF_PICK_FROM_FULL_IMAGE).
        // Since recon switches between reconPtr and referencePtr, the temporary buffers sizes used the referencePtr's which has padding,...
        EbPictureBufferDesc                 *inverse_quant_buffer;
        // Lambda
#if ADD_DELTA_QP_SUPPORT
        uint16_t                               qp;
#else
        uint8_t                                qp;
#endif
        uint16_t                                chroma_qp;
        uint32_t                               fast_lambda;
        uint32_t                               full_lambda;
        uint32_t                               fast_chroma_lambda;
        uint32_t                               full_chroma_lambda;
        uint32_t                               full_chroma_lambda_sao;

        //  Context Variables---------------------------------
        CodingUnit                          *cu_ptr;
        const CodedUnitStats                *cu_stats;
        uint16_t                               cu_origin_x; // within the picture
        uint16_t                               cu_origin_y; // within the picture
        uint8_t                                sb_sz;
        uint32_t                               sb_index;
        MvUnit                               mv_unit;
        int16_t                                x_mv_amvp_candidate_array_list0[MAX_NUM_OF_AMVP_CANDIDATES];
        uint8_t                                txb_itr;
        EbBool                                 is16bit; //enable 10 bit encode in CL
        EbColorFormat                          color_format;
        uint64_t                               tot_intra_coded_area;
        uint8_t                                intra_coded_area_sb[MAX_NUMBER_OF_TREEBLOCKS_PER_PICTURE];//percentage of intra coded area 0-100%
#if TWO_PASS_PART
        int8_t                                 first_pass_split_flag[MAX_NUMBER_OF_TREEBLOCKS_PER_PICTURE][BLOCK_MAX_COUNT_SB_128];
#endif
        uint8_t                                pmp_masking_level_enc_dec;
        EbBool                                 skip_qpm_flag;
        int16_t                                min_delta_qp_weight;
        int16_t                                max_delta_qp_weight;
        int8_t                                 min_delta_qp[4];
        int8_t                                 max_delta_qp[4];
        int8_t                                 non_moving_delta_qp;
        EbBool                                 grass_enhancement_flag;
        EbBool                                 backgorund_enhancement;
#if ADD_DELTA_QP_SUPPORT
        uint16_t                               qpm_qp;
#else
        uint8_t                                qpm_qp;
#endif
#if !PF_N2_SUPPORT
        EB_TRANS_COEFF_SHAPE                   trans_coeff_shape_luma;
        EB_TRANS_COEFF_SHAPE                   trans_coeff_shape_chroma;
#endif
        EbPmCand                             pm_cand_buffer[5];
        uint16_t                               qp_index;
        uint64_t                               three_quad_energy;

        // Needed for DC prediction
        EbBool                                 is_left_availble;
        EbBool                                 is_above_availble;
        uint8_t                                upsample_left;
        uint8_t                                upsample_above;
        uint8_t                                upsample_left_chroma;
        uint8_t                                upsample_above_chroma;

        uint16_t                               coded_area_sb;
        uint16_t                               coded_area_sb_uv;

        uint8_t                                is_inter;
        uint8_t                                reduced_tx_set_used;
        EbBool                                 evaluate_cfl_ep; // 0: CFL is evaluated @ mode decision, 1: CFL is evaluated @ encode pass
#if  BLK_SKIP_DECISION
        uint8_t                                md_skip_blk;
#endif
#if MOVE_TX_LEVELS_SIGNAL_UNDER_CTX
        uint8_t tx_search_level;
        uint64_t tx_weight;
        uint8_t tx_search_reduced_set;
        uint8_t skip_tx_search;
#endif
#if MOVE_ATB_MODE_SIGNAL_UNDER_CTX
#if ATB_SUPPORT
        uint8_t                                atb_mode;
#endif
#endif
    } EncDecContext;

    /**************************************
     * Extern Function Declarations
     **************************************/
    extern EbErrorType enc_dec_context_ctor(
        EncDecContext        **context_dbl_ptr,
        EbFifo                *mode_decision_configuration_input_fifo_ptr,
        EbFifo                *packetization_output_fifo_ptr,
        EbFifo                *feedback_fifo_ptr,
        EbFifo                *picture_demux_fifo_ptr,
        EbBool                   is16bit,
        EbColorFormat            color_format,
        uint32_t                 max_input_luma_width,
        uint32_t                 max_input_luma_height);

    extern void* enc_dec_kernel(void *input_ptr);

#ifdef __cplusplus
}
#endif
#endif // EbEncDecProcess_h
