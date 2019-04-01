/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbEncDecResults_h
#define EbEncDecResults_h

#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"
#ifdef __cplusplus
extern "C" {
#endif
    /**************************************
     * Process Results
     **************************************/
    typedef struct EncDecResults_s
    {
        EbObjectWrapper      *picture_control_set_wrapper_ptr;
        uint32_t                  completedLcuRowIndexStart;
        uint32_t                  completedLcuRowCount;

    } EncDecResults_t;

    typedef struct DlfResults_s
    {
        EbObjectWrapper      *picture_control_set_wrapper_ptr;
      
        uint32_t          segment_index;


    } DlfResults_t;
    typedef struct CdefResults_s
    {
        EbObjectWrapper      *picture_control_set_wrapper_ptr;
       
        uint32_t          segment_index;

    } CdefResults_t;
    typedef struct RestResults_s
    {
        EbObjectWrapper      *picture_control_set_wrapper_ptr;
        uint32_t                  completed_lcu_row_index_start;
        uint32_t                  completed_lcu_row_count;

    } RestResults_t;

    typedef struct EncDecResultsInitData_s
    {
        uint32_t         junk;
    } EncDecResultsInitData_t;

    /**************************************
     * Extern Function Declarations
     **************************************/
    extern EbErrorType EncDecResultsCtor(
        EbPtr *object_dbl_ptr,
        EbPtr object_init_data_ptr);


#ifdef __cplusplus
}
#endif
#endif // EbEncDecResults_h