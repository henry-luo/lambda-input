#include "image_internal.h"
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <limits.h>

static ImageStatus image_webp_error(ImageOperation *op, VP8StatusCode status) {
    ImageStatus code = status == VP8_STATUS_OUT_OF_MEMORY         ? IMAGE_OUT_OF_MEMORY
                       : status == VP8_STATUS_NOT_ENOUGH_DATA     ? IMAGE_TRUNCATED_INPUT
                       : status == VP8_STATUS_UNSUPPORTED_FEATURE ? IMAGE_UNSUPPORTED_FEATURE
                                                                  : IMAGE_INVALID_DATA;
    image_fail(op, code, "WebP codec status %d", status);
    op->error->native_code = status;
    return op->error->code;
}
static WebPDemuxer *image_webp_open_animation(ImageOperation *op, const uint8_t *data,
                                               size_t length) {
    if (length < 12 || read_le32(data + 4) > length - 8) {
        image_fail(op, IMAGE_TRUNCATED_INPUT, "incomplete WebP animation container");
        return NULL;
    }
    WebPData bytes = {data, length};
    WebPDemuxer *demux = WebPDemux(&bytes);
    if (!demux) {
        image_fail(op, IMAGE_INVALID_DATA, "invalid WebP animation container");
        return NULL;
    }
    if (WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT) > op->context->limits.max_frames) {
        WebPDemuxDelete(demux);
        image_fail(op, IMAGE_LIMIT_EXCEEDED, "WebP frame count exceeds limit");
        return NULL;
    }
    return demux;
}
static bool image_webp_working_fits(ImageOperation *op, const WebPBitstreamFeatures *features) {
    uint64_t bytes = features->format == 2 || features->has_alpha
                         ? (uint64_t)features->width * features->height * 8 : 0;
    if (bytes > op->context->limits.max_scratch_bytes - op->scratch_bytes) {
        image_fail(op, IMAGE_LIMIT_EXCEEDED, "WebP working memory estimate exceeds scratch budget");
        return false;
    }
    return true;
}
static ImageStatus image_webp_decode_buffer(ImageOperation *op, const uint8_t *data, size_t length,
                                             WebPDecoderConfig *config, uint8_t *pixels,
                                             size_t stride, size_t size) {
    config->output.colorspace = MODE_RGBA;
    config->output.is_external_memory = 1;
    config->output.u.RGBA.rgba = pixels;
    config->output.u.RGBA.stride = (int)stride;
    config->output.u.RGBA.size = size;
    config->options.use_threads = op->context->limits.max_threads > 1;
    VP8StatusCode status = WebPDecode(data, length, config);
    WebPFreeDecBuffer(&config->output);
    return status == VP8_STATUS_OK ? IMAGE_OK : image_webp_error(op, status);
}
static ImageStatus image_webp_first_frame(ImageOperation *op, const ImageSource *source,
                                          const uint8_t *data, const ImageDecodeOptions *options,
                                          WebPDecoderConfig *config, ImageDecoded *result) {
    uint32_t width, height;
    image_target_size(&result->info, options, &width, &height);
    if (options->mode == IMAGE_DECODE_THUMBNAIL && !options->allow_full_decode &&
        (width < result->info.width || height < result->info.height))
        return image_fail(op, IMAGE_FAST_PATH_UNAVAILABLE, "WebP animation needs a full initial canvas");
    WebPDemuxer *demux = image_webp_open_animation(op, data, source->length);
    if (!demux) return op->error->code;
    WebPIterator frame = {};
    if (!WebPDemuxGetFrame(demux, 1, &frame) || !frame.complete)
        image_fail(op, IMAGE_INVALID_DATA, "WebP animation has no complete initial frame");
    else {
        VP8StatusCode status = WebPGetFeatures(frame.fragment.bytes, frame.fragment.size, &config->input);
        if (status != VP8_STATUS_OK) image_webp_error(op, status);
        else if (image_webp_working_fits(op, &config->input) &&
                 image_allocate_pixels(op, result->info.width, result->info.height, result)) {
            // the initial canvas is transparent; ANIM background and later frames are not composited.
            memset(result->pixels, 0, result->size);
            size_t offset = (size_t)frame.y_offset * result->stride + (size_t)frame.x_offset * 4;
            image_webp_decode_buffer(op, frame.fragment.bytes, frame.fragment.size, config,
                                     result->pixels + offset, result->stride, result->size - offset);
            result->source_delay_ms = result->delay_ms = (uint32_t)frame.duration;
            result->loop_count = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);
            result->has_loop_extension = true;
        }
    }
    WebPDemuxReleaseIterator(&frame);
    WebPDemuxDelete(demux);
    return op->error->code;
}
ImageStatus image_webp_probe(ImageOperation *op, const ImageSource *source, ImageInfo *info) {
    const uint8_t *data = image_source_bytes(op, source);
    if (!data)
        return op->error->code;
    WebPBitstreamFeatures features;
    VP8StatusCode status = WebPGetFeatures(data, source->length, &features);
    if (status != VP8_STATUS_OK)
        return image_webp_error(op, status);
    info->width = (uint32_t)features.width;
    info->height = (uint32_t)features.height;
    info->has_alpha = features.has_alpha != 0;
    info->bit_depth = 8;
    info->orientation = 1;
    info->page_count = 1;
    info->pages_known = true;
    info->frames_known = !features.has_animation;
    info->frame_count = 1;
    if (features.has_animation) {
        WebPDemuxer *demux = image_webp_open_animation(op, data, source->length);
        if (!demux) return op->error->code;
        info->frame_count = WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT);
        info->frames_known = true;
        WebPDemuxDelete(demux);
    }
    return IMAGE_OK;
}
ImageStatus image_webp_decode(ImageOperation *op, const ImageSource *source,
                              const ImageDecodeOptions *options, ImageDecoded *result) {
    const uint8_t *data = image_source_bytes(op, source);
    if (!data)
        return op->error->code;
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config))
        return image_fail(op, IMAGE_INTERNAL_ERROR, "WebP ABI mismatch");
    VP8StatusCode status = WebPGetFeatures(data, source->length, &config.input);
    if (status != VP8_STATUS_OK)
        return image_webp_error(op, status);
    if (config.input.has_animation)
        return image_webp_first_frame(op, source, data, options, &config, result);
    uint32_t w = result->info.width, h = result->info.height;
    if (options->mode == IMAGE_DECODE_THUMBNAIL)
        image_target_size(&result->info, options, &w, &h);
    bool full_planes = config.input.format == 2 || config.input.has_alpha;
    bool shrinking = w < result->info.width || h < result->info.height;
    // VP8L history and the VP8 alpha plane retain source-size storage despite output scaling.
    if (full_planes && shrinking && !options->allow_full_decode)
        return image_fail(op, IMAGE_FAST_PATH_UNAVAILABLE, "WebP lossless/alpha needs source-size planes");
    if (!image_webp_working_fits(op, &config.input))
        return op->error->code;
    if (!image_allocate_pixels(op, w, h, result))
        return op->error->code;
    if (w != result->info.width || h != result->info.height) {
        config.options.use_scaling = 1;
        config.options.scaled_width = (int)w;
        config.options.scaled_height = (int)h;
        result->strategy = IMAGE_STRATEGY_NATIVE_SCALED;
        result->full_decode_fallback = full_planes;
    }
    return image_webp_decode_buffer(op, data, source->length, &config,
                                    result->pixels, result->stride, result->size);
}
typedef struct ImageWebpWriter {
    ImageOperation *op;
    const ImageSink *sink;
} ImageWebpWriter;
static int image_webp_write(const uint8_t *data, size_t size, const WebPPicture *picture) {
    ImageWebpWriter *writer = (ImageWebpWriter *)picture->custom_ptr;
    return image_sink_write(writer->op, writer->sink, data, size);
}
static int image_webp_progress(int, const WebPPicture *picture) {
    return !image_op_cancelled(((ImageWebpWriter *)picture->custom_ptr)->op);
}
ImageStatus image_webp_encode(ImageOperation *op, const ImagePixelView *pixels,
                              const ImageEncodeOptions *options, const ImageSink *sink) {
    if (pixels->stride > INT_MAX)
        return image_fail(op, IMAGE_LIMIT_EXCEEDED, "WebP stride exceeds codec range");
    ImagePixelView prepared;
    if (!image_prepare_pixels(op, pixels, pixels->channels == 1 ? 3 : pixels->channels, NULL, &prepared))
        return op->error->code;
    WebPConfig config;
    WebPPicture picture;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&picture))
        return image_fail(op, IMAGE_INTERNAL_ERROR, "WebP ABI mismatch");
    config.quality = (float)options->quality;
    config.lossless = options->lossless;
    config.method = 4;
    config.thread_level = op->context->limits.max_threads > 1;
    picture.width = (int)pixels->width;
    picture.height = (int)pixels->height;
    picture.use_argb = 1;
    int imported = prepared.channels == 4 ? WebPPictureImportRGBA(&picture, prepared.pixels, (int)prepared.stride)
                                           : WebPPictureImportRGB(&picture, prepared.pixels, (int)prepared.stride);
    if (!imported) {
        WebPPictureFree(&picture);
        return image_fail(op, IMAGE_OUT_OF_MEMORY, "cannot import WebP pixels");
    }
    ImageWebpWriter writer = {op, sink};
    picture.custom_ptr = &writer;
    picture.writer = image_webp_write;
    picture.progress_hook = image_webp_progress;
    if (!WebPEncode(&config, &picture))
        image_fail(
            op, picture.error_code == VP8_ENC_ERROR_OUT_OF_MEMORY ? IMAGE_OUT_OF_MEMORY : IMAGE_INVALID_DATA,
            "WebP encoding failed (%d)", picture.error_code);
    WebPPictureFree(&picture);
    return op->error->code;
}
