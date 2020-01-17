/*!< Copyright(c) 2019 Netflix, Inc.
 * SPDX - License - Identifier: BSD - 2 - Clause - Patent */

/*!< SUMMARY: Contains the Decode MT process related functions */

/**************************************/
/*!< Includes */
/**************************************/

#include "EbDefinitions.h"
#include "EbPictureBufferDesc.h"

#include "EbSvtAv1Dec.h"
#include "EbDecHandle.h"
#include "EbDecMemInit.h"

#include "EbObuParse.h"
#include "EbDecParseFrame.h"
#include "EbDecProcessFrame.h"
#include "EbDecLF.h"
#include "EbDecCdef.h"

#include "EbDecBitstream.h"
#include "EbTime.h"

#include "EbDecInverseQuantize.h"
#include "EbLog.h"

#include <stdlib.h>

void *dec_all_stage_kernel(void *input_ptr);
/*!< ToDo : Remove all these replications */
void eb_av1_loop_filter_frame_init(FrameHeader *frm_hdr, LoopFilterInfoN *lfi, int32_t plane_start,
                                   int32_t plane_end);
void dec_loop_filter_row(EbDecHandle *dec_handle_ptr, EbPictureBufferDesc *recon_picture_buf,
                         LfCtxt *lf_ctxt, LoopFilterInfoN *lf_info, uint32_t y_sb_index,
                         int32_t plane_start, int32_t plane_end);
void save_deblock_boundary_lines(uint8_t *src_buf, int32_t src_stride, int32_t src_width,
                                 int32_t src_height, const Av1Common *cm, int32_t plane,
                                 int32_t row, int32_t stripe, int32_t use_highbd, int32_t is_above,
                                 RestorationStripeBoundaries *boundaries);

EbErrorType dec_dummy_ctor(DecMtNode *context_ptr, EbPtr object_init_data_ptr) {
    context_ptr->node_index = *(uint32_t *)object_init_data_ptr;

    return EB_ErrorNone;
}

EbErrorType dec_dummy_creator(EbPtr *object_dbl_ptr, EbPtr object_init_data_ptr) {
    DecMtNode *obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, dec_dummy_ctor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}

/**************************************/
/*!< System Resource Managers & Fifos */
/**************************************/
EbErrorType dec_system_resource_init(EbDecHandle *dec_handle_ptr, TilesInfo *tiles_info) {
    EbErrorType     return_error = EB_ErrorNone;
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;

    int32_t num_tiles = tiles_info->tile_cols * tiles_info->tile_rows;

    assert(dec_handle_ptr->dec_config.threads > 1);

    /**************************************/
    /*!< System Resource Managers & Fifos */
    /**************************************/
    dec_handle_ptr->start_thread_process = EB_FALSE;

    /*!< Motion Filed Projection */
    dec_mt_frame_data->motion_proj_info.num_motion_proj_rows = -1;
    EB_CREATE_MUTEX(dec_mt_frame_data->motion_proj_info.motion_proj_mutex);

    /*!< Parse Q */
    uint32_t node_idx = 0;

    EB_NEW(dec_mt_frame_data->parse_tile_resource_ptr,
           eb_system_resource_ctor,
           num_tiles, /*!< object_total_count */
           1, /*!< producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
           1, /*!< consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
           dec_dummy_creator,
           &node_idx,
           NULL);

    dec_mt_frame_data->parse_tile_producer_fifo_ptr = eb_system_resource_get_producer_fifo(
        dec_mt_frame_data->parse_tile_resource_ptr, 0); /*!< producer_fifo */
    dec_mt_frame_data->parse_tile_consumer_fifo_ptr = eb_system_resource_get_consumer_fifo(
        dec_mt_frame_data->parse_tile_resource_ptr, 0); /*!< consumer_fifo */

    /*!< Recon queue */
    EB_NEW(dec_mt_frame_data->recon_tile_resource_ptr,
           eb_system_resource_ctor,
           num_tiles, /*!< object_total_count */
           1, /*!< producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
           1, /*!< consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
           dec_dummy_creator,
           &node_idx,
           NULL);

    dec_mt_frame_data->recon_tile_producer_fifo_ptr =
        eb_system_resource_get_producer_fifo(dec_mt_frame_data->recon_tile_resource_ptr, 0);
    dec_mt_frame_data->recon_tile_consumer_fifo_ptr =
        eb_system_resource_get_consumer_fifo(dec_mt_frame_data->recon_tile_resource_ptr, 0);

    int32_t  sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    uint32_t picture_height_in_sb =
        (dec_handle_ptr->seq_header.max_frame_height + sb_size_h - 1) / sb_size_h;

    /*!< LF queue */
    EB_NEW(dec_mt_frame_data->lf_frame_info.lf_resource_ptr,
           eb_system_resource_ctor,
           picture_height_in_sb, /*!< object_total_count */
           1, /*!< producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
           1, /*!< consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
           dec_dummy_creator,
           &node_idx,
           NULL);

    dec_mt_frame_data->lf_frame_info.lf_row_producer_fifo_ptr =
        eb_system_resource_get_producer_fifo(dec_mt_frame_data->lf_frame_info.lf_resource_ptr, 0);
    dec_mt_frame_data->lf_frame_info.lf_row_consumer_fifo_ptr =
        eb_system_resource_get_consumer_fifo(dec_mt_frame_data->lf_frame_info.lf_resource_ptr, 0);

    /*!< CDEF queue */
    EB_NEW(dec_mt_frame_data->cdef_resource_ptr,
           eb_system_resource_ctor,
           picture_height_in_sb, /*!< object_total_count */
           1, /*!< producer procs cnt : 1 Q per cnt is created inside, so kept 1 */
           1, /*!< consumer prcos cnt : 1 Q per cnt is created inside, so kept 1 */
           dec_dummy_creator,
           &node_idx,
           NULL);
    dec_mt_frame_data->cdef_row_producer_fifo_ptr =
        eb_system_resource_get_producer_fifo(dec_mt_frame_data->cdef_resource_ptr, 0);
    dec_mt_frame_data->cdef_row_consumer_fifo_ptr =
        eb_system_resource_get_consumer_fifo(dec_mt_frame_data->cdef_resource_ptr, 0);
    /************************************/
    /*!< Contexts */
    /************************************/

    /*!< Recon */
    EB_MALLOC_DEC(uint32_t *,
                  dec_mt_frame_data->sb_recon_row_map,
                  picture_height_in_sb * tiles_info->tile_cols * sizeof(uint32_t),
                  EB_N_PTR);
    /*!< recon top right sync */
    {
        int32_t tiles_ctr;

        EB_CREATE_MUTEX(dec_mt_frame_data->tile_switch_mutex);

        EB_MALLOC_DEC(DecMtParseReconTileInfo *,
                      dec_mt_frame_data->parse_recon_tile_info_array,
                      num_tiles * sizeof(DecMtParseReconTileInfo),
                      EB_N_PTR);

        for (tiles_ctr = 0; tiles_ctr < num_tiles; tiles_ctr++) {
            int32_t   tile_row = tiles_ctr / tiles_info->tile_cols;
            int32_t   tile_col = tiles_ctr % tiles_info->tile_cols;
            int32_t   tile_num_sb_rows;
            TileInfo *tile_info;

            tile_info = &dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].tile_info;

            /*!< init tile info */
            svt_tile_init(tile_info, &dec_handle_ptr->frame_header, tile_row, tile_col);

            tile_num_sb_rows = ((((tile_info->mi_row_end - 1) << MI_SIZE_LOG2) >>
                                 dec_handle_ptr->seq_header.sb_size_log2) -
                                ((tile_info->mi_row_start << MI_SIZE_LOG2) >>
                                 dec_handle_ptr->seq_header.sb_size_log2) +
                                1);

            dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].tile_num_sb_rows =
                tile_num_sb_rows;

            EB_MALLOC_DEC(
                uint32_t *,
                dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].sb_recon_row_parsed,
                tile_num_sb_rows * sizeof(uint32_t),
                EB_N_PTR);

            EB_MALLOC_DEC(
                uint32_t *,
                dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].sb_recon_completed_in_row,
                tile_num_sb_rows * sizeof(uint32_t),
                EB_N_PTR);

            EB_MALLOC_DEC(
                uint32_t *,
                dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].sb_recon_row_started,
                tile_num_sb_rows * sizeof(uint32_t),
                EB_N_PTR);

            EB_CREATE_MUTEX(
                dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].tile_sbrow_mutex);

            /*!< SB row queue */
            //EB_NEW(dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].recon_tile_sbrow_resource_ptr,
            //    eb_system_resource_ctor,
            //    tile_num_sb_rows, /*!< object_total_count */
            //    1, /*!< producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
            //    1, /*!< consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
            //    &dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].recon_tile_sbrow_producer_fifo_ptr, /*!< producer_fifo */
            //    &dec_mt_frame_data->parse_recon_tile_info_array[tiles_ctr].recon_tile_sbrow_consumer_fifo_ptr, /*!< consumer_fifo */
            //    EB_TRUE, /*!< Full Queue*/
            //    dec_dummy_creator,
            //    &node_idx,
            //    NULL);
        }
    }

    /*!< LF */
    EB_MALLOC_DEC(int32_t *,
                  dec_mt_frame_data->lf_frame_info.sb_lf_completed_in_row,
                  picture_height_in_sb * sizeof(int32_t),
                  EB_N_PTR);

    EB_MALLOC_DEC(uint32_t *,
                  dec_mt_frame_data->lf_row_map,
                  picture_height_in_sb * sizeof(uint32_t),
                  EB_N_PTR);

    /*!< CDEF */
    const int32_t num_planes = av1_num_planes(&dec_handle_ptr->seq_header.color_config);
    uint32_t      mi_cols    = 2 * ((dec_handle_ptr->seq_header.max_frame_width + 7) >> 3);

    const int32_t nhfb = (mi_cols + MI_SIZE_64X64 - 1) / MI_SIZE_64X64;
    const int32_t nvfb =
        (dec_handle_ptr->seq_header.max_frame_height + (MI_SIZE_64X64 << MI_SIZE_LOG2) - 1) /
        (MI_SIZE_64X64 << MI_SIZE_LOG2);

    const int32_t stride                   = (mi_cols << MI_SIZE_LOG2) + 2 * CDEF_HBORDER;
    dec_mt_frame_data->cdef_linebuf_stride = stride;

    /*!< ToDo: Linebuff memory we can allocate min(sb_rows , threads)*/
    /*!< Currently we r allocating for every (64x64 +1 )rows*/
    EB_MALLOC_DEC(
        uint16_t ***, dec_mt_frame_data->cdef_linebuf, (nvfb + 1) * sizeof(uint16_t **), EB_N_PTR);
    for (int32_t sb_row = 0; sb_row < (nvfb + 1); sb_row++) {
        uint16_t **p_linebuf;
        EB_MALLOC_DEC(uint16_t **,
                      dec_mt_frame_data->cdef_linebuf[sb_row],
                      num_planes * sizeof(uint16_t **),
                      EB_N_PTR);
        p_linebuf = dec_mt_frame_data->cdef_linebuf[sb_row];
        for (int32_t pli = 0; pli < num_planes; pli++) {
            EB_MALLOC_DEC(
                uint16_t *, p_linebuf[pli], sizeof(uint16_t) * CDEF_VBORDER * stride, EB_N_PTR);
        }
    }

    dec_mt_frame_data->cdef_map_stride = nhfb + 2;
    /*!< For fbr=0, previous row cdef points some junk memory,
     *   if we allocate memory only for nvfb 64x64 blocks,
     *   to avoid to pointing junck memory, we allocate nvfb+1 64x64 blocks */
    EB_MALLOC_DEC(uint8_t *,
                  dec_mt_frame_data->row_cdef_map,
                  (nvfb + 1) * dec_mt_frame_data->cdef_map_stride * sizeof(uint8_t),
                  EB_N_PTR);
    memset(dec_mt_frame_data->row_cdef_map,
           1,
           (nvfb + 1) * dec_mt_frame_data->cdef_map_stride * sizeof(uint8_t));

    EB_MALLOC_DEC(uint32_t *,
                  dec_mt_frame_data->cdef_completed_in_row,
                  (nvfb + 2) * sizeof(uint32_t),
                  EB_N_PTR);
    memset(dec_mt_frame_data->cdef_completed_in_row,
           0,
           (nvfb + 2) * /*!< Rem here nhbf+2 u replaced with nvfb + 2 */
               sizeof(uint32_t));
    dec_mt_frame_data->temp_mutex = eb_create_mutex();

    dec_mt_frame_data->start_motion_proj  = EB_FALSE;
    dec_mt_frame_data->start_parse_frame  = EB_FALSE;
    dec_mt_frame_data->start_decode_frame = EB_FALSE;
    dec_mt_frame_data->start_lf_frame     = EB_FALSE;
    dec_mt_frame_data->start_cdef_frame   = EB_FALSE;
    dec_mt_frame_data->num_threads_cdefed = 0;

    /************************************/
    /*!< Thread Handles */
    /************************************/

    /*!< Decode Library Threads */
    uint32_t num_lib_threads = (int32_t)dec_handle_ptr->dec_config.threads - 1;

    dec_mt_frame_data->end_flag           = EB_FALSE;
    dec_mt_frame_data->num_threads_exited = 0;

    if (num_lib_threads > 0) {
        DecThreadCtxt *thread_ctxt_pa;
        EB_MALLOC_DEC(
            DecThreadCtxt *, thread_ctxt_pa, num_lib_threads * sizeof(DecThreadCtxt), EB_N_PTR);
        dec_handle_ptr->thread_ctxt_pa = thread_ctxt_pa;
        EB_CREATE_SEMAPHORE(dec_handle_ptr->thread_semaphore, 0, 100000);
        for (uint32_t i = 0; i < num_lib_threads; i++) {
            thread_ctxt_pa[i].thread_cnt     = i + 1;
            thread_ctxt_pa[i].dec_handle_ptr = dec_handle_ptr;
            init_dec_mod_ctxt(dec_handle_ptr, &thread_ctxt_pa[i].dec_mod_ctxt);
            EB_CREATE_SEMAPHORE(thread_ctxt_pa[i].thread_semaphore, 0, 100000);
        }
        EB_CREATE_THREAD_ARRAY(dec_handle_ptr->decode_thread_handle_array,
                               num_lib_threads,
                               dec_all_stage_kernel,
                               (void **)&thread_ctxt_pa);
    }
    dec_handle_ptr->start_thread_process = EB_TRUE;
    return return_error;
}

/*!< Scan through the Tiles to find Bitstream offsets */
void svt_av1_scan_tiles(EbDecHandle *dec_handle_ptr, TilesInfo *tiles_info, ObuHeader *obu_header,
                        Bitstrm *bs, uint32_t tg_start, uint32_t tg_end) {
    size_t           tile_size;
    MasterParseCtxt *master_parse_ctxt = (MasterParseCtxt *)dec_handle_ptr->pv_master_parse_ctxt;
    ParseTileData *  parse_tile_data   = master_parse_ctxt->parse_tile_data;

    for (uint32_t tile_num = tg_start; tile_num <= tg_end; tile_num++) {
        if (tile_num == tg_end)
            tile_size = obu_header->payload_size;
        else {
            tile_size = dec_get_bits_le(bs, tiles_info->tile_size_bytes) + 1;
            obu_header->payload_size -= (tiles_info->tile_size_bytes + tile_size);
        }
        PRINT_FRAME("tile_size", (tile_size));

        /*!< Assign to ParseCtxt */
        parse_tile_data[tile_num].data      = get_bitsteam_buf(bs);
        parse_tile_data[tile_num].data_end  = bs->buf_max;
        parse_tile_data[tile_num].tile_size = tile_size;

        dec_bits_init(bs, (get_bitsteam_buf(bs) + tile_size), obu_header->payload_size);
    }
}
void svt_av1_queue_parse_jobs(EbDecHandle *dec_handle_ptr, TilesInfo *tiles_info, uint32_t tg_start,
                              uint32_t tg_end) {
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *parse_results_wrapper_ptr;
    int32_t          sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    uint32_t         picture_height_in_sb =
        (dec_handle_ptr->frame_header.frame_size.frame_height + sb_size_h - 1) / sb_size_h;
    EB_MEMSET(dec_mt_frame_data->sb_recon_row_map,
              0,
              picture_height_in_sb * tiles_info->tile_cols * sizeof(uint32_t));
    for (uint32_t tile_num = tg_start; tile_num <= tg_end; tile_num++) {
        /*!< Get Empty Parse Tile Job */
        eb_get_empty_object(dec_mt_frame_data->parse_tile_producer_fifo_ptr,
                            &parse_results_wrapper_ptr);

        DecMtNode *context_ptr  = (DecMtNode *)parse_results_wrapper_ptr->object_ptr;
        context_ptr->node_index = tile_num;

        /*!< Post Parse Tile Job */
        eb_post_full_object(parse_results_wrapper_ptr);
    }

    //dec_handle_ptr->start_thread_process = EB_TRUE;
}

EbErrorType parse_tile_job(EbDecHandle *dec_handle_ptr, int32_t tile_num) {
    EbErrorType status = EB_ErrorNone;

    TilesInfo *      tiles_info        = &dec_handle_ptr->frame_header.tiles_info;
    MasterParseCtxt *master_parse_ctxt = (MasterParseCtxt *)dec_handle_ptr->pv_master_parse_ctxt;
    ParseCtxt *      parse_ctxt        = &master_parse_ctxt->tile_parse_ctxt[tile_num];

    parse_ctxt->seq_header   = &dec_handle_ptr->seq_header;
    parse_ctxt->frame_header = &dec_handle_ptr->frame_header;

    parse_ctxt->parse_above_nbr4x4_ctxt = &master_parse_ctxt->parse_above_nbr4x4_ctxt[tile_num];
    parse_ctxt->parse_left_nbr4x4_ctxt  = &master_parse_ctxt->parse_left_nbr4x4_ctxt[tile_num];

    start_parse_tile(dec_handle_ptr, parse_ctxt, tiles_info, tile_num, 1);

    return status;
}
void recon_tile_job_post(DecMtFrameData *dec_mt_frame_data, uint32_t node_index) {
    EbObjectWrapper *recon_results_wrapper_ptr;
    /*!< Get Empty Recon Tile Job */
    eb_get_empty_object(dec_mt_frame_data->recon_tile_producer_fifo_ptr,
                        &recon_results_wrapper_ptr);

    DecMtNode *recon_context_ptr  = (DecMtNode *)recon_results_wrapper_ptr->object_ptr;
    recon_context_ptr->node_index = node_index;
    //SVT_LOG("\nPost dec job in queue Thread id : %d Tile id : %d \n",
    //    th_cnt, recon_context_ptr->node_index);
    /*!< Post Recon Tile Job */
    eb_post_full_object(recon_results_wrapper_ptr);
}
void parse_frame_tiles(EbDecHandle *dec_handle_ptr, DecThreadCtxt *thread_ctxt) {
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *parse_results_wrapper_ptr;
    DecMtNode *      context_ptr;

    volatile EbBool *start_parse_frame = &dec_mt_frame_data->start_parse_frame;
    while (*start_parse_frame != EB_TRUE)
        eb_block_on_semaphore(NULL == thread_ctxt ? dec_handle_ptr->thread_semaphore
                                                  : thread_ctxt->thread_semaphore);
    while (1) {
        eb_dec_get_full_object_non_blocking(dec_mt_frame_data->parse_tile_consumer_fifo_ptr,
                                            &parse_results_wrapper_ptr);

        if (NULL != parse_results_wrapper_ptr) {
            context_ptr = (DecMtNode *)parse_results_wrapper_ptr->object_ptr;
            recon_tile_job_post(dec_mt_frame_data, context_ptr->node_index);
            dec_mt_frame_data->start_decode_frame = EB_TRUE;
            if (EB_ErrorNone != parse_tile_job(dec_handle_ptr, context_ptr->node_index)) {
                SVT_LOG("\nParse Issue for Tile %d", context_ptr->node_index);
                break;
            }
            eb_post_semaphore(dec_handle_ptr->thread_semaphore);
            for (uint32_t lib_thrd = 0; lib_thrd < dec_handle_ptr->dec_config.threads - 1;
                 lib_thrd++)
                eb_post_semaphore(dec_handle_ptr->thread_ctxt_pa[lib_thrd].thread_semaphore);
            /*!< Release Parse Results */
            eb_release_object(parse_results_wrapper_ptr);
        } else
            break;
    }
}

EbErrorType decode_tile_job(EbDecHandle *dec_handle_ptr, int32_t tile_num,
                            DecModCtxt *dec_mod_ctxt) {
    EbErrorType status     = EB_ErrorNone;
    TilesInfo * tiles_info = &dec_handle_ptr->frame_header.tiles_info;
    status                 = start_decode_tile(dec_handle_ptr, dec_mod_ctxt, tiles_info, tile_num);
    return status;
}

void decode_frame_tiles(EbDecHandle *dec_handle_ptr, DecThreadCtxt *thread_ctxt) {
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *recon_results_wrapper_ptr;
    DecMtNode *      context_ptr;

    volatile EbBool *start_decode_frame = &dec_mt_frame_data->start_decode_frame;
    while (*start_decode_frame != EB_TRUE)
        eb_block_on_semaphore(NULL == thread_ctxt ? dec_handle_ptr->thread_semaphore
                                                  : thread_ctxt->thread_semaphore);
    while (1) {
        DecModCtxt *dec_mod_ctxt = (DecModCtxt *)dec_handle_ptr->pv_dec_mod_ctxt;

        eb_dec_get_full_object_non_blocking(dec_mt_frame_data->recon_tile_consumer_fifo_ptr,
                                            &recon_results_wrapper_ptr);

        if (thread_ctxt != NULL) {
            dec_mod_ctxt = thread_ctxt->dec_mod_ctxt;

            /*!< TODO : Calling this function at a tile level is
                   excessive. Move this call to operate at a frame level.*/
            setup_segmentation_dequant(thread_ctxt->dec_mod_ctxt);
        }

        if (NULL != recon_results_wrapper_ptr) {
            context_ptr = (DecMtNode *)recon_results_wrapper_ptr->object_ptr;

            if (EB_ErrorNone !=
                decode_tile_job(dec_handle_ptr, context_ptr->node_index, dec_mod_ctxt)) {
                SVT_LOG("\nDecode Issue for Tile %d", context_ptr->node_index);
                break;
            }
            eb_release_object(recon_results_wrapper_ptr);
        } else {
            int32_t max_rows_pend = -1;
            int32_t tile_idx, next_tile_idx = -1;
            int32_t num_tiles = dec_handle_ptr->frame_header.tiles_info.tile_cols *
                                dec_handle_ptr->frame_header.tiles_info.tile_rows;

            eb_block_on_mutex(dec_mt_frame_data->tile_switch_mutex);

            /*!< logic for switching acroos tile in SB decode */
            for (tile_idx = 0; tile_idx < num_tiles; tile_idx++) {
                DecMtParseReconTileInfo *parse_recon_tile_info_array =
                    &dec_mt_frame_data->parse_recon_tile_info_array[tile_idx];
                int32_t tile_num_sb_rows = parse_recon_tile_info_array->tile_num_sb_rows;

                /*!< check for incompleted tile with maximum rows to be processed */
                if (parse_recon_tile_info_array->sb_row_to_process !=
                    parse_recon_tile_info_array->tile_num_sb_rows) {
                    int32_t ctr;
                    int32_t curr_tile_rows_pend;

                    for (ctr = 0; ctr < tile_num_sb_rows; ctr++) {
                        if (0 == parse_recon_tile_info_array->sb_recon_row_started[ctr]) { break; }
                    }

                    curr_tile_rows_pend = tile_num_sb_rows - ctr;

                    /*!< check for min completed rows in a tile */
                    if (max_rows_pend < curr_tile_rows_pend) {
                        max_rows_pend = curr_tile_rows_pend;
                        next_tile_idx = tile_idx;
                    }
                }
            }

            eb_release_mutex(dec_mt_frame_data->tile_switch_mutex);

            /*!< break from while 1 loop if no tile to be processed */
            if (-1 == next_tile_idx) {
                break;
            } else {
                if (EB_ErrorNone != decode_tile_job(dec_handle_ptr, next_tile_idx, dec_mod_ctxt)) {
                    SVT_LOG("\nDecode Issue for Tile %d", next_tile_idx);
                    break;
                }
            }
        }
    }
    return;
}

void svt_av1_queue_lf_jobs(EbDecHandle *dec_handle_ptr) {
    DecMtlfFrameInfo *lf_frame_info =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data.lf_frame_info;
    EbObjectWrapper *lf_results_wrapper_ptr;

    int32_t sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    /*!< ToDo : picture_height_in_sb used many places. Reuse! */
    uint32_t picture_height_in_sb =
        (dec_handle_ptr->frame_header.frame_size.frame_height + sb_size_h - 1) / sb_size_h;
    uint32_t y_sb_index;

    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;

    EB_MEMSET(dec_mt_frame_data->lf_row_map, 0, picture_height_in_sb * sizeof(uint32_t));

    memset(lf_frame_info->sb_lf_completed_in_row, -1, picture_height_in_sb * sizeof(int32_t));

    for (y_sb_index = 0; y_sb_index < picture_height_in_sb; ++y_sb_index) {
        /*!< Get Empty LF Frame Row Job */
        eb_get_empty_object(lf_frame_info->lf_row_producer_fifo_ptr, &lf_results_wrapper_ptr);

        DecMtNode *context_ptr  = (DecMtNode *)lf_results_wrapper_ptr->object_ptr;
        context_ptr->node_index = y_sb_index;

        /*!< Post Parse Tile Job */
        eb_post_full_object(lf_results_wrapper_ptr);
    }
}

/*!< Store LF_boundary_line req for LR */
static INLINE void dec_save_lf_boundary_lines_sb_row(EbDecHandle *  dec_handle,
                                                     Av1PixelRect **tile_rect, int32_t sb_row,
                                                     uint8_t **src, int32_t *stride,
                                                     int32_t num_planes) {
    Av1Common *cm         = &dec_handle->cm;
    FrameSize *frame_size = &dec_handle->frame_header.frame_size;
    EbBool     sb_128     = dec_handle->seq_header.sb_size == BLOCK_128X128;
    int32_t    num64s     = sb_128 ? 1 : 0;
    const int  use_highbd = (dec_handle->seq_header.color_config.bit_depth > 8);
    LrCtxt *   lr_ctxt    = (LrCtxt *)dec_handle->pv_lr_ctxt;

    int32_t frame_stripe /*!< 64 strip */, plane_height;
    for (int32_t p = 0; p < num_planes; ++p) {
        int32_t                      ss_x          = p ? cm->subsampling_x : 0;
        int32_t                      ss_y          = p ? cm->subsampling_y : 0;
        const int32_t                stripe_height = RESTORATION_PROC_UNIT_SIZE >> ss_y;
        const int32_t                stripe_off    = RESTORATION_UNIT_OFFSET >> ss_y;
        RestorationStripeBoundaries *boundaries    = &lr_ctxt->boundaries[p];

        plane_height = ROUND_POWER_OF_TWO(cm->frm_size.frame_height, ss_y);

        int32_t src_width  = frame_size->frame_width >> ss_x;
        int32_t src_height = frame_size->frame_height >> ss_y;

        for (int32_t row_cnt = 0; row_cnt <= num64s; row_cnt++) {
            frame_stripe         = (sb_row << num64s) + row_cnt;
            const int32_t rel_y0 = AOMMAX(0, frame_stripe * stripe_height - stripe_off);
            const int32_t y0     = tile_rect[p]->top + rel_y0;
            if (y0 >= tile_rect[p]->bottom) break;

            const int32_t rel_y1 = (frame_stripe + 1) * stripe_height - stripe_off;
            const int32_t y1     = AOMMIN(tile_rect[p]->top + rel_y1, tile_rect[p]->bottom);

            int32_t use_deblock_above, use_deblock_below;
            /*!< In this case, we should only use CDEF pixels at the top
             * and bottom of the frame as a whole; internal tile boundaries
             * can use deblocked pixels from adjacent tiles for context. */
            use_deblock_above = (frame_stripe > 0);
            use_deblock_below = (y1 < plane_height);

            /*!< Save deblocked context where needed. */
            if (use_deblock_above) {
                save_deblock_boundary_lines(src[p],
                                            stride[p],
                                            src_width,
                                            src_height,
                                            cm,
                                            p,
                                            y0 - RESTORATION_CTX_VERT,
                                            frame_stripe,
                                            use_highbd,
                                            1,
                                            boundaries);
            }
            if (use_deblock_below) {
                save_deblock_boundary_lines(src[p],
                                            stride[p],
                                            src_width,
                                            src_height,
                                            cm,
                                            p,
                                            y1,
                                            frame_stripe,
                                            use_highbd,
                                            0,
                                            boundaries);
            }
        }
    }
}

/*!< Frame level function to trigger loop filter for each superblock */
void dec_av1_loop_filter_frame_mt(EbDecHandle *dec_handle, EbPictureBufferDesc *recon_picture_buf,
                                  LfCtxt *lf_ctxt, LoopFilterInfoN *lf_info, int32_t plane_start,
                                  int32_t plane_end, DecThreadCtxt *thread_ctxt) {
    int32_t         sb_row;
    DecMtFrameData *dec_mt_frame_data1 =
        &dec_handle->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;

    volatile EbBool *start_lf_frame = &dec_mt_frame_data1->start_lf_frame;
    while (*start_lf_frame != EB_TRUE)
        eb_block_on_semaphore(NULL == thread_ctxt ? dec_handle->thread_semaphore
                                                  : thread_ctxt->thread_semaphore);
    FrameHeader *frm_hdr = &dec_handle->frame_header;

    lf_ctxt->delta_lf_stride = dec_handle->master_frame_buf.sb_cols * FRAME_LF_COUNT;
    frm_hdr->loop_filter_params.combine_vert_horz_lf = 1;
    /*!< init hev threshold const vectors */
    for (int lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++)
        memset(lf_info->lfthr[lvl].hev_thr, (lvl >> 4), SIMD_WIDTH);

    eb_av1_loop_filter_frame_init(frm_hdr, lf_info, plane_start, plane_end);

    set_lbd_lf_filter_tap_functions();
    set_hbd_lf_filter_tap_functions();

    /*!< For LF boundary store : Can optimize based on CDEF & LR flag
     * Get the tile rectangle, with height rounded up to the next multiple of 8
     * luma pixels (only relevant for the bottom tile of the frame) */
    Av1PixelRect         tile_rect[MAX_MB_PLANE];
    Av1PixelRect *       tile_rect_p[MAX_MB_PLANE];
    const int            num_planes  = av1_num_planes(&dec_handle->seq_header.color_config);
    EbPictureBufferDesc *cur_pic_buf = dec_handle->cur_pic_buf[0]->ps_pic_buf;
    uint8_t *            src[MAX_MB_PLANE];
    int32_t              stride[MAX_MB_PLANE];
    for (int p = 0; p < num_planes; ++p) {
        int32_t is_uv  = p ? 1 : 0;
        int32_t sx     = is_uv ? dec_handle->cm.subsampling_x : 0;
        int32_t sy     = is_uv ? dec_handle->cm.subsampling_y : 0;
        tile_rect[p]   = whole_frame_rect(&dec_handle->cm.frm_size, sx, sy, is_uv);
        tile_rect_p[p] = &tile_rect[p];
        derive_blk_pointers(cur_pic_buf, p, 0, 0, (void *)&src[p], &stride[p], sx, sy);
    }

    DecMtFrameData *dec_mt_frame_data =
        &dec_handle->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *lf_results_wrapper_ptr;
    DecMtNode *      context_ptr;

    while (1) {
        eb_dec_get_full_object_non_blocking(
            dec_mt_frame_data->lf_frame_info.lf_row_consumer_fifo_ptr, &lf_results_wrapper_ptr);

        if (NULL != lf_results_wrapper_ptr) {
            context_ptr = (DecMtNode *)lf_results_wrapper_ptr->object_ptr;

            TilesInfo *tiles_info = &dec_handle->frame_header.tiles_info;
            sb_row                = context_ptr->node_index;

            /*!< Ensure all TileRecon jobs are over for (row, row-1, row+1)      */
            /*!< row-1 : To ensure line buf copy with TopR sync if LF skips row  */
            /*!< This prevent issues across Tiles where recon sync is not ensured */
            /*!< row+1 : This is for CDEF actually, should be moved to CDEF stage */
            int32_t start_lf[3] = {0};
            int32_t row_index[3];
            row_index[0] = (sb_row)*tiles_info->tile_cols;
            row_index[1] = (sb_row - (sb_row == 0 ? 0 : 1)) * tiles_info->tile_cols;
            row_index[2] = (sb_row + (sb_row == (dec_mt_frame_data->sb_rows - 1) ? 0 : 1)) *
                           tiles_info->tile_cols;
            while ((!start_lf[0]) || (!start_lf[1]) || (!start_lf[2])) {
                start_lf[0] = 1;
                start_lf[1] = 1;
                start_lf[2] = 1;
                for (int i = 0; i < tiles_info->tile_cols; i++) {
                    start_lf[0] &= dec_mt_frame_data->sb_recon_row_map[row_index[0] + i];
                    start_lf[1] &= dec_mt_frame_data->sb_recon_row_map[row_index[1] + i];
                    start_lf[2] &= dec_mt_frame_data->sb_recon_row_map[row_index[2] + i];
                }
            }

            if (!dec_handle->frame_header.allow_intrabc) {
                if (dec_handle->frame_header.loop_filter_params.filter_level[0] ||
                    dec_handle->frame_header.loop_filter_params.filter_level[1]) {
                    dec_loop_filter_row(dec_handle,
                                        recon_picture_buf,
                                        lf_ctxt,
                                        lf_info,
                                        sb_row,
                                        plane_start,
                                        plane_end);
                }
            }

            /*!< Store LR_save_boundary_lines at 64 lines : After LF         */
            /*!< Store Above 64 line always, for SB 128 store Middle 64 also */
            /*!< Bottom 64 won't be ready yet as next SB row Lf can modify it */
            /*!< TO DO: Should be based on LR flag! */
            dec_save_lf_boundary_lines_sb_row(
                dec_handle, tile_rect_p, sb_row, src, stride, num_planes);

            /*!< Update LF done map */
            dec_mt_frame_data1->lf_row_map[context_ptr->node_index] = 1;

            /*!< Release LF Results */
            eb_release_object(lf_results_wrapper_ptr);
        } else
            break;
    }
}

void svt_av1_queue_cdef_jobs(EbDecHandle *dec_handle_ptr) {
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *cdef_results_wrapper_ptr;

    int32_t  sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    uint32_t picture_height_in_sb =
        (dec_handle_ptr->frame_header.frame_size.frame_height + sb_size_h - 1) / sb_size_h;

    const int32_t nvfb = (dec_handle_ptr->frame_header.mi_rows + MI_SIZE_64X64 - 1) / MI_SIZE_64X64;

    memset(dec_mt_frame_data->cdef_completed_in_row, 0, nvfb * sizeof(uint32_t));

    for (uint32_t sb_fbr = 0; sb_fbr < picture_height_in_sb; ++sb_fbr) {
        /*!< Get Empty LF Frame Row Job */
        eb_get_empty_object(dec_mt_frame_data->cdef_row_producer_fifo_ptr,
                            &cdef_results_wrapper_ptr);

        DecMtNode *context_ptr  = (DecMtNode *)cdef_results_wrapper_ptr->object_ptr;
        context_ptr->node_index = sb_fbr;

        /*!< Post Parse Tile Job */
        eb_post_full_object(cdef_results_wrapper_ptr);
    }
}
void svt_cdef_frame_mt(EbDecHandle *dec_handle_ptr, DecThreadCtxt *thread_ctxt) {
    uint8_t *       curr_blk_recon_buf[MAX_MB_PLANE];
    int32_t         curr_recon_stride[MAX_MB_PLANE];
    DecMtFrameData *dec_mt_frame_data1 =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    volatile EbBool *start_cdef_frame = &dec_mt_frame_data1->start_cdef_frame;
    while (*start_cdef_frame != EB_TRUE)
        eb_block_on_semaphore(NULL == thread_ctxt ? dec_handle_ptr->thread_semaphore
                                                  : thread_ctxt->thread_semaphore);
    EbPictureBufferDesc *recon_picture_ptr = dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf;
    const int32_t        num_planes = av1_num_planes(&dec_handle_ptr->seq_header.color_config);

    DECLARE_ALIGNED(16, uint16_t, src[CDEF_INBUF_SIZE]);
    uint16_t *colbuf[2 * 3];
    int32_t   mi_wide_l2[3];
    int32_t   mi_high_l2[3];

    for (int32_t pli = 0; pli < num_planes; pli++) {
        int32_t sub_x   = (pli == 0) ? 0 : dec_handle_ptr->seq_header.color_config.subsampling_x;
        int32_t sub_y   = (pli == 0) ? 0 : dec_handle_ptr->seq_header.color_config.subsampling_y;
        mi_wide_l2[pli] = MI_SIZE_LOG2 - sub_x;
        mi_high_l2[pli] = MI_SIZE_LOG2 - sub_y;

        /*!< Deriveing  recon pict buffer ptr's */
        derive_blk_pointers(recon_picture_ptr,
                            pli,
                            0,
                            0,
                            (void *)&curr_blk_recon_buf[pli],
                            &curr_recon_stride[pli],
                            sub_x,
                            sub_y);

        if (dec_handle_ptr->seq_header.sb_size == BLOCK_128X128) {
            /*!< For SB SIZE 128x128, we need two colbuf because, because we do cdef for
             *   each 64x64 in SB block in raster scan order,
             *   i.e for transversing across 0 - 3 64x64s in SB block*/
            for (int32_t i = 0; i < 4; i += 3)
                colbuf[pli + i] = (uint16_t *)eb_aom_malloc(
                    sizeof(*colbuf) * ((CDEF_BLOCKSIZE << mi_high_l2[pli]) + 2 * CDEF_VBORDER) *
                    CDEF_HBORDER);
        } else {
            colbuf[pli] = (uint16_t *)eb_aom_malloc(
                sizeof(*colbuf) * ((CDEF_BLOCKSIZE << mi_high_l2[pli]) + 2 * CDEF_VBORDER) *
                CDEF_HBORDER);
        }
    }

    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *cdef_results_wrapper_ptr;
    DecMtNode *      context_ptr;

    while (1) {
        eb_dec_get_full_object_non_blocking(dec_mt_frame_data->cdef_row_consumer_fifo_ptr,
                                            &cdef_results_wrapper_ptr);

        if (NULL != cdef_results_wrapper_ptr) {
            context_ptr = (DecMtNode *)cdef_results_wrapper_ptr->object_ptr;

            /*!< Ensure all LF jobs are over for row_index (row / row+1) */
            int32_t offset =
                (int32_t)context_ptr->node_index == dec_mt_frame_data->sb_rows - 1 ? 0 : 1;

            volatile int32_t *start_cdef = (volatile int32_t *)&dec_mt_frame_data
                                               ->lf_row_map[context_ptr->node_index + offset];
            while (!*start_cdef)
                ;

            FrameHeader *frame_header = &dec_handle_ptr->frame_header;
            if (!frame_header->allow_intrabc) {
                const int32_t do_cdef = !frame_header->coded_lossless &&
                                        (frame_header->cdef_params.cdef_bits ||
                                         frame_header->cdef_params.cdef_y_strength[0] ||
                                         frame_header->cdef_params.cdef_uv_strength[0]);
                if (do_cdef) {
                    /*!< SB Row Index */
                    int32_t sb_fbr = (int32_t)context_ptr->node_index;
                    svt_cdef_sb_row_mt(dec_handle_ptr,
                                       mi_wide_l2,
                                       mi_high_l2,
                                       &colbuf[0],
                                       sb_fbr,
                                       &src[0],
                                       &curr_recon_stride[0],
                                       &curr_blk_recon_buf[0]);
                }
            }
            /*!< Release Parse Results */
            eb_release_object(cdef_results_wrapper_ptr);
        } else
            break;
    }
    if (dec_handle_ptr->seq_header.sb_size == BLOCK_128X128) {
        for (int32_t i = 0; i < 4; i += 3) {
            for (int32_t pli = 0; pli < num_planes; pli++) { eb_aom_free(colbuf[pli + i]); }
        }
    } else
        for (int32_t pli = 0; pli < num_planes; pli++) { eb_aom_free(colbuf[pli]); }
    const int32_t nvfb = (dec_handle_ptr->frame_header.mi_rows + MI_SIZE_64X64 - 1) / MI_SIZE_64X64;

    eb_block_on_mutex(dec_mt_frame_data->temp_mutex);
    dec_mt_frame_data->num_threads_cdefed++;
    if (dec_handle_ptr->dec_config.threads == dec_mt_frame_data->num_threads_cdefed) {
        dec_mt_frame_data->start_motion_proj  = EB_FALSE;
        dec_mt_frame_data->start_parse_frame  = EB_FALSE;
        dec_mt_frame_data->start_decode_frame = EB_FALSE;
        dec_mt_frame_data->start_lf_frame     = EB_FALSE;
        dec_mt_frame_data->start_cdef_frame   = EB_FALSE;
        memset(dec_mt_frame_data->cdef_completed_in_row, 0, nvfb * sizeof(uint32_t));
    }
    eb_release_mutex(dec_mt_frame_data->temp_mutex);

    volatile uint32_t *num_threads_cdefed = &dec_mt_frame_data->num_threads_cdefed;
    while (*num_threads_cdefed != dec_handle_ptr->dec_config.threads)
        ;
}

void *dec_all_stage_kernel(void *input_ptr) {
    /*!< Context */
    DecThreadCtxt * thread_ctxt    = (DecThreadCtxt *)input_ptr;
    EbDecHandle *   dec_handle_ptr = thread_ctxt->dec_handle_ptr;
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    volatile EbBool *start_thread = (volatile EbBool *)&dec_handle_ptr->start_thread_process;
    while (*start_thread == EB_FALSE)
        ;

    while (1) {
        /*!< Motion Field Projection */
        svt_setup_motion_field(dec_handle_ptr, thread_ctxt);
        /*!< Parse Tiles */
        parse_frame_tiles(dec_handle_ptr, thread_ctxt);
        /*!< Decode Tiles */
        decode_frame_tiles(dec_handle_ptr, thread_ctxt);
        dec_av1_loop_filter_frame_mt(dec_handle_ptr,
                                     dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf,
                                     dec_handle_ptr->pv_lf_ctxt,
                                     &thread_ctxt->lf_info,
                                     AOM_PLANE_Y,
                                     MAX_MB_PLANE,
                                     thread_ctxt);
        /*!< Frame CDEF */
        svt_cdef_frame_mt(dec_handle_ptr, thread_ctxt);
        if (EB_TRUE == dec_mt_frame_data->end_flag) {
            eb_block_on_mutex(dec_mt_frame_data->temp_mutex);
            dec_mt_frame_data->num_threads_exited++;
            eb_release_mutex(dec_mt_frame_data->temp_mutex);
            break;
        }
    }
    return EB_NULL;
}

void dec_sync_all_threads(EbDecHandle *dec_handle_ptr) {
    DecMtFrameData *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    dec_mt_frame_data->end_flag = EB_TRUE;

    /*!< To make all worker exit except main thread! */
    dec_mt_frame_data->num_threads_cdefed = 1;
    /*!< To make all worker exit except main thread! */
    dec_mt_frame_data->num_threads_header          = 1;
    dec_handle_ptr->frame_header.use_ref_frame_mvs = 0;
    dec_mt_frame_data->start_motion_proj           = EB_TRUE;

    dec_mt_frame_data->start_parse_frame = EB_TRUE;
    eb_post_semaphore(dec_handle_ptr->thread_semaphore);
    for (uint32_t lib_thrd = 0; lib_thrd < dec_handle_ptr->dec_config.threads - 1; lib_thrd++)
        eb_post_semaphore(dec_handle_ptr->thread_ctxt_pa[lib_thrd].thread_semaphore);
    dec_mt_frame_data->start_decode_frame = EB_TRUE;
    eb_post_semaphore(dec_handle_ptr->thread_semaphore);
    for (uint32_t lib_thrd = 0; lib_thrd < dec_handle_ptr->dec_config.threads - 1; lib_thrd++)
        eb_post_semaphore(dec_handle_ptr->thread_ctxt_pa[lib_thrd].thread_semaphore);
    dec_mt_frame_data->start_lf_frame = EB_TRUE;
    eb_post_semaphore(dec_handle_ptr->thread_semaphore);
    for (uint32_t lib_thrd = 0; lib_thrd < dec_handle_ptr->dec_config.threads - 1; lib_thrd++)
        eb_post_semaphore(dec_handle_ptr->thread_ctxt_pa[lib_thrd].thread_semaphore);
    dec_mt_frame_data->start_cdef_frame = EB_TRUE;
    eb_post_semaphore(dec_handle_ptr->thread_semaphore);
    for (uint32_t lib_thrd = 0; lib_thrd < dec_handle_ptr->dec_config.threads - 1; lib_thrd++)
        eb_post_semaphore(dec_handle_ptr->thread_ctxt_pa[lib_thrd].thread_semaphore);
    while (dec_mt_frame_data->num_threads_exited != dec_handle_ptr->dec_config.threads - 1)
        eb_sleep_ms(5);
}
