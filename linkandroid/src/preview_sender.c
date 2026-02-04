#define _DEFAULT_SOURCE

#include "preview_sender.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <SDL2/SDL.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

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

// Capture frame from SDL renderer and encode to PNG
static bool capture_and_encode_png(SDL_Renderer *renderer,
                                   const SDL_Rect *rect,
                                   unsigned char **out_data,
                                   size_t *out_size)
{
    int width = rect->w;
    int height = rect->h;

    // Create a surface to read pixels into
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

    // Encode to PNG using libavcodec
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec)
    {
        LOGE("PNG codec not found");
        SDL_FreeSurface(surface);
        return false;
    }

    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx)
    {
        LOGE("Failed to allocate codec context");
        SDL_FreeSurface(surface);
        return false;
    }

    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->pix_fmt = AV_PIX_FMT_RGBA;
    codec_ctx->time_base = (AVRational){1, 1};

    if (avcodec_open2(codec_ctx, codec, NULL) < 0)
    {
        LOGE("Failed to open codec");
        avcodec_free_context(&codec_ctx);
        SDL_FreeSurface(surface);
        return false;
    }

    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        LOGE("Failed to allocate frame");
        avcodec_free_context(&codec_ctx);
        SDL_FreeSurface(surface);
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
        SDL_FreeSurface(surface);
        return false;
    }

    // Copy SDL surface data to AVFrame
    for (int y = 0; y < height; y++)
    {
        memcpy(frame->data[0] + y * frame->linesize[0],
               (uint8_t *)surface->pixels + y * surface->pitch,
               width * 4);
    }

    SDL_FreeSurface(surface);

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

        // Check if screen has a renderer and valid content rect
        if (!sender->screen || !sender->screen->display.renderer || !sender->screen->has_frame)
        {
            continue;
        }

        SDL_Renderer *renderer = sender->screen->display.renderer;
        const SDL_Rect *rect = &sender->screen->rect;

        // Ensure rect has valid dimensions
        if (rect->w <= 0 || rect->h <= 0)
        {
            continue;
        }

        // Capture and encode frame (only the content rect, excluding panel buttons)
        unsigned char *png_data = NULL;
        size_t png_size = 0;

        if (!capture_and_encode_png(renderer, rect, &png_data, &png_size))
        {
            LOGW("Failed to capture and encode frame");
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
                            uint32_t interval_ms)
{
    if (!sender || !ws_client || !screen || interval_ms == 0)
    {
        return false;
    }

    sender->ws_client = ws_client;
    sender->screen = screen;
    sender->interval_ms = interval_ms;
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

    if (sender->running)
    {
        sender->running = false;
    }

    if (sender->thread_started)
    {
        pthread_join(sender->thread, NULL);
    }

    LOGI("LinkAndroid preview sender destroyed");
}
