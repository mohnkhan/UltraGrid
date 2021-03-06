/*
 * Copyright (c) 2003-2004 University of Southern California
 * Copyright (c) 2005-2017 CESNET, z. s. p. o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *
 *      This product includes software developed by the University of Southern
 *      California Information Sciences Institute.
 *
 * 4. Neither the name of the University nor of the Institute may be used
 *    to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file    rtp/video_decoders.cpp
 * @author  Colin Perkins
 * @author  Ladan Gharai
 * @author  Martin Pulec <pulec@cesnet.cz>
 *
 * @ingroup video_rtp_decoder
 *
 * Normal workflow through threads is following:
 * 1. decode data from in context of receiving thread with function decode_frame(),
 *    it will decode it to framebuffer, passes it to FEC thread
 * 2. fec_thread() passes frame to decompress thread
 * 3. thread running decompress_thread(), displays the frame
 *
 * ### Uncompressed video (without FEC) ###
 * In step one, the decoder is a linedecoder and framebuffer is the display framebuffer
 *
 * ### Compressed video ###
 * Data is saved to decompress buffer. The decompression itself is done by decompress_thread().
 *
 * ### video with FEC ###
 * Data is saved to FEC buffer. Decoded with fec_thread().
 *
 * ### Encrypted video (without FEC) ###
 * Prior to decoding, packet is decompressed.
 *
 * ### Encrypted video (with FEC) ###
 * After FEC decoding, the whole block is decompressed.
 *
 * @todo
 * This code is very very messy, it needs to be rewritten.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "compat/platform_time.h"
#include "control_socket.h"
#include "crypto/openssl_decrypt.h"
#include "debug.h"
#include "host.h"
#include "lib_common.h"
#include "messaging.h"
#include "module.h"
#include "perf.h"
#include "rtp/fec.h"
#include "rtp/rtp.h"
#include "rtp/rtp_callback.h"
#include "rtp/pbuf.h"
#include "rtp/video_decoders.h"
#include "utils/synchronized_queue.h"
#include "utils/timed_message.h"
#include "utils/worker.h"
#include "video.h"
#include "video_decompress.h"
#include "video_display.h"

#include <condition_variable>
#ifdef RECONFIGURE_IN_FUTURE_THREAD
#include <future>
#endif
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <thread>

#ifdef HAVE_LIBAVCODEC_AVCODEC_H
#include <libavcodec/avcodec.h> // AV_INPUT_BUFFER_PADDING_SIZE
#endif

#define MOD_NAME "[video dec.] "

using namespace std;

struct state_video_decoder;

/**
 * Interlacing changing function protoype. The function should be able to change buffer
 * in place, that is when dst and src are the same.
 */
typedef void (*change_il_t)(char *dst, char *src, int linesize, int height, void **state);

// prototypes
static bool reconfigure_decoder(struct state_video_decoder *decoder,
                struct video_desc desc);
static int check_for_mode_change(struct state_video_decoder *decoder, uint32_t *hdr);
static void wait_for_framebuffer_swap(struct state_video_decoder *decoder);
static void *fec_thread(void *args);
static void *decompress_thread(void *args);
static void cleanup(struct state_video_decoder *decoder);
static void decoder_process_message(struct module *);

static int sum_map(map<int, int> const & m) {
        int ret = 0;
        for (map<int, int>::const_iterator it = m.begin();
                        it != m.end(); ++it) {
                ret += it->second;
        }
        return ret;
}

namespace {

#ifdef HAVE_LIBAVCODEC_AVCODEC_H
constexpr int PADDING = AV_INPUT_BUFFER_PADDING_SIZE;
#else
constexpr int PADDING = 0;
#endif
/**
 * Enumerates 2 possibilities how to decode arriving data.
 */
enum decoder_type_t {
        UNSET,
        LINE_DECODER,    ///< This is simple decoder, that decodes incoming data per line (pixelformats only).
        EXTERNAL_DECODER /**< real decompress, received buffer is opaque and has to be decompressed as
                          * a whole. */
};

/**
 * This structure holds data needed to use a linedecoder.
 */
struct line_decoder {
        int                  base_offset;  ///< from the beginning of buffer. Nonzero if decoding from mutiple tiles.
        double               src_bpp;      ///< Source pixelformat BPP (bytes)
        double               dst_bpp;      ///< Destination pixelformat BPP (bytes)
        int                  shifts[3];    ///< requested red,green and blue shift (in bits)
        decoder_t            decode_line;  ///< actual decoding function
        unsigned int         dst_linesize; ///< destination linesize
        unsigned int         dst_pitch;    ///< framebuffer pitch - it can be larger if SDL resolution is larger than data */
        unsigned int         src_linesize; ///< source linesize
};

struct reported_statistics_cumul {
        mutex             lock;
        unsigned long long int     received_bytes_total = 0;
        unsigned long long int     expected_bytes_total = 0;
        unsigned long int displayed = 0, dropped = 0, corrupted = 0, missing = 0;
        unsigned long int fec_ok = 0, fec_corrected = 0, fec_nok = 0;
        unsigned long long int     nano_per_frame_decompress = 0;
        unsigned long long int     nano_per_frame_error_correction = 0;
        unsigned long long int     nano_per_frame_expected = 0;
        unsigned long int     reported_frames = 0;
        void print() {
                char buff[256];
                int bytes = sprintf(buff, "Video dec stats (cumulative): %lu total / %lu disp / %lu "
                                "drop / %lu corr / %lu missing.",
                                displayed + dropped + missing,
                                displayed, dropped, corrupted,
                                missing);
                if (fec_ok + fec_nok + fec_corrected > 0)
                        sprintf(buff + bytes, " FEC noerr/OK/NOK: %ld/%ld/%ld\n", fec_ok, fec_corrected, fec_nok);
                else
                        sprintf(buff + bytes, "\n");
                log_msg(LOG_LEVEL_INFO, buff);
        }
};

// message definitions
struct frame_msg {
        inline frame_msg(struct control_state *c, struct reported_statistics_cumul &sr) : control(c), recv_frame(nullptr),
                                nofec_frame(nullptr),
                             received_pkts_cum(0), expected_pkts_cum(0),
                             stats(sr)
        {}
        inline ~frame_msg() {
                if (recv_frame) {
                        lock_guard<mutex> lk(stats.lock);
                        int received_bytes = 0;
                        for (unsigned int i = 0; i < recv_frame->tile_count; ++i) {
                                received_bytes += sum_map(pckt_list[i]);
                        }
                        int expected_bytes = vf_get_data_len(recv_frame);
                        if (recv_frame->fec_params.type != FEC_NONE) {
                                if (is_corrupted) {
                                        stats.fec_nok += 1;
                                } else {
                                        if (received_bytes == expected_bytes) {
                                                stats.fec_ok += 1;
                                        } else {
                                                stats.fec_corrected += 1;
                                        }
                                }
                        }
                        ostringstream oss;
                        oss << "RECV " << "bufferId " << buffer_num[0] << " expectedPackets " <<
                                expected_pkts_cum <<  " receivedPackets " << received_pkts_cum <<
                                // droppedPackets
                                " expectedBytes " << (stats.expected_bytes_total += expected_bytes) <<
                                " receivedBytes " << (stats.received_bytes_total += received_bytes) <<
                                " isCorrupted " << (stats.corrupted += (is_corrupted ? 1 : 0)) <<
                                " isDisplayed " << (stats.displayed += (is_displayed ? 1 : 0)) <<
                                " timestamp " << time_since_epoch_in_ms() <<
                                " nanoPerFrameDecompress " << (stats.nano_per_frame_decompress += nanoPerFrameDecompress) <<
                                " nanoPerFrameErrorCorrection " << (stats.nano_per_frame_error_correction += nanoPerFrameErrorCorrection) <<
                                " nanoPerFrameExpected " << (stats.nano_per_frame_expected += nanoPerFrameExpected) <<
                                " reportedFrames " << (stats.reported_frames += 1);
                        if ((stats.displayed + stats.dropped + stats.missing) % 600 == 599) {
                                stats.print();
                        }
                        if (control) {
                                control_report_stats(control, oss.str());
                        }
                }
                vf_free(recv_frame);
                vf_free(nofec_frame);
        }
        struct control_state *control;
        vector <uint32_t> buffer_num;
        struct video_frame *recv_frame; ///< received frame with FEC and/or compression
        struct video_frame *nofec_frame; ///< frame without FEC
        unique_ptr<map<int, int>[]> pckt_list;
        unsigned long long int received_pkts_cum, expected_pkts_cum;
        struct reported_statistics_cumul &stats;
        unsigned long long int nanoPerFrameDecompress = 0;
        unsigned long long int nanoPerFrameErrorCorrection = 0;
        unsigned long long int nanoPerFrameExpected = 0;
        bool is_displayed = false;
        bool is_corrupted = false;
};

struct main_msg_reconfigure {
        inline main_msg_reconfigure(struct video_desc d,
                        unique_ptr<frame_msg> &&f,
                        bool force = false) :
                desc(d),
                last_frame(move(f)),
                force(force) {}

        struct video_desc desc;
        unique_ptr<frame_msg> last_frame;
        bool force;
};
}

/**
 * @brief Decoder state
 */
struct state_video_decoder
{
        state_video_decoder(struct module *parent) {
                module_init_default(&mod);
                mod.cls = MODULE_CLASS_DECODER;
                mod.priv_data = this;
                mod.new_message = decoder_process_message;
                module_register(&mod, parent);
                control = (struct control_state *) get_module(get_root_module(parent), "control");
        }
        ~state_video_decoder() {
                module_done(&mod);
        }
        struct module mod;
        struct control_state *control = {};

        thread decompress_thread_id,
                  fec_thread_id;
        struct video_desc received_vid_desc = {}; ///< description of the network video
        struct video_desc display_desc = {};      ///< description of the mode that display is currently configured to

        struct video_frame *frame = NULL; ///< @todo rewrite this more reasonably

        struct display   *display = NULL; ///< assigned display device
        /// @{
        codec_t           native_codecs[VIDEO_CODEC_COUNT] = {}; ///< list of native codecs
        size_t            native_count = 0;  ///< count of @ref native_codecs
        enum interlacing_t *disp_supported_il = NULL; ///< display supported interlacing mode
        size_t            disp_supported_il_cnt = 0; ///< count of @ref disp_supported_il
        /// @}

        unsigned int      max_substreams = 0; ///< maximal number of expected substreams
        change_il_t       change_il = NULL;      ///< function to change interlacing, if needed. Otherwise NULL.
        vector<void *>    change_il_state;

        mutex lock;

        enum decoder_type_t decoder_type = {};  ///< how will the video data be decoded
        struct line_decoder *line_decoder = NULL; ///< if the video is uncompressed and only pixelformat change
                                           ///< is neeeded, use this structure
        struct state_decompress **decompress_state = NULL; ///< state of the decompress (for every substream)
        bool accepts_corrupted_frame = false;     ///< whether we should pass corrupted frame to decompress
        bool buffer_swapped = true; /**< variable indicating that display buffer
                              * has been processed and we can write to a new one */
        condition_variable buffer_swapped_cv; ///< condition variable associated with @ref buffer_swapped

        synchronized_queue<unique_ptr<frame_msg>, 1> decompress_queue;

        codec_t           out_codec = VIDEO_CODEC_NONE;
        int               pitch = 0;

        synchronized_queue<unique_ptr<frame_msg>, 1> fec_queue;

        enum video_mode   video_mode = {} ;  ///< video mode set for this decoder
        bool          merged_fb = false; ///< flag if the display device driver requires tiled video or not

        long int last_buffer_number = -1; ///< last received buffer ID
        timed_message<LOG_LEVEL_WARNING> slow_msg; ///< shows warning ony in certain interval

        synchronized_queue<main_msg_reconfigure *> msg_queue;

        const struct openssl_decrypt_info *dec_funcs = NULL; ///< decrypt state
        struct openssl_decrypt      *decrypt = NULL; ///< decrypt state

#ifdef RECONFIGURE_IN_FUTURE_THREAD
        std::future<bool> reconfiguration_future;
        bool             reconfiguration_in_progress = false;
#endif
        struct reported_statistics_cumul stats = {}; ///< stats to be reported through control socket
};

/**
 * This function blocks until video frame is displayed and decoder::frame
 * can be filled with new data. Until this point, the video frame is not considered
 * valid.
 */
static void wait_for_framebuffer_swap(struct state_video_decoder *decoder) {
        unique_lock<mutex> lk(decoder->lock);
        decoder->buffer_swapped_cv.wait(lk, [decoder]{return decoder->buffer_swapped;});
}

#define ENCRYPTED_ERR "Receiving encrypted video data but " \
        "no decryption key entered!\n"
#define NOT_ENCRYPTED_ERR "Receiving unencrypted video data " \
        "while expecting encrypted.\n"

static void *fec_thread(void *args) {
        struct state_video_decoder *decoder =
                (struct state_video_decoder *) args;

        fec *fec_state = NULL;
        struct fec_desc desc(FEC_NONE);

        while(1) {
                unique_ptr<frame_msg> data = decoder->fec_queue.pop();

                if (!data->recv_frame) { // poisoned
                        decoder->decompress_queue.push(move(data));
                        break; // exit from loop
                }

                struct video_frame *frame = decoder->frame;
                struct tile *tile = NULL;
                auto t0 = std::chrono::high_resolution_clock::now();

                if (data->recv_frame->fec_params.type != FEC_NONE) {
                        if(!fec_state || desc.k != data->recv_frame->fec_params.k ||
                                        desc.m != data->recv_frame->fec_params.m ||
                                        desc.c != data->recv_frame->fec_params.c ||
                                        desc.seed != data->recv_frame->fec_params.seed
                          ) {
                                delete fec_state;
                                desc = data->recv_frame->fec_params;
                                fec_state = fec::create_from_desc(desc);
                                if(fec_state == NULL) {
                                        log_msg(LOG_LEVEL_FATAL, "[decoder] Unable to initialize FEC.\n");
                                        exit_uv(1);
                                        goto cleanup;
                                }
                        }
                }

                data->nofec_frame = vf_alloc(data->recv_frame->tile_count);
                data->nofec_frame->ssrc = data->recv_frame->ssrc;

                if (data->recv_frame->fec_params.type != FEC_NONE) {
                        bool buffer_swapped = false;
                        for (int pos = 0; pos < get_video_mode_tiles_x(decoder->video_mode)
                                        * get_video_mode_tiles_y(decoder->video_mode); ++pos) {
                                char *fec_out_buffer = NULL;
                                int fec_out_len = 0;

                                fec_state->decode(data->recv_frame->tiles[pos].data,
                                                data->recv_frame->tiles[pos].data_len,
                                                &fec_out_buffer, &fec_out_len, data->pckt_list[pos]);

                                if (data->recv_frame->tiles[pos].data_len != (unsigned int) sum_map(data->pckt_list[pos])) {
                                        verbose_msg("Frame incomplete - substream %d, buffer %d: expected %u bytes, got %u.\n", pos,
                                                        (unsigned int) data->buffer_num[pos],
                                                        data->recv_frame->tiles[pos].data_len,
                                                        (unsigned int) sum_map(data->pckt_list[pos]));
                                }

                                if(fec_out_len == 0) {
                                        verbose_msg("[decoder] FEC: unable to reconstruct data.\n");
                                        data->is_corrupted = true;
                                        goto cleanup;
                                }

                                video_payload_hdr_t video_hdr;
                                memcpy(&video_hdr, fec_out_buffer,
                                                sizeof(video_payload_hdr_t));
                                fec_out_buffer += sizeof(video_payload_hdr_t);
                                fec_out_len -= sizeof(video_payload_hdr_t);

                                struct video_desc network_desc;
                                parse_video_hdr(video_hdr, &network_desc);
                                if (!video_desc_eq_excl_param(decoder->received_vid_desc,
                                                        network_desc, PARAM_TILE_COUNT)) {
                                        decoder->msg_queue.push(new main_msg_reconfigure(network_desc, move(data)));
                                        goto cleanup;
                                }

                                if(!frame) {
                                        goto cleanup;
                                }

                                if(decoder->decoder_type == EXTERNAL_DECODER) {
                                        data->nofec_frame->tiles[pos].data_len = fec_out_len;
                                        data->nofec_frame->tiles[pos].data = fec_out_buffer;
                                } else { // linedecoder
                                        if (!buffer_swapped) {
                                                buffer_swapped = true;
                                                wait_for_framebuffer_swap(decoder);
                                                unique_lock<mutex> lk(decoder->lock);
                                                decoder->buffer_swapped = false;
                                        }

                                        int divisor;

                                        if (!decoder->merged_fb) {
                                                divisor = decoder->max_substreams;
                                        } else {
                                                divisor = 1;
                                        }

                                        tile = vf_get_tile(frame, pos % divisor);

                                        struct line_decoder *line_decoder =
                                                &decoder->line_decoder[pos];

                                        int data_pos = 0;
                                        char *src = fec_out_buffer;
                                        char *dst = tile->data + line_decoder->base_offset;
                                        while(data_pos < (int) fec_out_len) {
                                                line_decoder->decode_line((unsigned char*)dst, (unsigned char *) src, line_decoder->src_linesize,
                                                                line_decoder->shifts[0],
                                                                line_decoder->shifts[1],
                                                                line_decoder->shifts[2]);
                                                src += line_decoder->src_linesize;
                                                dst += vc_get_linesize(tile->width ,frame->color_spec);
                                                data_pos += line_decoder->src_linesize;
                                        }
                                }
                        }
                } else { /* PT_VIDEO */
                        for(int i = 0; i < (int) decoder->max_substreams; ++i) {
                                data->nofec_frame->tiles[i].data_len = data->recv_frame->tiles[i].data_len;
                                data->nofec_frame->tiles[i].data = data->recv_frame->tiles[i].data;

                                if (data->recv_frame->tiles[i].data_len != (unsigned int) sum_map(data->pckt_list[i])) {
                                        verbose_msg("Frame incomplete - substream %d, buffer %d: expected %u bytes, got %u.%s\n", i,
                                                        (unsigned int) data->buffer_num[i],
                                                        data->recv_frame->tiles[i].data_len,
                                                        (unsigned int) sum_map(data->pckt_list[i]),
                                                        decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame ? " dropped.\n" : "");
                                        data->is_corrupted = true;
                                        if(decoder->decoder_type == EXTERNAL_DECODER && !decoder->accepts_corrupted_frame) {
                                                goto cleanup;
                                        }
                                }
                        }
                }

                data->nanoPerFrameErrorCorrection =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t0).count();

                decoder->decompress_queue.push(move(data));
cleanup:
                ;
        }

        delete fec_state;

        return NULL;
}

static bool blacklist_current_out_codec(struct state_video_decoder *decoder){
        if(decoder->out_codec == VIDEO_CODEC_NONE)
                return false;

        for(size_t i = 0; i < decoder->native_count; i++){
                if(decoder->native_codecs[i] == decoder->out_codec){
                        log_msg(LOG_LEVEL_DEBUG, "Blacklisting codec %s\n", get_codec_name(decoder->out_codec));
                        decoder->native_codecs[i] = VIDEO_CODEC_NONE;
                        decoder->out_codec = VIDEO_CODEC_NONE;
                }
        }

        return true;
}

struct decompress_data {
        struct state_video_decoder *decoder;
        int pos;
        struct video_frame *compressed;
        int buffer_num;
        decompress_status ret = DECODER_NO_FRAME;
};
static void *decompress_worker(void *data)
{
        auto d = (struct decompress_data *) data;
        struct state_video_decoder *decoder = d->decoder;
        int pos = d->pos;

        char *out;
        int x = d->pos % get_video_mode_tiles_x(decoder->video_mode),
            y = d->pos / get_video_mode_tiles_x(decoder->video_mode);
        int tile_width = decoder->received_vid_desc.width; // get_video_mode_tiles_x(decoder->video_mode);
        int tile_height = decoder->received_vid_desc.height; // get_video_mode_tiles_y(decoder->video_mode);
        if(decoder->merged_fb) {
                // TODO: OK when rendering directly to display FB, otherwise, do not reflect pitch (we use PP)
                out = vf_get_tile(decoder->frame, 0)->data + y * decoder->pitch * tile_height +
                        vc_get_linesize(tile_width, decoder->out_codec) * x;
        } else {
                out = vf_get_tile(decoder->frame, pos)->data;
        }
        if (!d->compressed->tiles[pos].data)
                return NULL;
        d->ret = decompress_frame(decoder->decompress_state[pos],
                        (unsigned char *) out,
                        (unsigned char *) d->compressed->tiles[pos].data,
                        d->compressed->tiles[pos].data_len,
                        d->buffer_num,
                        &decoder->frame->callbacks);
        return d;
}

static void *decompress_thread(void *args) {
        struct state_video_decoder *decoder =
                (struct state_video_decoder *) args;

        while(1) {
                unique_ptr<frame_msg> msg = decoder->decompress_queue.pop();

                if(!msg->recv_frame) { // poisoned
                        break;
                }

                auto t0 = std::chrono::high_resolution_clock::now();

                if(decoder->decoder_type == EXTERNAL_DECODER) {
                        int tile_count = get_video_mode_tiles_x(decoder->video_mode) *
                                        get_video_mode_tiles_y(decoder->video_mode);
                        vector<task_result_handle_t> handle(tile_count);
                        vector<decompress_data> data(tile_count);
                        for (int pos = 0; pos < tile_count; ++pos) {
                                data[pos].decoder = decoder;
                                data[pos].pos = pos;
                                data[pos].compressed = msg->nofec_frame;
                                data[pos].buffer_num = msg->buffer_num[pos];
                                if (tile_count > 1) {
                                        handle[pos] = task_run_async(decompress_worker, &data[pos]);
                                } else {
                                        decompress_worker(&data[pos]);
                                }
                        }
                        if (tile_count > 1) {
                                for (int pos = 0; pos < tile_count; ++pos) {
                                        wait_task(handle[pos]);
                                }
                        }
                        for (int pos = 0; pos < tile_count; ++pos) {
                                if (data[pos].ret != DECODER_GOT_FRAME){
                                        if (data[pos].ret == DECODER_CANT_DECODE){
                                                if(blacklist_current_out_codec(decoder))
                                                        decoder->msg_queue.push(new main_msg_reconfigure(decoder->received_vid_desc, nullptr, true));
                                        }
                                        goto skip_frame;
                                }
                        }
                } else {
                        if (decoder->frame->decoder_overrides_data_len == TRUE) {
                                for (unsigned int i = 0; i < decoder->frame->tile_count; ++i) {
                                        decoder->frame->tiles[i].data_len = msg->nofec_frame->tiles[i].data_len;
                                }
                        }
                }

                msg->nanoPerFrameDecompress =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - t0).count();

                if(decoder->change_il) {
                        for(unsigned int i = 0; i < decoder->frame->tile_count; ++i) {
                                struct tile *tile = vf_get_tile(decoder->frame, i);
                                decoder->change_il(tile->data, tile->data, vc_get_linesize(tile->width,
                                                        decoder->out_codec), tile->height, &decoder->change_il_state[i]);
                        }
                }

                {
                        int putf_flags = PUTF_NONBLOCK;

                        if (is_codec_interframe(decoder->received_vid_desc.color_spec)) {
                                putf_flags = PUTF_NONBLOCK;
                        }

                        auto drop_policy = commandline_params.find("drop-policy");
                        if (drop_policy != commandline_params.end()) {
                                if (drop_policy->second == "nonblock") {
                                        putf_flags = PUTF_NONBLOCK;
                                } else if (drop_policy->second == "blocking") {
                                        putf_flags = PUTF_BLOCKING;
                                } else {
                                        LOG(LOG_LEVEL_WARNING) << "Wrong drop policy "
                                                << drop_policy->second << "!\n";
                                }
                        }

                        decoder->frame->ssrc = msg->nofec_frame->ssrc;
                        int ret = display_put_frame(decoder->display,
                                        decoder->frame, putf_flags);
                        if (ret == 0) {
                                msg->is_displayed = true;
                        }
                        decoder->frame = display_get_frame(decoder->display);
                }

skip_frame:
                {
                        unique_lock<mutex> lk(decoder->lock);
                        // we have put the video frame and requested another one which is
                        // writable so on
                        decoder->buffer_swapped = true;
                        lk.unlock();
                        decoder->buffer_swapped_cv.notify_one();
                }
        }

        return NULL;
}

static void decoder_set_video_mode(struct state_video_decoder *decoder, enum video_mode video_mode)
{
        decoder->video_mode = video_mode;
        decoder->max_substreams = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);
}

/**
 * @brief Initializes video decompress state.
 * @param video_mode  video_mode expected to be received from network
 * @param display     Video display that is supposed to be controlled from decoder.
 *                    display_get_frame(), display_put_frame(), display_get_propert()
 *                    and display_reconfigure() functions may be used. If set to NULL,
 *                    no decoding will take place.
 * @param encryption  Encryption config string. Currently, this is a passphrase to be
 *                    used. This may change eventually.
 * @return Newly created decoder state. If an error occured, returns NULL.
 * @ingroup video_rtp_decoder
 */
struct state_video_decoder *video_decoder_init(struct module *parent,
                enum video_mode video_mode,
                struct display *display, const char *encryption)
{
        struct state_video_decoder *s;

        s = new state_video_decoder(parent);

        if (encryption) {
                s->dec_funcs = static_cast<const struct openssl_decrypt_info *>(load_library("openssl_decrypt",
                                        LIBRARY_CLASS_UNDEFINED, OPENSSL_DECRYPT_ABI_VERSION));
                if (!s->dec_funcs) {
                                log_msg(LOG_LEVEL_FATAL, "UltraGrid was build without OpenSSL support!\n");
                                delete s;
                                return NULL;
                }
                if (s->dec_funcs->init(&s->decrypt, encryption) != 0) {
                        log_msg(LOG_LEVEL_FATAL, "Unable to create decompress!\n");
                        delete s;
                        return NULL;
                }
        }

        decoder_set_video_mode(s, video_mode);

        if(!video_decoder_register_display(s, display)) {
                delete s;
                return NULL;
        }

        return s;
}

/**
 * @brief starts decompress and ldmg threads
 *
 * Called from video_decoder_register_display(). It is also called after
 * video_decoder_stop_threads() in reconfiguration.
 *
 * @invariant
 * decoder->display != NULL
 */
static void video_decoder_start_threads(struct state_video_decoder *decoder)
{
        assert(decoder->display); // we want to run threads only if decoder is active

        decoder->decompress_thread_id = thread(decompress_thread, decoder);
        decoder->fec_thread_id = thread(fec_thread, decoder);
}

/**
 * @brief This function stops running threads.
 *
 * @invariant
 * decoder->display != NULL
 */
static void video_decoder_stop_threads(struct state_video_decoder *decoder)
{
        assert(decoder->display);

        unique_ptr<frame_msg> msg(new frame_msg(decoder->control, decoder->stats));
        decoder->fec_queue.push(move(msg));

        decoder->fec_thread_id.join();
        decoder->decompress_thread_id.join();
}

/**
 * @brief Registers video display to be used for displaying decoded video frames.
 *
 * No display should be managed by this decoder when this function is called.
 *
 * @see decoder_remove_display
 *
 * @param decoder decoder state
 * @param display new display to be registered
 * @return        whether registration was successful
 */
bool video_decoder_register_display(struct state_video_decoder *decoder, struct display *display)
{
        assert(display != NULL);
        assert(decoder->display == NULL);

        int ret;

        decoder->display = display;

        size_t len = sizeof decoder->native_codecs;
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_CODECS, decoder->native_codecs, &len);
        decoder->native_count = len / sizeof(codec_t);
        if (!ret) {
                log_msg(LOG_LEVEL_ERROR, "Failed to query codecs from video display.\n");
                decoder->native_count = 0;
        }

        free(decoder->disp_supported_il);
        decoder->disp_supported_il_cnt = 20 * sizeof(enum interlacing_t);
        decoder->disp_supported_il = (enum interlacing_t*) calloc(decoder->disp_supported_il_cnt,
                        sizeof(enum interlacing_t));
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_SUPPORTED_IL_MODES, decoder->disp_supported_il, &decoder->disp_supported_il_cnt);
        if(ret) {
                decoder->disp_supported_il_cnt /= sizeof(enum interlacing_t);
        } else {
                enum interlacing_t tmp[] = { PROGRESSIVE, INTERLACED_MERGED, SEGMENTED_FRAME}; /* default if not said othervise */
                memcpy(decoder->disp_supported_il, tmp, sizeof(tmp));
                decoder->disp_supported_il_cnt = sizeof(tmp) / sizeof(enum interlacing_t);
        }

        video_decoder_start_threads(decoder);

        return true;
}

/**
 * @brief This removes display from current decoder.
 *
 * From now on, no video frames will be decoded with current decoder.
 * @see decoder_register_display - the counterpart of this functon
 *
 * @param decoder decoder from which will the decoder be removed
 */
void video_decoder_remove_display(struct state_video_decoder *decoder)
{
        if (decoder->display) {
                video_decoder_stop_threads(decoder);
                control_report_event(decoder->control, string("RECV stream ended"));
                if (decoder->frame) {
                        display_put_frame(decoder->display, decoder->frame, PUTF_DISCARD);
                        decoder->frame = NULL;
                }
                decoder->display = NULL;
                memset(&decoder->display_desc, 0, sizeof(decoder->display_desc));
        }
}

static void cleanup(struct state_video_decoder *decoder)
{
        decoder->decoder_type = UNSET;
        if(decoder->decompress_state) {
                for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                        decompress_done(decoder->decompress_state[i]);
                }
                free(decoder->decompress_state);
                decoder->decompress_state = NULL;
        }
        if(decoder->line_decoder) {
                free(decoder->line_decoder);
                decoder->line_decoder = NULL;
        }

        for (auto && item : decoder->change_il_state) {
                free(item);
        }
        decoder->change_il_state.resize(0);
}

/**
 * @brief Destroys decoder created with decoder_init()
 * @param decoder decoder to be destroyed. If NULL, no action is performed.
 */
void video_decoder_destroy(struct state_video_decoder *decoder)
{
        if(!decoder)
                return;

        if (decoder->dec_funcs) {
                decoder->dec_funcs->destroy(decoder->decrypt);
        }

        video_decoder_remove_display(decoder);

        cleanup(decoder);

        free(decoder->disp_supported_il);

        decoder->stats.print();

        delete decoder;
}

/**
 * This function selects, according to given video description, appropriate
 *
 * @param[in]  decoder     decoder to be taken parameters from (video mode, native codecs etc.)
 * @param[in]  desc        incoming video description
 * @param[out] decode_line If chosen decoder is a linedecoder, this variable contains the
 *                         decoding function.
 * @return                 Output codec, if no decoding function found, -1 is returned.
 */
static codec_t choose_codec_and_decoder(struct state_video_decoder *decoder, struct video_desc desc,
                                decoder_t *decode_line)
{
        codec_t out_codec = VIDEO_CODEC_NONE;
        *decode_line = NULL;

        size_t native;
        /* first check if the codec is natively supported */
        for(native = 0u; native < decoder->native_count; ++native)
        {
                out_codec = decoder->native_codecs[native];
                if(desc.color_spec == out_codec) {
                        if((out_codec == DXT1 || out_codec == DXT1_YUV ||
                                        out_codec == DXT5)
                                        && decoder->video_mode != VIDEO_NORMAL)
                                continue; /* it is a exception, see NOTES #1 */

                        *decode_line = (decoder_t) memcpy;
                        decoder->decoder_type = LINE_DECODER;

                        if(desc.color_spec == RGBA || /* another exception - we may change shifts */
                                        desc.color_spec == RGB) {
                                *decode_line = desc.color_spec == RGBA ?
                                        vc_copylineRGBA : vc_copylineRGB;
                        }

                        goto after_linedecoder_lookup;
                }
        }
        /* otherwise if we have line decoder */
        for(native = 0; native < decoder->native_count; ++native)
        {
                decoder_t decode;
                if ((decode = get_decoder_from_to(desc.color_spec, decoder->native_codecs[native], false)) != NULL) {
                        *decode_line = decode;

                        decoder->decoder_type = LINE_DECODER;
                        out_codec = decoder->native_codecs[native];
                        goto after_linedecoder_lookup;

                }
        }
        /* the same, but include also slow decoders */
        for(native = 0; native < decoder->native_count; ++native)
        {
                decoder_t decode;
                if ((decode = get_decoder_from_to(desc.color_spec, decoder->native_codecs[native], true)) != NULL) {
                        *decode_line = decode;

                        decoder->decoder_type = LINE_DECODER;
                        out_codec = decoder->native_codecs[native];
                        goto after_linedecoder_lookup;

                }
        }

after_linedecoder_lookup:

        /* we didn't find line decoder. So try now regular (aka DXT) decoder */
        if(*decode_line == NULL) {
                for(native = 0; native < decoder->native_count; ++native)
                {
                        out_codec = decoder->native_codecs[native];
                        decoder->decompress_state = (struct state_decompress **)
                                calloc(decoder->max_substreams, sizeof(struct state_decompress *));
                        if (decompress_init_multi(desc.color_spec, decoder->native_codecs[native],
                                                decoder->decompress_state,
                                                decoder->max_substreams)) {
                                int res = 0, ret;
                                size_t size = sizeof(res);
                                ret = decompress_get_property(decoder->decompress_state[0],
                                                DECOMPRESS_PROPERTY_ACCEPTS_CORRUPTED_FRAME,
                                                &res,
                                                &size);
                                decoder->accepts_corrupted_frame = ret && res;

                                decoder->decoder_type = EXTERNAL_DECODER;
                                goto after_decoder_lookup;
                        } else {
                                free(decoder->decompress_state);
                                decoder->decompress_state = 0;
                        }
                }
        }
after_decoder_lookup:

        if(decoder->decoder_type == UNSET) {
                log_msg(LOG_LEVEL_ERROR, "Unable to find decoder for input codec \"%s\"!!!\n", get_codec_name(desc.color_spec));
                return VIDEO_CODEC_NONE;
        }

        return out_codec;
}

/**
 * This function finds interlacing mode changing function.
 *
 * @param[in]  in_il       input_interlacing
 * @param[in]  supported   list of supported output interlacing modes
 * @param[in]  il_out_cnt  count of supported items
 * @param[out] out_il      selected output interlacing
 * @return                 selected interlacing changing function, NULL if not needed or not found
 */
static change_il_t select_il_func(enum interlacing_t in_il, enum interlacing_t *supported,
                int il_out_cnt, /*out*/ enum interlacing_t *out_il)
{
        struct transcode_t { enum interlacing_t in; enum interlacing_t out; change_il_t func; };

        struct transcode_t transcode[] = {
                {LOWER_FIELD_FIRST, INTERLACED_MERGED, il_lower_to_merged},
                {UPPER_FIELD_FIRST, INTERLACED_MERGED, il_upper_to_merged},
                {INTERLACED_MERGED, UPPER_FIELD_FIRST, il_merged_to_upper}
        };

        int i;
        /* first try to check if it can be nativelly displayed */
        for (i = 0; i < il_out_cnt; ++i) {
                if(in_il == supported[i]) {
                        *out_il = in_il;
                        return NULL;
                }
        }

        for (i = 0; i < il_out_cnt; ++i) {
                size_t j;
                for (j = 0; j < sizeof(transcode) / sizeof(struct transcode_t); ++j) {
                        if(in_il == transcode[j].in && supported[i] == transcode[j].out) {
                                *out_il = transcode[j].out;
                                return transcode[j].func;
                        }
                }
        }

        log_msg(LOG_LEVEL_WARNING, "[Warning] Cannot find transition between incoming and display "
                        "interlacing modes!\n");
        return NULL;
}

/**
 * Reconfigures decoder if network received video data format has changed.
 *
 * @param decoder       the video decoder state
 * @param desc          new video description to be reconfigured to
 * @return              boolean value if reconfiguration was successful
 *
 * @invariant
 * decoder->display != NULL
 */
static bool reconfigure_decoder(struct state_video_decoder *decoder,
                struct video_desc desc)
{
        codec_t out_codec;
        decoder_t decode_line;
        enum interlacing_t display_il = PROGRESSIVE;
        //struct video_frame *frame;
        int display_requested_pitch;
        int display_requested_rgb_shift[3];

        // this code forces flushing the pipelined data
        video_decoder_stop_threads(decoder);
        if (decoder->frame)
                display_put_frame(decoder->display, decoder->frame, PUTF_DISCARD);
        decoder->frame = NULL;
        video_decoder_start_threads(decoder);

        assert(decoder->native_codecs != NULL);

        cleanup(decoder);

        desc.tile_count = get_video_mode_tiles_x(decoder->video_mode)
                        * get_video_mode_tiles_y(decoder->video_mode);

        out_codec = choose_codec_and_decoder(decoder, desc, &decode_line);
        if(out_codec == VIDEO_CODEC_NONE)
                return false;
        else
                decoder->out_codec = out_codec;
        struct video_desc display_desc = desc;

        int display_mode;
        size_t len = sizeof(int);
        int ret;

        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_VIDEO_MODE,
                        &display_mode, &len);
        if(!ret) {
                debug_msg("Failed to get video display mode.\n");
                display_mode = DISPLAY_PROPERTY_VIDEO_MERGED;
        }

        if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED) {
                display_desc.width *= get_video_mode_tiles_x(decoder->video_mode);
                display_desc.height *= get_video_mode_tiles_y(decoder->video_mode);
                display_desc.tile_count = 1;
        }

        decoder->change_il = select_il_func(desc.interlacing, decoder->disp_supported_il,
                        decoder->disp_supported_il_cnt, &display_il);
        decoder->change_il_state.resize(decoder->max_substreams);

        display_desc.color_spec = out_codec;
        display_desc.interlacing = display_il;

        if(!video_desc_eq(decoder->display_desc, display_desc))
        {
                int ret;
                /* reconfigure VO and give it opportunity to pass us pitch */
                ret = display_reconfigure(decoder->display, display_desc, decoder->video_mode);
                if(!ret) {
                        LOG(LOG_LEVEL_ERROR) << MOD_NAME << "Unable to reconfigure display to "
                                << display_desc << "\n";
                        return false;
                }
                LOG(LOG_LEVEL_VERBOSE) << MOD_NAME << "Sucessfully reconfigured display to "
                        << display_desc << "\n";
                decoder->display_desc = display_desc;
        }

        len = sizeof(display_requested_rgb_shift);
        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_RGB_SHIFT,
                        &display_requested_rgb_shift, &len);
        if(!ret) {
                debug_msg("Failed to get r,g,b shift property from video driver.\n");
                int rgb_shift[3] = {0, 8, 16};
                memcpy(&display_requested_rgb_shift, rgb_shift, sizeof(rgb_shift));
        }

        ret = display_get_property(decoder->display, DISPLAY_PROPERTY_BUF_PITCH,
                        &display_requested_pitch, &len);
        if(!ret) {
                debug_msg("Failed to get pitch from video driver.\n");
                display_requested_pitch = PITCH_DEFAULT;
        }

        int linewidth;
        if (display_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                linewidth = desc.width;
        } else {
                linewidth = desc.width * get_video_mode_tiles_x(decoder->video_mode);
        }

        if(display_requested_pitch == PITCH_DEFAULT)
                decoder->pitch = vc_get_linesize(linewidth, out_codec);
        else
                decoder->pitch = display_requested_pitch;


        int src_x_tiles = get_video_mode_tiles_x(decoder->video_mode);
        int src_y_tiles = get_video_mode_tiles_y(decoder->video_mode);

        if(decoder->decoder_type == LINE_DECODER) {
                decoder->line_decoder = (struct line_decoder *) malloc(src_x_tiles * src_y_tiles *
                                        sizeof(struct line_decoder));
                if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED && decoder->video_mode == VIDEO_NORMAL) {
                        struct line_decoder *out = &decoder->line_decoder[0];
                        out->base_offset = 0;
                        out->src_bpp = get_bpp(desc.color_spec);
                        out->dst_bpp = get_bpp(out_codec);
                        memcpy(out->shifts, display_requested_rgb_shift, 3 * sizeof(int));

                        out->decode_line = decode_line;
                        out->dst_pitch = decoder->pitch;
                        out->src_linesize = vc_get_linesize(desc.width, desc.color_spec);
                        out->dst_linesize = vc_get_linesize(desc.width, out_codec);
                        decoder->merged_fb = true;
                } else if(display_mode == DISPLAY_PROPERTY_VIDEO_MERGED
                                && decoder->video_mode != VIDEO_NORMAL) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x +
                                                        src_x_tiles * y];
                                        out->base_offset = y * (desc.height)
                                                        * decoder->pitch +
                                                        vc_get_linesize(x * desc.width, out_codec);

                                        out->src_bpp = get_bpp(desc.color_spec);
                                        out->dst_bpp = get_bpp(out_codec);
                                        memcpy(out->shifts, display_requested_rgb_shift,
                                                        3 * sizeof(int));

                                        out->decode_line = decode_line;

                                        out->dst_pitch = decoder->pitch;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, desc.color_spec);
                                        out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = true;
                } else if (display_mode == DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES) {
                        int x, y;
                        for(x = 0; x < src_x_tiles; ++x) {
                                for(y = 0; y < src_y_tiles; ++y) {
                                        struct line_decoder *out = &decoder->line_decoder[x +
                                                        src_x_tiles * y];
                                        out->base_offset = 0;
                                        out->src_bpp = get_bpp(desc.color_spec);
                                        out->dst_bpp = get_bpp(out_codec);
                                        memcpy(out->shifts, display_requested_rgb_shift,
                                                        3 * sizeof(int));

                                        out->decode_line = decode_line;
                                        out->src_linesize =
                                                vc_get_linesize(desc.width, desc.color_spec);
                                        out->dst_pitch =
                                                out->dst_linesize =
                                                vc_get_linesize(desc.width, out_codec);
                                }
                        }
                        decoder->merged_fb = false;
                }
        } else if (decoder->decoder_type == EXTERNAL_DECODER) {
                int buf_size;

                for(unsigned int i = 0; i < decoder->max_substreams; ++i) {
                        buf_size = decompress_reconfigure(decoder->decompress_state[i], desc,
                                        display_requested_rgb_shift[0],
                                        display_requested_rgb_shift[1],
                                        display_requested_rgb_shift[2],
                                        decoder->pitch,
                                        out_codec);
                        if(!buf_size) {
                                return false;
                        }
                }
                decoder->merged_fb = display_mode != DISPLAY_PROPERTY_VIDEO_SEPARATE_TILES;
        }

        // Pass metadata to receiver thread (it can tweak parameters)
        struct msg_receiver *msg = (struct msg_receiver *)
                new_message(sizeof(struct msg_receiver));
        msg->type = RECEIVER_MSG_VIDEO_PROP_CHANGED;
        msg->new_desc = decoder->received_vid_desc;
        struct response *resp =
                send_message_to_receiver(decoder->mod.parent, (struct message *) msg);
        free_response(resp);

        decoder->frame = display_get_frame(decoder->display);

        return true;
}

bool parse_video_hdr(uint32_t *hdr, struct video_desc *desc)
{
        uint32_t tmp;
        int fps_pt, fpsd, fd, fi;

        tmp = ntohl(hdr[0]);
        desc->tile_count = (tmp >> 22) + 1; // a bit hacky - assuming this packet is from last substream

        desc->width = ntohl(hdr[3]) >> 16;
        desc->height = ntohl(hdr[3]) & 0xffff;
        desc->color_spec = get_codec_from_fcc(hdr[4]);
        if(desc->color_spec == VIDEO_CODEC_NONE) {
                log_msg(LOG_LEVEL_ERROR, "Unknown FourCC \"%4s\"!\n", (char *) &hdr[4]);
                return false;
        }

        tmp = ntohl(hdr[5]);
        desc->interlacing = (enum interlacing_t) (tmp >> 29);
        fps_pt = (tmp >> 19) & 0x3ff;
        fpsd = (tmp >> 15) & 0xf;
        fd = (tmp >> 14) & 0x1;
        fi = (tmp >> 13) & 0x1;

        desc->fps = compute_fps(fps_pt, fpsd, fd, fi);

        return true;
}

static int reconfigure_if_needed(struct state_video_decoder *decoder,
                struct video_desc network_desc,
                bool force = false)
{
        bool desc_changed = !video_desc_eq_excl_param(decoder->received_vid_desc, network_desc, PARAM_TILE_COUNT);
        if(!desc_changed && !force)
                return FALSE;

        if (desc_changed) {
                LOG(LOG_LEVEL_NOTICE) << "[video dec.] New incoming video format detected: " << network_desc << endl;
                control_report_event(decoder->control, string("RECV received video changed - ") +
                                (string) network_desc);

                decoder->received_vid_desc = network_desc;
        }

        if(force){
                log_msg(LOG_LEVEL_VERBOSE, "forced reconf\n");
        }

#ifdef RECONFIGURE_IN_FUTURE_THREAD
        decoder->reconfiguration_in_progress = true;
        decoder->reconfiguration_future = std::async(std::launch::async,
                        [decoder](){ return reconfigure_decoder(decoder, decoder->received_vid_desc); });
#else
        int ret = reconfigure_decoder(decoder, decoder->received_vid_desc);
        if (!ret) {
                log_msg(LOG_LEVEL_ERROR, "[video dec.] Reconfiguration failed!!!\n");
                decoder->frame = NULL;
        }
#endif
        return TRUE;
}
/**
 * Checks if network format has changed.
 *
 * @param decoder decoder state
 * @param hdr     raw RTP payload header
 *
 * @return TRUE if format changed (and reconfiguration was successful), FALSE if not
 */
static int check_for_mode_change(struct state_video_decoder *decoder,
                uint32_t *hdr)
{
        struct video_desc network_desc;

        parse_video_hdr(hdr, &network_desc);
        return reconfigure_if_needed(decoder, network_desc);
}

#define ERROR_GOTO_CLEANUP ret = FALSE; goto cleanup;
#define max(a, b)       (((a) > (b))? (a): (b))

/**
 * @brief Decodes a participant buffer representing one video frame.
 * @param cdata        PBUF buffer
 * @param decoder_data @ref vcodec_state containing decoder state and some additional data
 * @retval TRUE        if decoding was successful.
 *                     It stil doesn't mean that the frame will be correctly displayed,
 *                     decoding may fail in some subsequent (asynchronous) steps.
 * @retval FALSE       if decoding failed
 */
int decode_video_frame(struct coded_data *cdata, void *decoder_data, struct pbuf_stats *stats)
{
        struct vcodec_state *pbuf_data = (struct vcodec_state *) decoder_data;
        struct state_video_decoder *decoder = pbuf_data->decoder;

        int ret = TRUE;
        rtp_packet *pckt = NULL;
        int prints=0;
        int max_substreams = decoder->max_substreams;
        uint32_t ssrc;
        unsigned int frame_size = 0;

        vector<uint32_t> buffer_num(max_substreams);
        // the following is just FEC related optimalization - normally we fill up
        // allocated buffers when we have compressed data. But in case of FEC, there
        // is just the FEC buffer present, so we point to it instead to copying
        struct video_frame *frame = vf_alloc(max_substreams);
        frame->callbacks.data_deleter = vf_data_deleter;
        unique_ptr<map<int, int>[]> pckt_list(new map<int, int>[max_substreams]);

        int k = 0, m = 0, c = 0, seed = 0; // LDGM
        int buffer_number, buffer_length;

        int pt;
        bool buffer_swapped = false;

        perf_record(UVP_DECODEFRAME, cdata);

        // We have no framebuffer assigned, exitting
        if(!decoder->display) {
                vf_free(frame);
                return FALSE;
        }

#ifdef RECONFIGURE_IN_FUTURE_THREAD
        // check if we are not in the middle of reconfiguration
        if (decoder->reconfiguration_in_progress) {
                std::future_status status =
                        decoder->reconfiguration_future.wait_until(std::chrono::system_clock::now());
                if (status == std::future_status::ready) {
                        bool ret = decoder->reconfiguration_future.get();
                        if (ret) {
                                decoder->frame = display_get_frame(decoder->display);
                        } else {
                                log_msg(LOG_LEVEL_ERROR, "Decoder reconfiguration failed!!!\n");
                                decoder->frame = NULL;
                        }
                        decoder->reconfiguration_in_progress = false;
                } else {
                        // skip the frame if we are not yet reconfigured
                        vf_free(frame);
                        return FALSE;
                }
        }
#endif

        main_msg_reconfigure *msg_reconf;
        while ((msg_reconf = decoder->msg_queue.pop(true /* nonblock */))) {
                if (reconfigure_if_needed(decoder, msg_reconf->desc, msg_reconf->force)) {
#ifdef RECONFIGURE_IN_FUTURE_THREAD
                        vf_free(frame);
                        return FALSE;
#endif
                }
                if (msg_reconf->last_frame) {
                        decoder->fec_queue.push(move(msg_reconf->last_frame));
                }
                delete msg_reconf;
        }

        while (cdata != NULL) {
                uint32_t tmp;
                uint32_t *hdr;
                int len;
                uint32_t offset;
                unsigned char *source;
                char *data;
                uint32_t data_pos;
                uint32_t substream;
                pckt = cdata->data;
                enum openssl_mode crypto_mode;

                pt = pckt->pt;
                hdr = (uint32_t *)(void *) pckt->data;
                data_pos = ntohl(hdr[1]);
                tmp = ntohl(hdr[0]);
                substream = tmp >> 22;
                buffer_number = tmp & 0x3fffff;
                buffer_length = ntohl(hdr[2]);
                ssrc = pckt->ssrc;

                if (PT_VIDEO_HAS_FEC(pt)) {
                        tmp = ntohl(hdr[3]);
                        k = tmp >> 19;
                        m = 0x1fff & (tmp >> 6);
                        c = 0x3f & tmp;
                        seed = ntohl(hdr[4]);
                }

                if (PT_VIDEO_IS_ENCRYPTED(pt)) {
                        if(!decoder->decrypt) {
                                log_msg(LOG_LEVEL_ERROR, ENCRYPTED_ERR);
                                ERROR_GOTO_CLEANUP
                        }
                } else {
                        if(decoder->decrypt) {
                                log_msg(LOG_LEVEL_ERROR, NOT_ENCRYPTED_ERR);
                                ERROR_GOTO_CLEANUP
                        }
                }

                switch (pt) {
                case PT_VIDEO:
                        len = pckt->data_len - sizeof(video_payload_hdr_t);
                        data = (char *) hdr + sizeof(video_payload_hdr_t);
                        break;
                case PT_VIDEO_RS:
                case PT_VIDEO_LDGM:
                        len = pckt->data_len - sizeof(fec_video_payload_hdr_t);
                        data = (char *) hdr + sizeof(fec_video_payload_hdr_t);
                        break;
                case PT_ENCRYPT_VIDEO:
                case PT_ENCRYPT_VIDEO_LDGM:
                case PT_ENCRYPT_VIDEO_RS:
                        {
				size_t media_hdr_len = pt == PT_ENCRYPT_VIDEO ? sizeof(video_payload_hdr_t) : sizeof(fec_video_payload_hdr_t);
                                len = pckt->data_len - sizeof(crypto_payload_hdr_t) - media_hdr_len;
				data = (char *) hdr + sizeof(crypto_payload_hdr_t) + media_hdr_len;
                                uint32_t crypto_hdr = ntohl(*(uint32_t *)((char *) hdr + media_hdr_len));
                                crypto_mode = (enum openssl_mode) (crypto_hdr >> 24);
				if (crypto_mode == MODE_AES128_NONE || crypto_mode > MODE_AES128_MAX) {
					log_msg(LOG_LEVEL_WARNING, "Unknown cipher mode: %d\n", (int) crypto_mode);
					ret = FALSE;
					goto cleanup;
				}
                        }
                        break;
                default:
                        log_msg(LOG_LEVEL_WARNING, "[decoder] Unknown packet type: %d.\n", pckt->pt);
                        ret = FALSE;
                        goto cleanup;
                }

                if ((int) substream >= max_substreams) {
                        log_msg(LOG_LEVEL_WARNING, "[decoder] received substream ID %d. Expecting at most %d substreams. Did you set -M option?\n",
                                        substream, max_substreams);
                        // the guess is valid - we start with highest substream number (anytime - since it holds a m-bit)
                        // in next iterations, index is valid
                        enum video_mode video_mode =
                                guess_video_mode(substream + 1);
                        if (video_mode != VIDEO_UNKNOWN) {
                                log_msg(LOG_LEVEL_NOTICE, "[decoder] Guessing mode: ");
                                decoder_set_video_mode(decoder, video_mode);
                                decoder->received_vid_desc.width = 0; // just for sure, that we reconfigure in next iteration
                                log_msg(LOG_LEVEL_NOTICE, "%s. Check if it is correct.\n", get_video_mode_description(decoder->video_mode));
                        } else {
                                log_msg(LOG_LEVEL_FATAL, "[decoder] Unknown video mode!\n");
                                exit_uv(1);
                        }
                        // we need skip this frame (variables are illegal in this iteration
                        // and in case that we got unrecognized number of substreams - exit
                        ret = FALSE;
                        goto cleanup;
                }

                char plaintext[len]; // will be actually shorter
                if (PT_VIDEO_IS_ENCRYPTED(pt)) {
                        int data_len;

                        if((data_len = decoder->dec_funcs->decrypt(decoder->decrypt,
                                        data, len,
                                        (char *) hdr, pt == PT_ENCRYPT_VIDEO ?
                                        sizeof(video_payload_hdr_t) : sizeof(fec_video_payload_hdr_t),
                                        plaintext, crypto_mode)) == 0) {
                                log_msg(LOG_LEVEL_VERBOSE, "Warning: Packet dropped AES - wrong CRC!\n");
                                goto next_packet;
                        }
                        data = (char *) plaintext;
                        len = data_len;
                }

                if (!PT_VIDEO_HAS_FEC(pt))
                {
                        /* Critical section
                         * each thread *MUST* wait here if this condition is true
                         */
                        if (check_for_mode_change(decoder, hdr)) {
#ifdef RECONFIGURE_IN_FUTURE_THREAD
                                vf_free(frame);
                                return FALSE;
#endif
                        }

                        // hereafter, display framebuffer can be used, so we
                        // check if we got it
                        if (decoder->frame == NULL) {
                                vf_free(frame);
                                return FALSE;
                        }
                }

                buffer_num[substream] = buffer_number;
                frame->tiles[substream].data_len = buffer_length;
                pckt_list[substream][data_pos] = len;

                if ((pt == PT_VIDEO || pt == PT_ENCRYPT_VIDEO) && decoder->decoder_type == LINE_DECODER) {
                        struct tile *tile = NULL;
                        if(!buffer_swapped) {
                                wait_for_framebuffer_swap(decoder);
                                buffer_swapped = true;
                                unique_lock<mutex> lk(decoder->lock);
                                decoder->buffer_swapped = false;
                        }

                        if (!decoder->merged_fb) {
                                tile = vf_get_tile(decoder->frame, substream);
                        } else {
                                tile = vf_get_tile(decoder->frame, 0);
                        }

                        struct line_decoder *line_decoder =
                                &decoder->line_decoder[substream];

                        /* End of critical section */

                        /* MAGIC, don't touch it, you definitely break it
                         *  *source* is data from network, *destination* is frame buffer
                         */

                        /* compute Y pos in source frame and convert it to
                         * byte offset in the destination frame
                         */
                        int y = (data_pos / line_decoder->src_linesize) * line_decoder->dst_pitch;

                        /* compute X pos in source frame */
                        int s_x = data_pos % line_decoder->src_linesize;

                        /* convert X pos from source frame into the destination frame.
                         * it is byte offset from the beginning of a line.
                         */
                        int d_x = ((int)((s_x) / line_decoder->src_bpp)) *
                                line_decoder->dst_bpp;

                        /* pointer to data payload in packet */
                        source = (unsigned char*)(data);

                        /* copy whole packet that can span several lines.
                         * we need to clip data (v210 case) or center data (RGBA, R10k cases)
                         */
                        while (len > 0) {
                                /* len id payload length in source BPP
                                 * decoder needs len in destination BPP, so convert it
                                 */
                                int l = ((int)(len / line_decoder->src_bpp)) * line_decoder->dst_bpp;

                                /* do not copy multiple lines, we need to
                                 * copy (& clip, center) line by line
                                 */
                                if (l + d_x > (int) line_decoder->dst_linesize) {
                                        l = line_decoder->dst_linesize - d_x;
                                }

                                /* compute byte offset in destination frame */
                                offset = y + d_x;

                                /* watch the SEGV */
                                if (l + line_decoder->base_offset + offset <= tile->data_len) {
                                        /*decode frame:
                                         * we have offset for destination
                                         * we update source contiguously
                                         * we pass {r,g,b}shifts */
                                        line_decoder->decode_line((unsigned char*)tile->data + line_decoder->base_offset + offset, source, l,
                                                        line_decoder->shifts[0], line_decoder->shifts[1],
                                                        line_decoder->shifts[2]);
                                        /* we decoded one line (or a part of one line) to the end of the line
                                         * so decrease *source* len by 1 line (or that part of the line */
                                        len -= line_decoder->src_linesize - s_x;
                                        /* jump in source by the same amount */
                                        source += line_decoder->src_linesize - s_x;
                                } else {
                                        /* this should not ever happen as we call reconfigure before each packet
                                         * iff reconfigure is needed. But if it still happens, something is terribly wrong
                                         * say it loudly
                                         */
                                        if((prints % 100) == 0) {
                                                log_msg(LOG_LEVEL_ERROR, "WARNING!! Discarding input data as frame buffer is too small.\n"
                                                                "Well this should not happened. Expect troubles pretty soon.\n");
                                        }
                                        prints++;
                                        len = 0;
                                }
                                /* each new line continues from the beginning */
                                d_x = 0;        /* next line from beginning */
                                s_x = 0;
                                y += line_decoder->dst_pitch;  /* next line */
                        }
                } else { /* PT_VIDEO_LDGM or external decoder */
                        if(!frame->tiles[substream].data) {
                                frame->tiles[substream].data = (char *) malloc(buffer_length + PADDING);
                        }

                        memcpy(frame->tiles[substream].data + data_pos, (unsigned char*) data,
                                len);
                }

next_packet:
                cdata = cdata->nxt;
        }

        if(!pckt) {
                vf_free(frame);
                return FALSE;
        }

        if (decoder->frame == NULL && (pt == PT_VIDEO || pt == PT_ENCRYPT_VIDEO)) {
                ret = FALSE;
                goto cleanup;
        }

        assert(ret == TRUE);

        for(int i = 0; i < max_substreams; ++i) {
                frame_size += frame->tiles[i].data_len;
        }

        // format message
        {
                unique_ptr <frame_msg> fec_msg (new frame_msg(decoder->control, decoder->stats));
                fec_msg->buffer_num = std::move(buffer_num);
                fec_msg->recv_frame = frame;
                frame = NULL;
                fec_msg->recv_frame->fec_params = fec_desc(fec::fec_type_from_pt(pt), k, m, c, seed);
                fec_msg->recv_frame->ssrc = ssrc;
                fec_msg->pckt_list = std::move(pckt_list);
                fec_msg->received_pkts_cum = stats->received_pkts_cum;
                fec_msg->expected_pkts_cum = stats->expected_pkts_cum;
                fec_msg->nanoPerFrameExpected = decoder->frame ? 1000000000 / decoder->frame->fps : 0;

                auto t0 = std::chrono::high_resolution_clock::now();
                decoder->fec_queue.push(move(fec_msg));
                auto t1 = std::chrono::high_resolution_clock::now();
                double tpf = 1.0 / decoder->display_desc.fps;
                if (std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0).count() > tpf && decoder->stats.displayed > 20) {
                        decoder->slow_msg.print("Your computer may be too SLOW to play this !!!\n");
                }

        }
cleanup:
        ;
        if(ret != TRUE) {
                vf_free(frame);
        }

        pbuf_data->max_frame_size = max(pbuf_data->max_frame_size, frame_size);
        pbuf_data->decoded++;

        if (decoder->last_buffer_number != -1) {
                long int missing = buffer_number -
                        ((decoder->last_buffer_number + 1) & 0x3fffff);
                missing = (missing + 0x3fffff) % 0x3fffff;
                lock_guard<mutex> lk(decoder->stats.lock);
                if (missing < 0x3fffff / 2) {
                        decoder->stats.missing += missing;
                } else { // frames may have been reordered, add arbitrary 1
                        decoder->stats.missing += 1;
                }
        }
        decoder->last_buffer_number = buffer_number;

        return ret;
}

static void decoder_process_message(struct module *m)
{
        struct state_video_decoder *s = (struct state_video_decoder *) m->priv_data;

        struct message *msg;
        while ((msg = check_message(m))) {
                struct response *r;
                struct msg_universal *m_univ = (struct msg_universal *) msg;
                if (strcmp(m_univ->text, "get_format") == 0) {
                        s->lock.lock();
                        string video_desc = s->received_vid_desc;
                        s->lock.unlock();
                        r = new_response(RESPONSE_OK, video_desc.c_str());
                } else {
                        r = new_response(RESPONSE_NOT_FOUND, NULL);
                }
                free_message(msg, r);

        }
}

