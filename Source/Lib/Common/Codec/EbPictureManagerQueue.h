/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbPictureManagerQueue_h
#define EbPictureManagerQueue_h

#include "EbDefinitions.h"
#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"
#include "EbPredictionStructure.h"
#include "EbApiSei.h"

#ifdef __cplusplus
extern "C" {
#endif
    /************************************************
     * Input Queue Entry
     ************************************************/
    struct ReferenceQueueEntry;   // empty struct definition

    typedef struct InputQueueEntry 
    {
        EbObjectWrapper *input_object_ptr;
        uint32_t         dependent_count;
        uint32_t         reference_entry_index;
        ReferenceList   *list0_ptr;
        ReferenceList   *list1_ptr;
    } InputQueueEntry;

    /************************************************
     * Reference Queue Entry
     ************************************************/
    typedef struct ReferenceQueueEntry 
    {
        uint64_t         picture_number;
        uint64_t         decode_order;
        EbObjectWrapper *reference_object_ptr;
        uint32_t         dependent_count;
        EbBool           release_enable;
        EbBool           reference_available;
        uint32_t         dep_list0_count;
        uint32_t         dep_list1_count;
        DependentList    list0;
        DependentList    list1;
        EbBool           is_used_as_reference_flag;
        EB_SLICE         slice_type;
        uint8_t          temporal_layer_index;
        uint64_t         last_islice_picture_number;         
#if RC_FEEDBACK         
        EbBool           feedback_arrived;
#endif 
    } ReferenceQueueEntry;

    /************************************************
     * Rate Control Input Queue Entry
     ************************************************/

    typedef struct RcInputQueueEntry 
    {
        uint64_t         picture_number;
        EbObjectWrapper *input_object_ptr;
        EbBool           release_enabled;
        uint64_t         group_id;
        uint64_t         gop_first_poc;
        uint32_t         gop_index;
    } RcInputQueueEntry;


    /************************************************
     * Rate Control FeedBack  Queue Entry
     ************************************************/
    typedef struct RcFeedbackQueueEntry 
    {
        uint64_t  picture_number;
        EbBool    release_enabled;
        uint64_t  group_id;
        uint64_t  gop_first_poc;
        uint32_t  gop_index;
    } RcFeedbackQueueEntry;


    extern EbErrorType input_queue_entry_ctor(
        InputQueueEntry **entry_dbl_ptr);

    extern EbErrorType reference_queue_entry_ctor(
        ReferenceQueueEntry  **entry_dbl_ptr);

#ifdef __cplusplus
}
#endif
#endif // EbPictureManagerQueue_h