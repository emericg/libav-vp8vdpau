/*
 * Copyright (c) 2011 Stefano Sabatini
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * buffer sink
 */

#include "libavutil/audio_fifo.h"
#include "libavutil/audioconvert.h"
#include "libavutil/avassert.h"
#include "libavutil/common.h"
#include "libavutil/mathematics.h"

#include "audio.h"
#include "avfilter.h"
#include "buffersink.h"
#include "internal.h"

typedef struct {
    AVFilterBufferRef *cur_buf;  ///< last buffer delivered on the sink
    AVAudioFifo  *audio_fifo;    ///< FIFO for audio samples
    int64_t next_pts;            ///< interpolating audio pts
} BufferSinkContext;

static av_cold void uninit(AVFilterContext *ctx)
{
    BufferSinkContext *sink = ctx->priv;

    if (sink->audio_fifo)
        av_audio_fifo_free(sink->audio_fifo);
}

static int start_frame(AVFilterLink *link, AVFilterBufferRef *buf)
{
    BufferSinkContext *s = link->dst->priv;

    av_assert0(!s->cur_buf);
    s->cur_buf    = buf;
    link->cur_buf = NULL;

    return 0;
};

int av_buffersink_read(AVFilterContext *ctx, AVFilterBufferRef **buf)
{
    BufferSinkContext *s    = ctx->priv;
    AVFilterLink      *link = ctx->inputs[0];
    int ret;

    if (!buf)
        return ff_poll_frame(ctx->inputs[0]);

    if ((ret = ff_request_frame(link)) < 0)
        return ret;

    if (!s->cur_buf)
        return AVERROR(EINVAL);

    *buf       = s->cur_buf;
    s->cur_buf = NULL;

    return 0;
}

static int read_from_fifo(AVFilterContext *ctx, AVFilterBufferRef **pbuf,
                          int nb_samples)
{
    BufferSinkContext *s = ctx->priv;
    AVFilterLink   *link = ctx->inputs[0];
    AVFilterBufferRef *buf;

    if (!(buf = ff_get_audio_buffer(link, AV_PERM_WRITE, nb_samples)))
        return AVERROR(ENOMEM);
    av_audio_fifo_read(s->audio_fifo, (void**)buf->extended_data, nb_samples);

    buf->pts = s->next_pts;
    s->next_pts += av_rescale_q(nb_samples, (AVRational){1, link->sample_rate},
                                link->time_base);

    *pbuf = buf;
    return 0;

}

int av_buffersink_read_samples(AVFilterContext *ctx, AVFilterBufferRef **pbuf,
                               int nb_samples)
{
    BufferSinkContext *s = ctx->priv;
    AVFilterLink   *link = ctx->inputs[0];
    int ret = 0;

    if (!s->audio_fifo) {
        int nb_channels = av_get_channel_layout_nb_channels(link->channel_layout);
        if (!(s->audio_fifo = av_audio_fifo_alloc(link->format, nb_channels, nb_samples)))
            return AVERROR(ENOMEM);
    }

    while (ret >= 0) {
        AVFilterBufferRef *buf;

        if (av_audio_fifo_size(s->audio_fifo) >= nb_samples)
            return read_from_fifo(ctx, pbuf, nb_samples);

        ret = av_buffersink_read(ctx, &buf);
        if (ret == AVERROR_EOF && av_audio_fifo_size(s->audio_fifo))
            return read_from_fifo(ctx, pbuf, av_audio_fifo_size(s->audio_fifo));
        else if (ret < 0)
            return ret;

        if (buf->pts != AV_NOPTS_VALUE) {
            s->next_pts = buf->pts -
                          av_rescale_q(av_audio_fifo_size(s->audio_fifo),
                                       (AVRational){ 1, link->sample_rate },
                                       link->time_base);
        }

        ret = av_audio_fifo_write(s->audio_fifo, (void**)buf->extended_data,
                                  buf->audio->nb_samples);
        avfilter_unref_buffer(buf);
    }

    return ret;
}

AVFilter avfilter_vsink_buffer = {
    .name      = "buffersink",
    .description = NULL_IF_CONFIG_SMALL("Buffer video frames, and make them available to the end of the filter graph."),
    .priv_size = sizeof(BufferSinkContext),
    .uninit    = uninit,

    .inputs    = (const AVFilterPad[]) {{ .name          = "default",
                                          .type          = AVMEDIA_TYPE_VIDEO,
                                          .start_frame   = start_frame,
                                          .min_perms     = AV_PERM_READ,
                                          .needs_fifo    = 1 },
                                        { .name = NULL }},
    .outputs   = NULL,
};

AVFilter avfilter_asink_abuffer = {
    .name      = "abuffersink",
    .description = NULL_IF_CONFIG_SMALL("Buffer audio frames, and make them available to the end of the filter graph."),
    .priv_size = sizeof(BufferSinkContext),
    .uninit    = uninit,

    .inputs    = (const AVFilterPad[]) {{ .name           = "default",
                                          .type           = AVMEDIA_TYPE_AUDIO,
                                          .filter_samples = start_frame,
                                          .min_perms      = AV_PERM_READ,
                                          .needs_fifo     = 1 },
                                        { .name = NULL }},
    .outputs   = NULL,
};
