/*!<
 * Copyright(c) 2019 Netflix, Inc.
 * SPDX - License - Identifier: BSD - 2 - Clause - Patent */

#ifndef EbDecProcess_h
#define EbDecProcess_h

#ifdef __cplusplus
extern "C" {
#endif
#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"

/*!< Node structure used in Decoder Queues. Can be used for tile/row idx */
typedef struct DecMtNode {
    EbDctor dctor;

    uint32_t node_index;

} DecMtNode;

typedef struct DecMtMotionProjInfo {
    /*!< Array to store MotionFieldProjection rows picked up for processing in Frame.
     *   This will be used for deciding row for motion_field_projections_row. */
    //uint32_t    *row_motion_proj_started;

    /*!< Number of rows in Frame : 64x64 level */
    int32_t num_motion_proj_rows;

    /*!< mutex handle for row assignment */
    EbHandle motion_proj_mutex;

    /*!< Motion Projection row state context */
    int32_t motion_proj_row_to_process;

} DecMtMotionProjInfo;

/*!< Stores the Queue & other related info needed between
 *   Parse and Recon for a Tile */
typedef struct DecMtParseReconTileInfo {
    /*!< Tile info for the current Tile */
    TileInfo tile_info;

    /*!< System Resource Managers at Tile Row level */
    //EbSystemResource    *recon_tile_sbrow_resource_ptr;

    /* EbFifo at SB row level : Recon Stage */
    //EbFifo      **recon_tile_sbrow_producer_fifo_ptr;
    //EbFifo      **recon_tile_sbrow_consumer_fifo_ptr;

    /*!< Array to store SB Recon rows completed parsing in the Tile. This will be
       used for sb deocde row start processing. */
    uint32_t *sb_recon_row_parsed;

    /*!< Array to store SB Recon rows picked up for processing in the Tile. This will be
       used for deciding which tile to be picked up for processing. */
    uint32_t *sb_recon_row_started;

    /*!< Array to store SBs completed in every SB row of Recon stage.
       Used for top-right sync */
    uint32_t *sb_recon_completed_in_row;

    /*!< Number of SB rows in tile */
    int32_t tile_num_sb_rows;

    /*!< mutex handle for sb row assignment */
    EbHandle tile_sbrow_mutex;

    /*!< SB row state context */
    int32_t sb_row_to_process;

} DecMtParseReconTileInfo;

typedef struct DecMtlfFrameInfo {
    /*!< System Resource Managers */
    EbSystemResource *lf_resource_ptr;
    /*!< EbFifo at Frame Row level */
    EbFifo *lf_row_producer_fifo_ptr;
    EbFifo *lf_row_consumer_fifo_ptr;

    /*!< Array to store SBs completed in every SB row of LF stage.
     *   Used for top sync */
    int32_t *sb_lf_completed_in_row;

} DecMtlfFrameInfo;

/*!< Previous frame's properties to check for MT support */
typedef struct PrevFrameMtCheck {
    /*!< Flag to indicate if the first
     *   frame header has been read */
    EbBool frame_header_read;

    /*!< Previous frame's height */
    uint16_t prev_max_frame_height;

    /*!< Previous frame's width */
    uint16_t prev_max_frame_width;

    /*!< Previous frame's SB size */
    BlockSize prev_sb_size;

    /*!< Previous frame's tile information */
    TilesInfo prev_tiles_info;
} PrevFrameMtCheck;

/*!< MT State information for each frame in parallel */
typedef struct DecMtFrameData {
    uint32_t num_threads_cdefed;

    uint32_t num_threads_exited;
    EbBool   end_flag;
    EbBool   start_motion_proj;
    EbBool   start_parse_frame;
    EbBool   start_decode_frame;
    EbBool   start_lf_frame;
    EbBool   start_cdef_frame;
    EbHandle temp_mutex;

    TilesInfo *tiles_info;

    /*!< Motion Field Projection Info*/
    DecMtMotionProjInfo motion_proj_info;
    uint32_t            num_threads_header; /*!< ToDo : should remove */

    /*!< System Resource Managers */
    EbSystemResource *parse_tile_resource_ptr;
    /*!< EbFifo at Tile level : Parse Stage */
    EbFifo *parse_tile_producer_fifo_ptr;
    EbFifo *parse_tile_consumer_fifo_ptr;

    /*!< System Resource Managers */
    EbSystemResource *recon_tile_resource_ptr;

    /*!< EbFifo at Tile level : Recon Stage */
    EbFifo *recon_tile_producer_fifo_ptr;
    EbFifo *recon_tile_consumer_fifo_ptr;

    /*!< To prevent more than 1 thread from mod. recon_row_started simult. */
    EbHandle recon_mutex;
    /*!< Parse-Recon Stage structure */
    DecMtParseReconTileInfo *parse_recon_tile_info_array;
    EbHandle                 tile_switch_mutex;

    /*!< Bhavna: Comment */
    uint32_t *sb_recon_row_map;

    /*!< Bhavna: Comment */
    uint32_t *lf_row_map;

    /*!< LF Stage structure */
    DecMtlfFrameInfo lf_frame_info;

    /*!< EbFifo at Frame Row level : CDEF Stage */
    /*!< System Resource Managers */
    EbSystemResource *cdef_resource_ptr;
    /*!< EbFifo at Frame Row level */
    EbFifo *cdef_row_producer_fifo_ptr;
    EbFifo *cdef_row_consumer_fifo_ptr;
    /*!< Array to store 64x64s completed in every 64x64 row of CDEF stage.
     *   Used for top-right sync */
    uint32_t *cdef_completed_in_row;
    /*!< line buffers for every 64x64 row, to store lf o/p */
    uint16_t ***cdef_linebuf;
    int32_t     cdef_linebuf_stride;
    /*!< cdef map for every 64x64row+1 to track whether cdef is performed or not */
    uint8_t *row_cdef_map;
    /*!< Siva: */
    uint32_t cdef_map_stride;
    EbFifo * cdef_fifo_ptr;
    /*!< EbFifo at Frame Row level : SR Stage */
    EbFifo *sr_fifo_ptr;
    /*!< EbFifo at Frame Row level : LR Stage */
    EbFifo *lr_fifo_ptr;
    /*!< EbFifo at Frame Row level : Pad Stage */
    EbFifo *pad_fifo_ptr;

    PrevFrameMtCheck prev_frame_info;

    int32_t sb_cols;
    int32_t sb_rows;

} DecMtFrameData;

#ifdef __cplusplus
}
#endif

#endif /*!< EbDecProcess_h */
