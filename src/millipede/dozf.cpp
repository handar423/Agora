/**
 * Author: Jian Ding
 * Email: jianding17@gmail.com
 * 
 */
#include "dozf.hpp"
#include "Consumer.hpp"
#include "doer.hpp"

using namespace arma;
DoZF::DoZF(Config* in_config, int in_tid,
    moodycamel::ConcurrentQueue<Event_data>& in_task_queue, Consumer& in_consumer,
    Table<complex_float>& in_csi_buffer, Table<complex_float>& in_precoder_buffer,
    Stats* in_stats_manager)
    : Doer(in_config, in_tid, in_task_queue, in_consumer)
    , csi_buffer_(in_csi_buffer)
    , precoder_buffer_(in_precoder_buffer)
{
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    int UE_NUM = config_->UE_NUM;
    alloc_buffer_1d(&pred_csi_buffer_, BS_ANT_NUM * UE_NUM, 64, 0);

    ZF_task_duration = &in_stats_manager->zf_stats_worker.task_duration;
    ZF_task_count = in_stats_manager->zf_stats_worker.task_count;
    // ZF_task_duration = in_ZF_task_duration;
    // ZF_task_count = in_ZF_task_count;

    csi_gather_buffer = (complex_float*)aligned_alloc(BS_ANT_NUM * UE_NUM * sizeof(complex_float), BS_ANT_NUM * UE_NUM * sizeof(complex_float));
}

DoZF::~DoZF()
{
    free(csi_gather_buffer);
    free_buffer_1d(&pred_csi_buffer_);
}

void DoZF::launch(int offset)
{
    if (config_->freq_orthogonal_pilot)
        ZF_freq_orthogonal(offset);
    else
        ZF_time_orthogonal(offset);

    // inform main thread
    Event_data ZF_finish_event;
    ZF_finish_event.event_type = EVENT_ZF;
    ZF_finish_event.data = offset;
    consumer_.handle(ZF_finish_event);
}

void DoZF::ZF_time_orthogonal(int offset)
{
    int OFDM_DATA_NUM = config_->OFDM_DATA_NUM;
    int zf_block_size = config_->zf_block_size;

    int frame_id = offset / config_->zf_block_num;
    int sc_id = offset % config_->zf_block_num * zf_block_size;
#if DEBUG_PRINT_IN_TASK
    printf("In doZF thread %d: frame: %d, subcarrier: %d\n", tid, frame_id, sc_id);
#endif
    int offset_in_buffer = frame_id * OFDM_DATA_NUM + sc_id;
    int max_sc_ite = std::min(zf_block_size, OFDM_DATA_NUM - sc_id);
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    int UE_NUM = config_->UE_NUM;
    for (int i = 0; i < max_sc_ite; i++) {

#if DEBUG_UPDATE_STATS
        double start_time1 = get_time();
#endif
        int cur_sc_id = sc_id + i;
        int cur_offset = offset_in_buffer + i;
        int transpose_block_size = config_->transpose_block_size;
        // directly gather data from FFT buffer
        __m256i index = _mm256_setr_epi32(0, 1, transpose_block_size * 2, transpose_block_size * 2 + 1, transpose_block_size * 4,
            transpose_block_size * 4 + 1, transpose_block_size * 6, transpose_block_size * 6 + 1);
        int transpose_block_id = cur_sc_id / transpose_block_size;
        int sc_inblock_idx = cur_sc_id % transpose_block_size;
        int offset_in_csi_buffer = transpose_block_id * BS_ANT_NUM * transpose_block_size + sc_inblock_idx;
        int subframe_offset = frame_id * UE_NUM;
        float* tar_csi_ptr = (float*)csi_gather_buffer;

        // // if (sc_id == 4) {
        // //     cout<<"csi_buffer_ for subframe "<<subframe_offset<<endl;
        // //     for (int i=0;i<BS_ANT_NUM*OFDM_CA_NUM; i++) {
        // //         cout<<"("<<i<<",  "<<csi_buffer_.CSI[subframe_offset][i].real<<","<<csi_buffer_.CSI[subframe_offset][i].imag<<") ";
        // //     }

        // //     cout<<endl;
        // // }

        // gather data for all users and antennas
        // printf("In doZF thread %d: frame: %d, subcarrier: %d\n", tid, frame_id, sc_id);
        // int mat_elem = UE_NUM * BS_ANT_NUM;
        // int cache_line_num = mat_elem / 8;
        // for (int line_idx = 0; line_idx < cache_line_num; line_idx ++) {
        //     _mm_prefetch((char *)precoder_buffer_.precoder[cur_offset + 8 * line_idx], _MM_HINT_ET1);
        //     // _mm_prefetch((char *)(tar_csi_ptr + 16 * line_idx), _MM_HINT_ET1);
        // }

        for (int ue_idx = 0; ue_idx < UE_NUM; ue_idx++) {
            float* src_csi_ptr = (float*)csi_buffer_[subframe_offset + ue_idx] + offset_in_csi_buffer * 2;
            for (int ant_idx = 0; ant_idx < BS_ANT_NUM; ant_idx += 4) {
                // fetch 4 complex floats for 4 ants
                __m256 t_csi = _mm256_i32gather_ps(src_csi_ptr, index, 4);
                _mm256_store_ps(tar_csi_ptr, t_csi);
                // printf("UE %d, ant %d, data: %.4f, %.4f, %.4f, %.4f, %.4f, %.4f\n", ue_idx, ant_idx, *((float *)tar_csi_ptr), *((float *)tar_csi_ptr+1),
                //         *((float *)tar_csi_ptr+2), *((float *)tar_csi_ptr+3),  *((float *)tar_csi_ptr+4), *((float *)tar_csi_ptr+5));
                src_csi_ptr += 8 * transpose_block_size;
                tar_csi_ptr += 8;
            }
        }

        // // gather data for all users and antennas
        // for (int ue_idx = 0; ue_idx < UE_NUM; ue_idx++) {
        //     float *src_csi_ptr = (float *)fft_buffer_.FFT_outputs[subframe_offset+ue_idx] + sc_id * 2;
        //     for (int ant_idx = 0; ant_idx < BS_ANT_NUM; ant_idx += 4) {
        //         // fetch 4 complex floats for 4 ants
        //         __m256 pilot_rx = _mm256_i32gather_ps(src_csi_ptr, index, 4);

        //         if (pilots_[sc_id] > 0) {
        //             _mm256_store_ps(tar_csi_ptr, pilot_rx);
        //         }
        //         else if (pilots_[sc_id] < 0){
        //             __m256 pilot_tx = _mm256_set1_ps(pilots_[sc_id]);
        //             __m256 csi_est = _mm256_mul_ps(pilot_rx, pilot_tx);
        //             _mm256_store_ps(tar_csi_ptr, csi_est);
        //         }
        //         else {
        //             _mm256_store_ps(tar_csi_ptr, _mm256_setzero_ps());
        //         }

        //         // printf("Frame %d, sc: %d, UE %d, ant %d, data: %.4f, %.4f, %.4f, %.4f, %.4f, %.4f\n", frame_id, sc_id, ue_idx, ant_idx, *((float *)tar_csi_ptr), *((float *)tar_csi_ptr+1),
        //         //         *((float *)tar_csi_ptr+2), *((float *)tar_csi_ptr+3),  *((float *)tar_csi_ptr+4), *((float *)tar_csi_ptr+5));
        //         src_csi_ptr += 8 * OFDM_CA_NUM;
        //         tar_csi_ptr += 8;

        //     }
        // }

#if DEBUG_UPDATE_STATS_DETAILED
        double duration1 = get_time() - start_time1;
        (*ZF_task_duration)[tid * 8][1] += duration1;
#endif
        cx_float* ptr_in = (cx_float*)csi_gather_buffer;
        cx_fmat mat_input(ptr_in, BS_ANT_NUM, UE_NUM, false);
        // cout<<"CSI matrix"<<endl;
        // cout<<mat_input.st()<<endl;
        cx_float* ptr_out = (cx_float*)precoder(&mat_input, frame_id, cur_sc_id, cur_offset);
        cx_fmat mat_output(ptr_out, UE_NUM, BS_ANT_NUM, false);

#if DEBUG_UPDATE_STATS_DETAILED
        double start_time2 = get_time();
        double duration2 = start_time2 - start_time1;
        (*ZF_task_duration)[tid * 8][2] += duration2;
#endif

        pinv(mat_output, mat_input, 1e-1, "dc");

        // cout<<"Precoder:" <<mat_output<<endl;
#if DEBUG_UPDATE_STATS_DETAILED
        double duration3 = get_time() - start_time2;
        (*ZF_task_duration)[tid * 8][3] += duration3;
#endif

        // float *tar_ptr = (float *)precoder_buffer_.precoder[cur_offset];
        // // float temp = *tar_ptr;
        // float *src_ptr = (float *)ptr_out;

        // // int mat_elem = UE_NUM * BS_ANT_NUM;
        // // int cache_line_num = mat_elem / 8;
        // for (int line_idx = 0; line_idx < cache_line_num; line_idx++) {
        //     _mm256_stream_ps(tar_ptr, _mm256_load_ps(src_ptr));
        //     _mm256_stream_ps(tar_ptr + 8, _mm256_load_ps(src_ptr + 8));
        //     tar_ptr += 16;
        //     src_ptr += 16;
        // }
// #if DEBUG_UPDATE_STATS
//     double duration3 = get_time() - start_time3;
//     (*ZF_task_duration)[tid][3] += duration3;
// #endif
#if DEBUG_UPDATE_STATS
        ZF_task_count[tid * 16] = ZF_task_count[tid * 16] + 1;
        double duration = get_time() - start_time1;
        (*ZF_task_duration)[tid * 8][0] += duration;
        if (duration > 500) {
            printf("Thread %d ZF takes %.2f\n", tid, duration);
        }
#endif
    }
}

void DoZF::ZF_freq_orthogonal(int offset)
{
    int OFDM_DATA_NUM = config_->OFDM_DATA_NUM;
    int zf_block_size = config_->zf_block_size;
    int UE_NUM = config_->UE_NUM;
    int frame_id = offset / config_->zf_block_num;
    int sc_id = offset % config_->zf_block_num * zf_block_size;
#if DEBUG_PRINT_IN_TASK
    printf("In doZF thread %d: frame: %d, subcarrier: %d, block: %d\n", tid, frame_id, sc_id, sc_id / UE_NUM);
#endif
    int offset_in_buffer = frame_id * OFDM_DATA_NUM + sc_id;

#if DEBUG_UPDATE_STATS
    double start_time1 = get_time();
#endif
    int transpose_block_size = config_->transpose_block_size;
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    for (int i = 0; i < UE_NUM; i++) {
        int cur_sc_id = sc_id + i;
        // directly gather data from FFT buffer
        __m256i index = _mm256_setr_epi32(0, 1, transpose_block_size * 2, transpose_block_size * 2 + 1, transpose_block_size * 4,
            transpose_block_size * 4 + 1, transpose_block_size * 6, transpose_block_size * 6 + 1);

        int transpose_block_id = cur_sc_id / transpose_block_size;
        int sc_inblock_idx = cur_sc_id % transpose_block_size;
        int offset_in_csi_buffer = transpose_block_id * BS_ANT_NUM * transpose_block_size + sc_inblock_idx;
        int subframe_offset = frame_id;
        float* tar_csi_ptr = (float*)csi_gather_buffer + BS_ANT_NUM * i * 2;

        float* src_csi_ptr = (float*)csi_buffer_[subframe_offset] + offset_in_csi_buffer * 2;
        for (int ant_idx = 0; ant_idx < BS_ANT_NUM; ant_idx += 4) {
            // fetch 4 complex floats for 4 ants
            __m256 t_csi = _mm256_i32gather_ps(src_csi_ptr, index, 4);
            _mm256_store_ps(tar_csi_ptr, t_csi);
            // printf("UE %d, ant %d, data: %.4f, %.4f, %.4f, %.4f, %.4f, %.4f\n", ue_idx, ant_idx, *((float *)tar_csi_ptr), *((float *)tar_csi_ptr+1),
            //         *((float *)tar_csi_ptr+2), *((float *)tar_csi_ptr+3),  *((float *)tar_csi_ptr+4), *((float *)tar_csi_ptr+5));
            src_csi_ptr += 8 * transpose_block_size;
            tar_csi_ptr += 8;
        }
    }

#if DEBUG_UPDATE_STATS_DETAILED
    double duration1 = get_time() - start_time1;
    (*ZF_task_duration)[tid * 8][1] += duration1;
#endif
    cx_float* ptr_in = (cx_float*)csi_gather_buffer;
    cx_fmat mat_input(ptr_in, BS_ANT_NUM, UE_NUM, false);
    // cout<<"CSI matrix"<<endl;
    // cout<<mat_input.st()<<endl;
    cx_float* ptr_out = (cx_float*)precoder(&mat_input, frame_id, sc_id, offset_in_buffer);
    cx_fmat mat_output(ptr_out, UE_NUM, BS_ANT_NUM, false);

#if DEBUG_UPDATE_STATS_DETAILED
    double start_time2 = get_time();
    double duration2 = start_time2 - start_time1;
    (*ZF_task_duration)[tid * 8][2] += duration2;
#endif

    pinv(mat_output, mat_input, 1e-1, "dc");

    // cout<<"Precoder:" <<mat_output<<endl;
#if DEBUG_UPDATE_STATS_DETAILED
    double duration3 = get_time() - start_time2;
    (*ZF_task_duration)[tid * 8][3] += duration3;
#endif

#if DEBUG_UPDATE_STATS
    ZF_task_count[tid * 16] = ZF_task_count[tid * 16] + 1;
    double duration = get_time() - start_time1;
    (*ZF_task_duration)[tid * 8][0] += duration;
    if (duration > 500) {
        printf("Thread %d ZF takes %.2f\n", tid, duration);
    }
#endif
}

void DoZF::Predict(int offset)
{
    int OFDM_DATA_NUM = config_->OFDM_DATA_NUM;
    int zf_block_size = config_->zf_block_size;
    int frame_id = offset / config_->zf_block_num;
    int sc_id = offset % config_->zf_block_num * zf_block_size;

    // Use stale CSI as predicted CSI
    // TODO: add prediction algorithm
    int offset_in_buffer = frame_id * OFDM_DATA_NUM + sc_id;
    cx_float* ptr_in = (cx_float*)pred_csi_buffer_;
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    int UE_NUM = config_->UE_NUM;
    memcpy(ptr_in, (cx_float*)csi_buffer_[offset_in_buffer], sizeof(cx_float) * BS_ANT_NUM * UE_NUM);
    cx_fmat mat_input(ptr_in, BS_ANT_NUM, UE_NUM, false);
    int offset_next_frame = ((frame_id + 1) % TASK_BUFFER_FRAME_NUM) * OFDM_DATA_NUM + sc_id;
    cx_float* ptr_out = (cx_float*)precoder(&mat_input, frame_id, sc_id, offset_next_frame);
    cx_fmat mat_output(ptr_out, UE_NUM, BS_ANT_NUM, false);
    pinv(mat_output, mat_input, 1e-1, "dc");

    // inform main thread
    Event_data pred_finish_event;
    pred_finish_event.event_type = EVENT_ZF;
    pred_finish_event.data = offset_next_frame;
    consumer_.handle(pred_finish_event);
}

DoUpZF::DoUpZF(Config* in_config, int in_tid,
    moodycamel::ConcurrentQueue<Event_data>& in_task_queue, Consumer& in_consumer,
    Table<complex_float>& in_csi_buffer,
    Table<complex_float>& in_precoder_buffer,
    Stats* in_stats_manager)
    : DoZF(in_config, in_tid, in_task_queue, in_consumer, in_csi_buffer,
          in_precoder_buffer, in_stats_manager)
{
}

void* DoUpZF::precoder(void*, int, int, int offset)
{
    void* ptr_out = (cx_float*)precoder_buffer_[offset];
    return (ptr_out);
}

DoDnZF::DoDnZF(Config* in_config, int in_tid,
    moodycamel::ConcurrentQueue<Event_data>& in_task_queue, Consumer& in_consumer,
    Table<complex_float>& in_csi_buffer, Table<complex_float>& in_recip_buffer,
    Table<complex_float>& in_precoder_buffer,
    Stats* in_stats_manager)
    : DoZF(in_config, in_tid, in_task_queue, in_consumer, in_csi_buffer,
          in_precoder_buffer, in_stats_manager)
    , recip_buffer_(in_recip_buffer)
{
}

void* DoDnZF::precoder(void* mat_input_ptr, int frame_id, int sc_id, int offset)
{
    cx_fmat& mat_input = *(cx_fmat*)mat_input_ptr;
    cx_float* ptr_out;
    int BS_ANT_NUM = config_->BS_ANT_NUM;
    if (config_->recipCalEn) {
        cx_float* calib = (cx_float*)(&recip_buffer_[frame_id][sc_id * BS_ANT_NUM]);
        cx_fvec vec_calib(calib, BS_ANT_NUM, false);
        cx_fmat mat_calib(BS_ANT_NUM, BS_ANT_NUM);
        mat_calib = diagmat(vec_calib);
        mat_input = mat_calib * mat_input;
    }
    ptr_out = (cx_float*)precoder_buffer_[offset];
    return (ptr_out);
}
