/*
 * FILE:    video_compress/libavcodec.c
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2011 CESNET z.s.p.o.
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
 *      This product includes software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of the CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
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
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "video_compress/libavcodec.h"

#include <assert.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "debug.h"
#include "host.h"
#include "utils/resource_manager.h"
#include "video.h"
#include "video_codec.h"

#define DEFAULT_CODEC MJPG

struct libav_video_compress {
        pthread_mutex_t    *lavcd_global_lock;

        struct tile        *out[2];
        struct video_desc   saved_desc;

        AVFrame            *in_frame;
        AVCodec            *codec;
        AVCodecContext     *codec_ctx;
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
        AVPacket            pkt[2];
#endif

        unsigned char      *decoded;
        decoder_t           decoder;

        codec_t             selected_codec_id;
        int                 requested_bitrate;
        // may be 422, 420 or 0 (no subsampling explicitly requested
        int                 requested_subsampling;
        // actual value used
        int                 subsampling;

        codec_t             out_codec;
};

static void to_yuv420(AVFrame *out_frame, unsigned char *in_data, int width, int height);
static void to_yuv422(AVFrame *out_frame, unsigned char *in_data, int width, int height);
static void usage(void);
static void cleanup(struct libav_video_compress *s);

static void usage() {
        printf("Libavcodec encoder usage:\n");
        printf("\t-c libavcodec[:codec=<codec_name>][:bitrate=<bits_per_sec>]"
                        "[:subsampling=<subsampling>]\n");
        printf("\t\t<codec_name> may be "
                        " one of \"H.264\", \"VP8\" or "
                        "\"MJPEG\" (default)\n");
        printf("\t\t<bits_per_sec> specifies requested bitrate\n");
        printf("\t\t<subsampling> may be one of 422 or 420, default 420 for progresive, 422 for interlaced\n");
        printf("\t\t\t0 means codec default (same as when parameter omitted)\n");
}

void * libavcodec_compress_init(char * fmt)
{
        struct libav_video_compress *s;
        char *item, *save_ptr = NULL;
        
        s = (struct libav_video_compress *) malloc(sizeof(struct libav_video_compress));
        s->lavcd_global_lock = rm_acquire_shared_lock(LAVCD_LOCK_NAME);

        s->out[0] = s->out[1] = NULL;

        s->codec = NULL;
        s->codec_ctx = NULL;
        s->in_frame = NULL;
        s->selected_codec_id = DEFAULT_CODEC;
        s->subsampling = s->requested_subsampling = 0;

        s->requested_bitrate = -1;

        memset(&s->saved_desc, 0, sizeof(s->saved_desc));

        if(fmt) {
                while((item = strtok_r(fmt, ":", &save_ptr)) != NULL) {
                        if(strncasecmp("help", item, strlen("help")) == 0) {
                                usage();
                                return NULL;
                        } else if(strncasecmp("codec=", item, strlen("codec=")) == 0) {
                                char *codec = item + strlen("codec=");
                                int i;
                                for (i = 0; codec_info[i].name != NULL; i++) {
                                        if (strcasecmp(codec, codec_info[i].name) == 0) {
                                                s->selected_codec_id = codec_info[i].codec;
                                                break;
                                        }
                                }
                                if(codec_info[i].name == NULL) {
                                        fprintf(stderr, "[lavd] Unable to find codec: \"%s\"\n", codec);
                                        return NULL;
                                }
                        } else if(strncasecmp("bitrate=", item, strlen("bitrate=")) == 0) {
                                char *bitrate_str = item + strlen("bitrate=");
                                char *end_ptr;
                                char unit_prefix_u;
                                s->requested_bitrate = strtoul(bitrate_str, &end_ptr, 10);
                                unit_prefix_u = toupper(*end_ptr);
                                switch(unit_prefix_u) {
                                        case 'G':
                                                s->requested_bitrate *= 1000;
                                        case 'M':
                                                s->requested_bitrate *= 1000;
                                        case 'K':
                                                s->requested_bitrate *= 1000;
                                                break;
                                        case '\0':
                                                break;
                                        default:
                                                fprintf(stderr, "[lavc] Error: unknown unit prefix %c.\n",
                                                                *end_ptr);
                                                return NULL;
                                }
                        } else if(strncasecmp("subsampling=", item, strlen("subsampling=")) == 0) {
                                char *subsample_str = item + strlen("subsampling=");
                                s->requested_subsampling = atoi(subsample_str);
                                if(s->requested_subsampling != 422 &&
                                                s->requested_subsampling != 420) {
                                        fprintf(stderr, "[lavc] Supported subsampling is only 422 or 420.\n");
                                        free(s);
                                        return NULL;
                                }
                        } else {
                                fprintf(stderr, "[lavc] Error: unknown option %s.\n",
                                                item);
                                return NULL;
                        }
                        fmt = NULL;
                }
        }

        s->decoded = NULL;

#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
        for(int i = 0; i < 2; ++i) {
                av_init_packet(&s->pkt[i]);
                s->pkt[i].data = NULL;
                s->pkt[i].size = 0;
        }
#endif

        /*  register all the codecs (you can also register only the codec
         *         you wish to have smaller code */
        avcodec_register_all();

        return s;
}

static bool configure_with(struct libav_video_compress *s, struct video_desc desc)
{
        int ret;
        int codec_id;
        int pix_fmt; 
        double avg_bpp; // average bite per pixel

        struct video_desc compressed_desc;
        enum {
                YUV420P,
                YUV422P,
                YUVJ420P,
                YUVJ422P
        } tmp_pixfmt;
        compressed_desc = desc;
        switch(s->selected_codec_id) {
                case H264:
#ifdef HAVE_GPL
                        codec_id = CODEC_ID_H264;
                        tmp_pixfmt = YUV420P;
                        compressed_desc.color_spec = H264;
                        // from H.264 Primer
                        avg_bpp =
                                4 * /* for H.264: 1 - low motion, 2 - medium motion, 4 - high motion */
                                0.07;
                        break;
#else
                        fprintf(stderr, "H.264 not available in UltraGrid BSD build. "
                                        "Reconfigure UltraGrid with --enable-gpl if "
                                        "needed.\n");
                        exit_uv(1);
                        return false;
#endif
                case MJPG:
                        codec_id = CODEC_ID_MJPEG;
                        tmp_pixfmt = YUVJ420P;
                        compressed_desc.color_spec = MJPG;
                        avg_bpp = 0.7;
                        break;
                case VP8:
                        codec_id = CODEC_ID_VP8;
                        tmp_pixfmt = YUV420P;
                        compressed_desc.color_spec = VP8;
                        avg_bpp = 0.5;
                        break;
                default:
                        fprintf(stderr, "[lavc] Requested output codec isn't "
                                        "supported by libavcodec.\n");
                        return false;

        }

        /* either user has explicitly requested subsampling
         * or for interlaced format is better to have 422 */
        if(s->requested_subsampling == 422 ||
                        (desc.interlacing == INTERLACED_MERGED &&
                         s->requested_subsampling != 420)) {
                s->subsampling = 422;
                if(tmp_pixfmt == YUV420P) {
                        tmp_pixfmt = YUV422P;
                }
                if(tmp_pixfmt == YUVJ420P) {
                        tmp_pixfmt = YUVJ422P;
                }
        } else {
                s->subsampling = 420;
        }

        switch(tmp_pixfmt) {
                case YUV420P:
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                        pix_fmt = AV_PIX_FMT_YUV420P;
#else
                        pix_fmt = PIX_FMT_YUV420P;
#endif
                        break;
                case YUV422P:
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                        pix_fmt = AV_PIX_FMT_YUV422P;
#else
                        pix_fmt = PIX_FMT_YUV422P;
#endif
                        break;
                case YUVJ420P:
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                        pix_fmt = AV_PIX_FMT_YUVJ420P;
#else
                        pix_fmt = PIX_FMT_YUVJ420P;
#endif
                        break;
                case YUVJ422P:
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                        pix_fmt = AV_PIX_FMT_YUVJ422P;
#else
                        pix_fmt = PIX_FMT_YUVJ422P;
#endif
                        break;
        }

        for(int i = 0; i < 2; ++i) {
                s->out[i] = tile_alloc_desc(compressed_desc);
#ifndef HAVE_AVCODEC_ENCODE_VIDEO2
                s->out[i]->data = malloc(compressed_desc.width *
                        compressed_desc.height * 4);
#endif // HAVE_AVCODEC_ENCODE_VIDEO2
        }

        /* find the video encoder */
        s->codec = avcodec_find_encoder(codec_id);
        if (!s->codec) {
                fprintf(stderr, "Libavcodec doesn't contain specified codec.\n"
                                "Hint: Check if you have libavcodec-extra package installed.\n");
                return false;
        }

        // avcodec_alloc_context3 allocates context and sets default value
        s->codec_ctx = avcodec_alloc_context3(s->codec);
        if (!s->codec_ctx) {
                fprintf(stderr, "Could not allocate video codec context\n");
                return false;
        }

        /* put parameters */
        if(s->requested_bitrate > 0) {
                s->codec_ctx->bit_rate = s->requested_bitrate;
        } else {
                s->codec_ctx->bit_rate = desc.width * desc.height *
                        avg_bpp * desc.fps;
        }
        s->codec_ctx->bit_rate_tolerance = s->codec_ctx->bit_rate / 4;

        /* resolution must be a multiple of two */
        s->codec_ctx->width = desc.width;
        s->codec_ctx->height = desc.height;
        /* frames per second */
        s->codec_ctx->time_base= (AVRational){1,(int) desc.fps};
        s->codec_ctx->gop_size = 20; /* emit one intra frame every ten frames */
        s->codec_ctx->max_b_frames = 0;
        switch(desc.color_spec) {
                case Vuy2:
                case DVS8:
                case UYVY:
                        s->decoder = (decoder_t) memcpy;
                        break;
                case YUYV:
                        s->decoder = (decoder_t) vc_copylineYUYV;
                        break;
                case v210:
                        s->decoder = (decoder_t) vc_copylinev210;
                        break;
                case RGB:
                        s->decoder = (decoder_t) vc_copylineRGBtoUYVY;
                        break;
                case BGR:
                        s->decoder = (decoder_t) vc_copylineBGRtoUYVY;
                        break;
                case RGBA:
                        s->decoder = (decoder_t) vc_copylineRGBAtoUYVY;
                        break;
                default:
                        fprintf(stderr, "[Libavcodec] Unable to find "
                                        "appropriate pixel format.\n");
                        return false;
        }

        s->codec_ctx->pix_fmt = pix_fmt;

        s->decoded = malloc(desc.width * desc.height * 4);

        if(codec_id == CODEC_ID_H264) {
                av_opt_set(s->codec_ctx->priv_data, "preset", "ultrafast", 0);
                //av_opt_set(s->codec_ctx->priv_data, "tune", "fastdecode", 0);
                av_opt_set(s->codec_ctx->priv_data, "tune", "zerolatency", 0);
#ifndef DISABLE_H264_INTRA_REFRESH
                s->codec_ctx->refs = 1;
                av_opt_set(s->codec_ctx->priv_data, "intra-refresh", "1", 0);
#endif
        } else if(codec_id == CODEC_ID_VP8) {
                s->codec_ctx->thread_count = 8;
                s->codec_ctx->profile = 3;
                s->codec_ctx->slices = 4;
                s->codec_ctx->rc_buffer_size = s->codec_ctx->bit_rate / desc.fps;
                s->codec_ctx->rc_buffer_aggressivity = 0.5;
        } else {
                // zero should mean count equal to the number of virtual cores
                if(s->codec->capabilities & CODEC_CAP_SLICE_THREADS) {
                        s->codec_ctx->thread_count = 0;
                        s->codec_ctx->thread_type = FF_THREAD_SLICE;
                } else {
                        fprintf(stderr, "[lavd] Warning: Codec doesn't support slice-based multithreading.\n");
#if 0
                        if(s->codec->capabilities & CODEC_CAP_FRAME_THREADS) {
                                s->codec_ctx->thread_count = 0;
                                s->codec_ctx->thread_type = FF_THREAD_FRAME;
                        } else {
                                fprintf(stderr, "[lavd] Warning: Codec doesn't support frame-based multithreading.\n");
                        }
#endif
                }
        }

        pthread_mutex_lock(s->lavcd_global_lock);
        /* open it */
        if (avcodec_open2(s->codec_ctx, s->codec, NULL) < 0) {
                fprintf(stderr, "Could not open codec\n");
                pthread_mutex_unlock(s->lavcd_global_lock);
                return false;
        }
        pthread_mutex_unlock(s->lavcd_global_lock);

        s->in_frame = avcodec_alloc_frame();
        if (!s->in_frame) {
                fprintf(stderr, "Could not allocate video frame\n");
                return false;
        }
#if 0
        s->in_frame->format = s->codec_ctx->pix_fmt;
        s->in_frame->width = s->codec_ctx->width;
        s->in_frame->height = s->codec_ctx->height;
#endif

        /* the image can be allocated by any means and av_image_alloc() is
         * just the most convenient way if av_malloc() is to be used */
        ret = av_image_alloc(s->in_frame->data, s->in_frame->linesize,
                        s->codec_ctx->width, s->codec_ctx->height,
                        s->codec_ctx->pix_fmt, 32);
        if (ret < 0) {
                fprintf(stderr, "Could not allocate raw picture buffer\n");
                return false;
        }

        s->saved_desc = desc;
        s->out_codec = compressed_desc.color_spec;

        return true;
}

static void to_yuv420(AVFrame *out_frame, unsigned char *in_data, int width, int height)
{
        unsigned char *src = in_data + 1;
        for(int y = 0; y < (int) height; ++y) {
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                for(int x = 0; x < width; ++x) {
                        *dst_y++ = *src;
                        src += 2;
                }
        }

        for(int y = 0; y < (int) height / 2; ++y) {
                /*  every even row */
                unsigned char *src1 = in_data + (y * 2) * (width * 2);
                /*  every odd row */
                unsigned char *src2 = in_data + (y * 2 + 1) * (width * 2);
                unsigned char *dst_cb = out_frame->data[1] + out_frame->linesize[1] * y;
                unsigned char *dst_cr = out_frame->data[2] + out_frame->linesize[2] * y;
                for(int x = 0; x < width / 2; ++x) {
                        *dst_cb++ = (*src1 + *src2) / 2;
                        src1 += 2;
                        src2 += 2;
                        *dst_cr++ = (*src1 + *src2) / 2;
                        src1 += 2;
                        src2 += 2;
                }
        }
}

static void to_yuv422(AVFrame *out_frame, unsigned char *src, int width, int height)
{
        for(int y = 0; y < (int) height; ++y) {
                unsigned char *dst_y = out_frame->data[0] + out_frame->linesize[0] * y;
                unsigned char *dst_cb = out_frame->data[1] + out_frame->linesize[1] * y;
                unsigned char *dst_cr = out_frame->data[2] + out_frame->linesize[2] * y;
                for(int x = 0; x < width; x += 2) {
                        *dst_cb++ = *src++;
                        *dst_y++ = *src++;
                        *dst_cr++ = *src++;
                        *dst_y++ = *src++;
                }
        }
}

struct tile * libavcodec_compress_tile(void *arg, struct tile *tx, struct video_desc *desc,
                int buffer_idx)
{
        struct libav_video_compress *s = (struct libav_video_compress *) arg;
        assert (buffer_idx == 0 || buffer_idx == 1);
        static int frame_seq = 0;
        int ret;
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
        int got_output;
#endif

        if(!video_desc_eq(*desc, s->saved_desc)) {
                cleanup(s);
                int ret = configure_with(s, *desc);
                if(!ret) {
                        return NULL;
                }
        }

        s->in_frame->pts = frame_seq++;
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
        av_free_packet(&s->pkt[buffer_idx]);
        av_init_packet(&s->pkt[buffer_idx]);
        s->pkt[buffer_idx].data = NULL;
        s->pkt[buffer_idx].size = 0;
#endif

        unsigned char *decoded;

        if((void *) s->decoder != (void *) memcpy) {
                unsigned char *line1 = (unsigned char *) tx->data;
                unsigned char *line2 = (unsigned char *) s->decoded;
                int src_linesize = vc_get_linesize(tx->width, desc->color_spec);
                int dst_linesize = tx->width * 2; /* UYVY */
                for (int i = 0; i < (int) tx->height; ++i) {
                        s->decoder(line2, line1, dst_linesize,
                                        0, 8, 16);
                        line1 += src_linesize;
                        line2 += dst_linesize;
                }
                decoded = s->decoded;
        } else {
                decoded = (unsigned char *) tx->data;
        }

        if(s->subsampling == 420) {
                to_yuv420(s->in_frame, decoded, tx->width, tx->height);
        } else {
                assert(s->subsampling == 422);
                to_yuv422(s->in_frame, decoded, tx->width, tx->height);
        }


#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
        /* encode the image */
        ret = avcodec_encode_video2(s->codec_ctx, &s->pkt[buffer_idx],
                        s->in_frame, &got_output);
        if (ret < 0) {
                fprintf(stderr, "Error encoding frame\n");
                return NULL;
        }

        if (got_output) {
                //printf("Write frame %3d (size=%5d)\n", frame_seq, s->pkt[buffer_idx].size);
                s->out[buffer_idx]->data = (char *) s->pkt[buffer_idx].data;
                s->out[buffer_idx]->data_len = s->pkt[buffer_idx].size;
        } else {
                return NULL;
        }
#else
        /* encode the image */
        ret = avcodec_encode_video(s->codec_ctx, (uint8_t *) s->out[buffer_idx]->data,
                        s->out[buffer_idx]->width * s->out[buffer_idx]->height * 4,
                        s->in_frame);
        if (ret < 0) {
                fprintf(stderr, "Error encoding frame\n");
                return NULL;
        }

        if (ret) {
                //printf("Write frame %3d (size=%5d)\n", frame_seq, s->pkt[buffer_idx].size);
                s->out[buffer_idx]->data_len = ret;
        } else {
                return NULL;
        }
#endif // HAVE_AVCODEC_ENCODE_VIDEO2

        desc->color_spec = s->out_codec;

        return s->out[buffer_idx];
}

static void cleanup(struct libav_video_compress *s)
{
        for(int i = 0; i < 2; ++i) {
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                vf_free(s->out[i]);
                s->out[i] = 0;
                av_free_packet(&s->pkt[i]);
#else
                tile_free_data(s->out[i]);
                s->out[i] = 0;
#endif // HAVE_AVCODEC_ENCODE_VIDEO2
        }

        if(s->codec_ctx) {
                pthread_mutex_lock(s->lavcd_global_lock);
                avcodec_close(s->codec_ctx);
                pthread_mutex_unlock(s->lavcd_global_lock);
        }
        if(s->in_frame) {
                av_freep(s->in_frame->data);
                av_free(s->in_frame);
                s->in_frame = NULL;
        }
        av_free(s->codec_ctx);
        s->codec_ctx = NULL;
        free(s->decoded);
        s->decoded = NULL;
}

void libavcodec_compress_done(void *arg)
{
        struct libav_video_compress *s = (struct libav_video_compress *) arg;

        cleanup(s);

        rm_release_shared_lock(LAVCD_LOCK_NAME);
        free(s);
}

