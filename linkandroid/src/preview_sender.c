#define _DEFAULT_SOURCE

#include "preview_sender.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "websocket_client.h"
#include "../../app/src/screen.h"
#include "../../app/src/util/log.h"

// Base64 encoding table
static const char base64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Base64 encode function
static char *base64_encode(const unsigned char *data, size_t input_length)
{
    size_t output_length = 4 * ((input_length + 2) / 3);
    char *encoded_data = malloc(output_length + 1);
    if (!encoded_data)
    {
        return NULL;
    }

    for (size_t i = 0, j = 0; i < input_length;)
    {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = base64_chars[(triple >> 18) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 12) & 0x3F];
        encoded_data[j++] = base64_chars[(triple >> 6) & 0x3F];
        encoded_data[j++] = base64_chars[triple & 0x3F];
    }

    // Add padding
    for (size_t i = 0; i < (3 - input_length % 3) % 3; i++)
    {
        encoded_data[output_length - 1 - i] = '=';
    }

    encoded_data[output_length] = '\0';
    return encoded_data;
}

// Encode AVFrame directly to PNG (without going through SDL)
static bool encode_avframe_to_png(const AVFrame *src_frame,
                                  uint8_t ratio,
                                  unsigned char **out_data,
                                  size_t *out_size)
{
    if (!src_frame || !out_data || !out_size)
    {
        return false;
    }

    int orig_width = src_frame->width;
    int orig_height = src_frame->height;
    
    // Calculate scaled dimensions based on ratio (1-100)
    int scaled_width = (orig_width * ratio) / 100;
    int scaled_height = (orig_height * ratio) / 100;
    
    // Ensure minimum dimensions
    if (scaled_width < 1) scaled_width = 1;
    if (scaled_height < 1) scaled_height = 1;
    
    static bool first_log = true;
    if (first_log)
    {
        LOGI("Preview encode: original=%dx%d, ratio=%d%%, scaled=%dx%d",
             orig_width, orig_height, ratio, scaled_width, scaled_height);
        first_log = false;
    }

    // Find PNG encoder
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec)
    {
        LOGE("PNG codec not found");
        return false;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        LOGE("Failed to allocate codec context");
        return false;
    }

    codec_ctx->width = scaled_width;
    codec_ctx->height = scaled_height;
    codec_ctx->pix_fmt = AV_PIX_FMT_RGB24; // PNG works well with RGB24
    codec_ctx->time_base = (AVRational){1, 1};
    codec_ctx->compression_level = 3; // Balanced compression

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
    {
        LOGE("Failed to open PNG codec");
        avcodec_free_context(&codec_ctx);
        return false;
    }

    // Allocate frame for encoding
    AVFrame *rgb_frame = av_frame_alloc();
    if (!rgb_frame)
    {
        LOGE("Failed to allocate RGB frame");
        avcodec_free_context(&codec_ctx);
        return false;
    }

    rgb_frame->format = codec_ctx->pix_fmt;
    rgb_frame->width = scaled_width;
    rgb_frame->height = scaled_height;

    if (av_frame_get_buffer(rgb_frame, 32) < 0)
    {
        LOGE("Failed to allocate RGB frame buffer");
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    // Use swscale to convert and scale
    struct SwsContext *sws_ctx = sws_getContext(
        orig_width, orig_height, src_frame->format,
        scaled_width, scaled_height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    if (!sws_ctx)
    {
        LOGE("Failed to create swscale context");
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    sws_scale(sws_ctx,
              (const uint8_t *const *)src_frame->data,
              src_frame->linesize,
              0, orig_height,
              rgb_frame->data,
              rgb_frame->linesize);

    sws_freeContext(sws_ctx);

    // Encode frame to PNG
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        LOGE("Failed to allocate packet");
        av_frame_free(&rgb_frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    int ret = avcodec_send_frame(codec_ctx, rgb_frame);
    av_frame_free(&rgb_frame);

    if (ret < 0)
    {
        LOGE("Error sending frame for encoding");
        av_packet_free(&pkt);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    ret = avcodec_receive_packet(codec_ctx, pkt);
    if (ret < 0)
    {
        LOGE("Error receiving packet from encoder");
        av_packet_free(&pkt);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    // Copy encoded data
    *out_size = pkt->size;
    *out_data = malloc(pkt->size);
    if (!*out_data)
    {
        LOGE("Failed to allocate output buffer");
        av_packet_free(&pkt);
        avcodec_free_context(&codec_ctx);
        return false;
    }
    memcpy(*out_data, pkt->data, pkt->size);

    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);

    return true;
}

// Capture frame from SDL renderer and encode to PNG (DEPRECATED - kept for fallback)
static bool capture_and_encode_png(SDL_Renderer *renderer,
                                   const SDL_Rect *rect,
                                   uint8_t ratio,
                                   unsigned char **out_data,
                                   size_t *out_size)
{
    int width = rect->w;
    int height = rect->h;
    
    // Calculate scaled dimensions based on ratio (1-100)
    int scaled_width = (width * ratio) / 100;
    int scaled_height = (height * ratio) / 100;
    
    // Ensure minimum dimensions
    if (scaled_width < 1) scaled_width = 1;
    if (scaled_height < 1) scaled_height = 1;
    
    static bool first_log = true;
    if (first_log) {
        LOGI("Preview capture: original=%dx%d, ratio=%d%%, scaled=%dx%d",
             width, height, ratio, scaled_width, scaled_height);
        first_log = false;
    }

    // Create a surface to read pixels into (original size)
    SDL_Surface *surface = SDL_CreateRGBSurface(0, width, height, 32,
                                                0x00FF0000,
                                                0x0000FF00,
                                                0x000000FF,
                                                0xFF000000);
    if (!surface)
    {
        LOGE("Failed to create surface for capture: %s", SDL_GetError());
        return false;
    }

    // Read pixels from renderer (only the content rect)
    if (SDL_RenderReadPixels(renderer, rect, SDL_PIXELFORMAT_ARGB8888,
                             surface->pixels, surface->pitch) != 0)
    {
        LOGE("Failed to read pixels from renderer: %s", SDL_GetError());
        SDL_FreeSurface(surface);
        return false;
    }
    
    // Create scaled surface if ratio != 100
    SDL_Surface *final_surface = surface;
    if (ratio != 100)
    {
        final_surface = SDL_CreateRGBSurface(0, scaled_width, scaled_height, 32,
                                            0x00FF0000,
                                            0x0000FF00,
                                            0x000000FF,
                                            0xFF000000);
        if (!final_surface)
        {
            LOGE("Failed to create scaled surface: %s", SDL_GetError());
            SDL_FreeSurface(surface);
            return false;
        }
        
        // Scale the surface using SDL_BlitScaled
        SDL_Rect dst_rect = {0, 0, scaled_width, scaled_height};
        if (SDL_BlitScaled(surface, NULL, final_surface, &dst_rect) != 0)
        {
            LOGE("Failed to scale surface: %s", SDL_GetError());
            SDL_FreeSurface(final_surface);
            SDL_FreeSurface(surface);
            return false;
        }
        
        // Free original surface, we only need the scaled one
        SDL_FreeSurface(surface);
    }

    // Encode to PNG using libavcodec with scaled dimensions
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec)
    {
        LOGE("PNG codec not found");
        SDL_FreeSurface(final_surface);
        return false;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        LOGE("Failed to allocate codec context");
        SDL_FreeSurface(final_surface);
        return false;
    }

    codec_ctx->width = scaled_width;
    codec_ctx->height = scaled_height;
    codec_ctx->pix_fmt = AV_PIX_FMT_RGBA;
    codec_ctx->time_base = (AVRational){1, 1};
    codec_ctx->compression_level = 3; // 0-9: 0=no compression, 9=max compression, 3=balanced

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
    {
        LOGE("Failed to open codec");
        avcodec_free_context(&codec_ctx);
        SDL_FreeSurface(final_surface);
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        LOGE("Failed to allocate frame");
        avcodec_free_context(&codec_ctx);
        SDL_FreeSurface(final_surface);
        return false;
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;

    if (av_frame_get_buffer(frame, 32) < 0)
    {
        LOGE("Failed to allocate frame buffer");
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        SDL_FreeSurface(final_surface);
        return false;
    }

    // Copy SDL surface data to AVFrame (using scaled dimensions)
    for (int y = 0; y < scaled_height; y++)
    {
        memcpy(frame->data[0] + y * frame->linesize[0],
               (uint8_t *)final_surface->pixels + y * final_surface->pitch,
               scaled_width * 4);
    }

    SDL_FreeSurface(final_surface);

    // Encode frame
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
    {
        LOGE("Failed to allocate packet");
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    int ret = avcodec_send_frame(codec_ctx, frame);
    if (ret < 0)
    {
        LOGE("Error sending frame for encoding");
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    ret = avcodec_receive_packet(codec_ctx, pkt);
    if (ret < 0)
    {
        LOGE("Error receiving packet from encoder");
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }

    // Copy encoded data
    *out_size = pkt->size;
    *out_data = malloc(pkt->size);
    if (!*out_data)
    {
        LOGE("Failed to allocate output buffer");
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec_ctx);
        return false;
    }
    memcpy(*out_data, pkt->data, pkt->size);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);

    return true;
}

// Preview sender thread function
static void *preview_sender_thread(void *arg)
{
    struct la_preview_sender *sender = (struct la_preview_sender *)arg;

    LOGI("LinkAndroid preview sender thread started (interval: %u ms)",
         sender->interval_ms);

    while (sender->running)
    {
        // Wait for the interval
        usleep(sender->interval_ms * 1000);

        if (!sender->running)
        {
            break;
        }

        // Check if WebSocket is connected
        if (!la_websocket_client_is_connected(sender->ws_client))
        {
            continue;
        }

        // Check if screen has a frame available
        if (!sender->screen || !sender->screen->has_frame || !sender->screen->frame)
        {
            continue;
        }

        // Get the current frame (original resolution)
        AVFrame *frame = sender->screen->frame;
        if (frame->width <= 0 || frame->height <= 0)
        {
            continue;
        }

        // Encode frame directly from AVFrame (original resolution)
        unsigned char *png_data = NULL;
        size_t png_size = 0;

        if (!encode_avframe_to_png(frame, sender->ratio, &png_data, &png_size))
        {
            LOGW("Failed to encode frame to PNG");
            continue;
        }

        // Base64 encode
        char *base64_data = base64_encode(png_data, png_size);
        free(png_data);

        if (!base64_data)
        {
            LOGE("Failed to base64 encode image data");
            continue;
        }

        // Prepend data:image/png;base64, prefix
        size_t prefix_len = strlen("data:image/png;base64,");
        size_t base64_len = strlen(base64_data);
        char *prefixed_data = malloc(prefix_len + base64_len + 1);
        if (!prefixed_data)
        {
            LOGE("Failed to allocate memory for prefixed data");
            free(base64_data);
            continue;
        }
        strcpy(prefixed_data, "data:image/png;base64,");
        strcat(prefixed_data, base64_data);
        free(base64_data);

        // Send to WebSocket server
        bool sent = la_websocket_client_send_preview(sender->ws_client,
                                                     prefixed_data, "png");
        if (sent)
        {
            LOGD("Preview sent to server (size: %zu bytes)", png_size);
        }
        else
        {
            LOGW("Failed to send preview to server");
        }

        free(prefixed_data);
    }

    LOGI("LinkAndroid preview sender thread stopped");
    return NULL;
}

bool la_preview_sender_init(struct la_preview_sender *sender,
                            struct la_websocket_client *ws_client,
                            struct sc_screen *screen,
                            uint32_t interval_ms,
                            uint8_t ratio)
{
    if (!sender || !ws_client || !screen || interval_ms == 0 || ratio < 1 || ratio > 100)
    {
        return false;
    }

    sender->ws_client = ws_client;
    sender->screen = screen;
    sender->interval_ms = interval_ms;
    sender->ratio = ratio;
    sender->running = false;
    sender->thread_started = false;

    return true;
}

bool la_preview_sender_start(struct la_preview_sender *sender)
{
    if (!sender || sender->running)
    {
        return false;
    }

    sender->running = true;

    if (pthread_create(&sender->thread, NULL, preview_sender_thread, sender) != 0)
    {
        LOGE("Failed to create preview sender thread");
        sender->running = false;
        return false;
    }

    sender->thread_started = true;
    return true;
}

void la_preview_sender_stop(struct la_preview_sender *sender)
{
    if (!sender)
    {
        return;
    }

    sender->running = false;
}

void la_preview_sender_destroy(struct la_preview_sender *sender)
{
    if (!sender)
    {
        return;
    }

    // Signal thread to stop
    if (sender->running)
    {
        sender->running = false;
    }

    // Wait for thread to finish
    if (sender->thread_started)
    {
        pthread_join(sender->thread, NULL);
        sender->thread_started = false;
    }

    LOGI("LinkAndroid preview sender destroyed");
}
