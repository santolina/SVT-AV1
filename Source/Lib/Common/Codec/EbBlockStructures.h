/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbBlockStructures_h
#define EbBlockStructures_h

#include "EbDefinitions.h"
#include "EbSegmentationParams.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MV {
    int16_t row;
    int16_t col;
} MV;

typedef union IntMv {
    uint32_t as_int;
    MV       as_mv;
} IntMv; /* facilitates faster equality tests and copies */

typedef struct mv32 {
    int32_t row;
    int32_t col;
} MV32;

typedef struct CandidateMv {
    IntMv   this_mv;
    IntMv   comp_mv;
    int32_t weight;
} CandidateMv;

typedef struct TileInfo {
    int32_t mi_row_start, mi_row_end;
    int32_t mi_col_start, mi_col_end;
    int32_t tg_horz_boundary;
    int32_t tile_row;
    int32_t tile_col;
} TileInfo;

#define INTER_TX_SIZE_BUF_LEN 16
#define TXK_TYPE_BUF_LEN 64

typedef struct FilterIntraModeInfo {
    /*!< Specifies the type of intra filtering, and can represent any of the following:
         * FILTER_DC_PRED, FILTER_V_PRED, FILTER_H_PRED, FILTER_D157_PRED, FILTER_PAETH_PRED */
    FilterIntraMode filter_intra_mode;

    /*!< This bit specifies whether or not intra filtering can be used. */
    uint8_t use_filter_intra;
} FilterIntraModeInfo_t;

typedef struct InterIntraModeParams {
    /*!< Specifies the type of intra prediction to be used */
    InterIntraMode interintra_mode;

    /*!< equal to 1 specifies that wedge blending should be used.
            * wedge_interintra equal to 0 specifies that intra blending should be used. */
    uint8_t wedge_interintra;

    /*!< Used to derive the direction and offset of the wedge mask used during blending. */
    uint8_t interintra_wedge_index;

    /*!< Specifies the sign of the wedge blend. */
    // int interintra_wedge_sign; Always 0
} InterIntraModeParams;


typedef struct BlockModeInfo {
    // Common for both INTER and INTRA blocks
    BlockSize      sb_type;
    PredictionMode mode;
    int8_t         skip;

    PartitionType partition;

    /*!< 1 indicates that this block will use some default settings and skip mode info.
            * 0 indicates that the mode info is not skipped. */
    int8_t skip_mode;

    /*!< Specifies which segment is associated with the current intra block being decoded. */
    int8_t segment_id;

    /*!< Equal to 1 specifies that the segment_id is taken from the segmentation map. */
    int8_t seg_id_predicted;

    /*!< For Lossy mode   : Specifies number of TUs in a block for each plane
             For Lossless mode: Specifies number of TUs for a block of size other than
                                128x128, 128x64, 64x128 and 64x64 - computed based on blocksize */
    uint8_t num_tus[MAX_MB_PLANE - 1];

    /*!< Offset of first transform info from strat of SB pointer for each plane */
    uint16_t first_txb_offset[MAX_MB_PLANE - 1];

    // Only for INTRA blocks
    UvPredictionMode uv_mode;

    uint8_t use_intrabc;

    // Only for INTER blocks

    MvReferenceFrame ref_frame[2];
    IntMv            mv[2];

    uint16_t ref_mv_idx;

    // interinter members

    InterIntraModeParams interintra_mode_params;

    /*!< Specifies the type of motion compensation to perform. */
    MotionMode motion_mode;

    InterIntraMode is_inter_intra;

    /*!< 0 indicates that a distance based weighted scheme should be used for blending.
         *   1 indicates that the averaging scheme should be used for blending.*/
    uint8_t compound_idx;

    InterInterCompoundData inter_inter_compound;
    FilterIntraModeInfo_t  filter_intra_mode_info;

    /*!< Specifies how the motion vector used by inter prediction is obtained when using compound prediction. */
    uint8_t compound_mode;

    /*!< Specifies the type of filter used in inter prediction. Values 0..3 are allowed
        * with the same interpretation as for interpolation_filter. One filter type is specified
        * for the vertical filter direction and one for the horizontal filter direction.*/
    uint32_t interp_filters;

    /*!< Index of the alpha Cb and alpha Cr combination */
    uint8_t cfl_alpha_idx;

    /*!< Contains the sign of the alpha values for U and V packed together into a single syntax element. */
    uint8_t cfl_alpha_signs;

    /*!< The actual prediction angle is the base angle + (angle_delta * step). */
    int8_t angle_delta[PLANE_TYPES];

    // Number of base colors for Y (0) and UV (1)
    uint8_t palette_size[MAX_MB_PLANE - 1];

#if MODE_INFO_DBG
    int32_t mi_row;
    int32_t mi_col;
#endif
} BlockModeInfo;

#ifdef __cplusplus
}
#endif
#endif // EbBlockStructures_h
