// SPDX-License-Identifier: MIT
// Custom minimal FFmpeg codec/demuxer/parser registry for Switube background playback.
// Replaces allcodecs.o, allformats.o, parsers.o, and protocols.o to prevent linking
// 500+ unused video/audio decoders, parsers, and protocols into the sysmodule.

#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <stdint.h>
#include <string.h>

// --- Minimal FFmpeg decoders (Opus packets go to libopus) ---
extern const void ff_opus_decoder;
extern const void ff_aac_decoder;
extern const void ff_vorbis_decoder;
extern const void ff_flac_decoder;
extern const void ff_mp3_decoder;
extern const void ff_mp3float_decoder;

static const void * const custom_codec_list[] = {
#ifdef HOMENX_STREAMING_MODULE
    &ff_opus_decoder,
#endif
    &ff_aac_decoder,
    &ff_vorbis_decoder,
    &ff_flac_decoder,
    &ff_mp3_decoder,
    &ff_mp3float_decoder,
    NULL
};

const AVCodec *av_codec_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    const AVCodec *c = (const AVCodec *)custom_codec_list[i];
    if (c) {
        *opaque = (void *)(i + 1);
        return c;
    }
    return NULL;
}

const AVCodec *avcodec_find_decoder(enum AVCodecID id) {
    void *opaque = NULL;
    const AVCodec *c;
    while ((c = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(c) && c->id == id) {
            return c;
        }
    }
    return NULL;
}

const AVCodec *avcodec_find_decoder_by_name(const char *name) {
    if (!name) return NULL;
    void *opaque = NULL;
    const AVCodec *c;
    while ((c = av_codec_iterate(&opaque))) {
        if (av_codec_is_decoder(c) && strcmp(c->name, name) == 0) {
            return c;
        }
    }
    return NULL;
}

const AVCodec *avcodec_find_encoder(enum AVCodecID id) {
    (void)id;
    return NULL;
}

const AVCodec *avcodec_find_encoder_by_name(const char *name) {
    (void)name;
    return NULL;
}

// --- Minimal Demuxers (Matroska/WebM, MOV/MP4, AAC, OGG, MP3, FLAC, WAV) ---
extern const AVInputFormat ff_matroska_demuxer;
extern const AVInputFormat ff_mov_demuxer;
extern const AVInputFormat ff_aac_demuxer;
extern const AVInputFormat ff_ogg_demuxer;
extern const AVInputFormat ff_mp3_demuxer;
extern const AVInputFormat ff_flac_demuxer;
extern const AVInputFormat ff_wav_demuxer;

static const AVInputFormat * const custom_demuxer_list[] = {
    &ff_matroska_demuxer,
    &ff_mov_demuxer,
    &ff_aac_demuxer,
    &ff_ogg_demuxer,
    &ff_mp3_demuxer,
    &ff_flac_demuxer,
    &ff_wav_demuxer,
    NULL
};

const AVInputFormat *av_demuxer_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    const AVInputFormat *f = custom_demuxer_list[i];
    if (f) {
        *opaque = (void *)(i + 1);
        return f;
    }
    return NULL;
}

const AVOutputFormat *av_muxer_iterate(void **opaque) {
    (void)opaque;
    return NULL;
}

void avpriv_register_devices(void *muxer_list, void *demuxer_list) {
    (void)muxer_list;
    (void)demuxer_list;
}

// --- Minimal Parsers ---
extern const AVCodecParser ff_opus_parser;
extern const AVCodecParser ff_aac_parser;
extern const AVCodecParser ff_vorbis_parser;
extern const AVCodecParser ff_flac_parser;
extern const AVCodecParser ff_mpegaudio_parser;

static const AVCodecParser * const custom_parser_list[] = {
    &ff_opus_parser,
    &ff_aac_parser,
    &ff_vorbis_parser,
    &ff_flac_parser,
    &ff_mpegaudio_parser,
    NULL
};

const AVCodecParser *av_parser_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    const AVCodecParser *p = custom_parser_list[i];
    if (p) {
        *opaque = (void *)(i + 1);
        return p;
    }
    return NULL;
}

// --- Minimal Bitstream Filters (None/Null + AAC ADTS-to-ASC) ---
// Prevents linking bitstream_filters.o which pulls in 1.2+ MB of AV1/H266/HEVC/H264/VP9 CBS code.
extern const void ff_null_bsf;
extern const void ff_aac_adtstoasc_bsf;

static const void * const custom_bsf_list[] = {
    &ff_null_bsf,
    &ff_aac_adtstoasc_bsf,
    NULL
};

const void *av_bsf_iterate(void **opaque) {
    uintptr_t i = (uintptr_t)*opaque;
    const void *b = custom_bsf_list[i];
    if (b) {
        *opaque = (void *)(i + 1);
        return b;
    }
    return NULL;
}

const void *av_bsf_get_by_name(const char *name) {
    if (!name) return NULL;
    void *opaque = NULL;
    const char * const *b;
    while ((b = (const char * const *)av_bsf_iterate(&opaque))) {
        if (*b && strcmp(*b, name) == 0) {
            return b;
        }
    }
    return NULL;
}

const AVClass *ff_bsf_child_class_iterate(void **opaque) {
    (void)opaque;
    return NULL;
}

// --- Minimal URL Protocols (None - Switube uses custom AVIO / libcurl exclusively) ---
const char *avio_enum_protocols(void **opaque, int output) {
    (void)opaque;
    (void)output;
    return NULL;
}

const AVClass *avio_protocol_get_class(const char *name) {
    (void)name;
    return NULL;
}

const AVClass *ff_urlcontext_child_class_iterate(void **opaque) {
    (void)opaque;
    return NULL;
}

const void **ffurl_get_protocols(const char *whitelist, const char *blacklist) {
    (void)whitelist;
    (void)blacklist;
    return NULL;
}
