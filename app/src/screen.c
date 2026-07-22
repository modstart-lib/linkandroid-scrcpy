#include "screen.h"

#include <assert.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef HAVE_SDL3_IMAGE
#include <SDL3_image/SDL_image.h>
#endif

#ifdef HAVE_SDL3_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"
#include "util/sdl.h"

// LinkAndroid: WebSocket event forwarding
#include "input_manager.h"
#include "../../linkandroid/src/json/cJSON.h"
#include "../../linkandroid/src/websocket_client.h"

// External declaration for WebSocket client
extern struct la_websocket_client *g_websocket_client;

#define DISPLAY_MARGINS 96
#define PANEL_WIDTH 60
#define PANEL_BUTTON_HEIGHT 30
#define PANEL_START_Y 10
#define PANEL_BUTTON_MARGIN 10
#define PANEL_FONT_SIZE 12

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

// Forward declarations
static void sc_screen_render_panel(struct sc_screen *screen);

static void
set_aspect_ratio(struct sc_screen *screen, struct sc_size content_size) {
    assert(content_size.width && content_size.height);

    if (screen->window_aspect_ratio_lock) {
        // When the panel is visible, do NOT lock the window aspect ratio.
        // The panel adds extra width that would make a fixed AR incorrect
        // (the correct AR = content_AR + 60/H, which depends on window height).
        // compute_content_rect() already handles content AR inside the
        // remaining space after subtracting the panel.
        if (screen->panel.visible) {
            return;
        }
        float ar = (float) content_size.width / content_size.height;
        bool ok = SDL_SetWindowAspectRatio(screen->window, ar, ar);
        if (!ok) {
            LOGW("Could not set window aspect ratio: %s", SDL_GetError());
        }
    }
}

static inline struct sc_size
get_oriented_size(struct sc_size size, enum sc_orientation orientation)
{
    struct sc_size oriented_size;
    if (sc_orientation_is_swap(orientation))
    {
        oriented_size.width = size.height;
        oriented_size.height = size.width;
    }
    else
    {
        oriented_size.width = size.width;
        oriented_size.height = size.height;
    }
    return oriented_size;
}

static inline bool
is_windowed(struct sc_screen *screen) {
    return !(SDL_GetWindowFlags(screen->window) & (SDL_WINDOW_FULLSCREEN
                                                 | SDL_WINDOW_MINIMIZED
                                                 | SDL_WINDOW_MAXIMIZED));
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds)
{
    SDL_Rect rect;
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (!display) {
        LOGW("Could not get primary display: %s", SDL_GetError());
        return false;
    }

    bool ok = SDL_GetDisplayUsableBounds(display, &rect);
    if (!ok) {
        LOGW("Could not get display usable bounds: %s", SDL_GetError());
        return false;
    }

    bounds->width = MAX(0, rect.w - DISPLAY_MARGINS);
    bounds->height = MAX(0, rect.h - DISPLAY_MARGINS);
    return true;
}

static bool
is_optimal_size(struct sc_size current_size, struct sc_size content_size)
{
    // The size is optimal if we can recompute one dimension of the current
    // size from the other
    return current_size.height == (uint32_t) current_size.width
                                * content_size.height / content_size.width
        || current_size.width == (uint32_t) current_size.height
                               * content_size.width / content_size.height;
}

// return the optimal size of the window, with the following constraints:
//  - it attempts to keep at least one dimension of the current_size (i.e. it
//    crops the black borders)
//  - it keeps the aspect ratio
//  - it scales down to make it fit in the display_size
static struct sc_size
get_optimal_size(struct sc_size current_size, struct sc_size content_size,
                 bool within_display_bounds)
{
    if (content_size.width == 0 || content_size.height == 0)
    {
        // avoid division by 0
        return current_size;
    }

    struct sc_size window_size;

    struct sc_size display_size;
    if (!within_display_bounds ||
        !get_preferred_display_bounds(&display_size))
    {
        // do not constraint the size
        window_size = current_size;
    }
    else
    {
        window_size.width = MIN(current_size.width, display_size.width);
        window_size.height = MIN(current_size.height, display_size.height);
    }

    if (is_optimal_size(window_size, content_size))
    {
        return window_size;
    }

    bool keep_width = (uint32_t) content_size.width * window_size.height
                    > (uint32_t) content_size.height * window_size.width;
    if (keep_width) {
        // remove black borders on top and bottom
        window_size.height = (uint32_t) content_size.height * window_size.width
                           / content_size.width;
    } else {
        // remove black borders on left and right (or none at all if it already
        // fits)
        window_size.width = (uint32_t) content_size.width * window_size.height
                          / content_size.height;
    }

    return window_size;
}

// initially, there is no current size, so use the frame size as current size
// req_width and req_height, if not 0, are the sizes requested by the user
static inline struct sc_size
get_initial_optimal_size(struct sc_size content_size, uint16_t req_width,
                         uint16_t req_height)
{
    struct sc_size window_size;
    if (!req_width && !req_height)
    {
        window_size = get_optimal_size(content_size, content_size, true);
    }
    else
    {
        if (req_width)
        {
            window_size.width = req_width;
        }
        else
        {
            // compute from the requested height
            window_size.width = (uint32_t)req_height * content_size.width / content_size.height;
        }
        if (req_height)
        {
            window_size.height = req_height;
        }
        else
        {
            // compute from the requested width
            window_size.height = (uint32_t)req_width * content_size.height / content_size.width;
        }
    }
    return window_size;
}

static inline void
sc_screen_track_resize(struct sc_screen *screen, struct sc_size size) {
    LOGV("Track resize: %" PRIu16 "x%" PRIu16, size.width, size.height);
    screen->resize_tracker.time = sc_tick_now();
    screen->resize_tracker.size = size;
}

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen)
{
    return screen->im.mp && screen->im.mp->relative_mode;
}

// Get HiDPI-scaled panel width in drawable (physical) pixels
static inline int
sc_screen_get_panel_width_scaled(struct sc_screen *screen)
{
    if (!screen->panel.visible)
    {
        return 0;
    }
    int ww, wh, dw, dh;
    SDL_GetWindowSize(screen->window, &ww, &wh);
    SDL_GetWindowSizeInPixels(screen->window, &dw, &dh);
    float hidpi_scale = (float)dw / ww;
    return (int)(PANEL_WIDTH * hidpi_scale);
}

static void
compute_content_rect(struct sc_size window_size, struct sc_size content_size,
                     bool is_icon, enum sc_render_fit render_fit,
                     SDL_FRect *rect) {
    if (is_icon) {
        if (content_size.width <= window_size.width
                && content_size.height <= window_size.height) {
            // Center without upscaling
            rect->x = (window_size.width - content_size.width) / 2.f;
            rect->y = (window_size.height - content_size.height) / 2.f;
            rect->w = content_size.width;
            rect->h = content_size.height;
            return;
        }
    } else if (render_fit == SC_RENDER_FIT_UNSCALED) {
        // Cast to float first because input sizes are unsigned
        float x = ((float) window_size.width - content_size.width) / 2.f;
        float y = ((float) window_size.height - content_size.height) / 2.f;
        rect->x = MAX(0, x);
        rect->y = MAX(0, y);
        rect->w = content_size.width;
        rect->h = content_size.height;
        return;
    } else if (render_fit == SC_RENDER_FIT_STRETCHED) {
        rect->x = 0;
        rect->y = 0;
        rect->w = window_size.width;
        rect->h = window_size.height;
        return;
    }

    assert(is_icon || render_fit == SC_RENDER_FIT_LETTERBOX);

    if (is_optimal_size(window_size, content_size)) {
        rect->x = 0;
        rect->y = 0;
        rect->w = window_size.width;
        rect->h = window_size.height;
        return;
    }

    bool keep_width = (uint32_t) content_size.width * window_size.height
                    > (uint32_t) content_size.height * window_size.width;
    if (keep_width) {
        rect->x = 0;
        rect->w = window_size.width;
        rect->h = (float) window_size.width * content_size.height
                                            / content_size.width;
        rect->y = (window_size.height - rect->h) / 2.f;
    } else {
        rect->y = 0;
        rect->h = window_size.height;
        rect->w = (float) window_size.height * content_size.width
                                             / content_size.height;
        rect->x = (window_size.width - rect->w) / 2.f;
    }
}

static void
sc_screen_update_content_rect(struct sc_screen *screen) {
    bool is_icon = !screen->video || screen->disconnected;

    // Get window size in LOGICAL points (from SDL_GetWindowSize)
    // SDL3 rendering uses physical pixels, so we need to convert
    struct sc_size window_size = sc_sdl_get_window_size(screen->window);

    // LinkAndroid: Subtract panel width from available window area
    // The panel width is in physical pixels, convert to logical points
    int panel_width_px = sc_screen_get_panel_width_scaled(screen);
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale == 0) {
        scale = 1;
    }
    // Convert physical pixel panel width to logical point equivalent
    int panel_width_pt = (int)(panel_width_px / scale);
    if (panel_width_pt > 0 && window_size.width > (uint32_t)panel_width_pt) {
        window_size.width -= panel_width_pt;
    }

    compute_content_rect(window_size, screen->content_size, is_icon,
                         screen->render_fit, &screen->rect);
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect) {
    if (!screen->window_shown) {
        return;
    }

    if (update_content_rect || screen->panel_layout_dirty)
    {
        screen->panel_layout_dirty = false;
        sc_screen_update_content_rect(screen);
    }

    SDL_Renderer *renderer = screen->renderer;
    struct sc_screen_bg_color bg = screen->bg;
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, 0);
    sc_sdl_render_clear(renderer);

    SDL_Texture *texture = screen->tex.texture;
    if (texture) {
        float scale = SDL_GetWindowPixelDensity(screen->window);
        if (scale == 0) {
            LOGE("Cannot get scale value: %s", SDL_GetError());
            scale = 1;
        }

        SDL_FRect geometry = {
            .x = screen->rect.x * scale,
            .y = screen->rect.y * scale,
            .w = screen->rect.w * scale,
            .h = screen->rect.h * scale,
        };
        enum sc_orientation orientation = screen->orientation;

        bool ok = false;
        if (orientation == SC_ORIENTATION_0) {
            geometry.x = (int32_t) geometry.x;
            geometry.y = (int32_t) geometry.y;
            ok = SDL_RenderTexture(renderer, texture, NULL, &geometry);
        } else {
            unsigned cw_rotation = sc_orientation_get_rotation(orientation);
            double angle = 90 * cw_rotation;

            SDL_FRect *dstrect = NULL;
            SDL_FRect rect;
            if (sc_orientation_is_swap(orientation)) {
                rect.x = geometry.x + (geometry.w - geometry.h) / 2.f;
                rect.y = geometry.y + (geometry.h - geometry.w) / 2.f;
                rect.w = geometry.h;
                rect.h = geometry.w;
                dstrect = &rect;
            } else {
                dstrect = &geometry;
            }

            SDL_FlipMode flip = sc_orientation_is_mirror(orientation)
                                  ? SDL_FLIP_HORIZONTAL : 0;

            dstrect->x = (int32_t) dstrect->x;
            dstrect->y = (int32_t) dstrect->y;
            ok = SDL_RenderTextureRotated(renderer, texture, NULL, dstrect,
                                          angle, NULL, flip);
        }

        if (!ok) {
            LOGE("Could not render texture: %s", SDL_GetError());
        }
    }

    // LinkAndroid: Render panel if visible
    if (screen->panel.visible && screen->panel.button_count > 0)
    {
        sc_screen_render_panel(screen);
    }

    sc_sdl_render_present(renderer);

    // LinkAndroid: Send ready event after the first frame is completely rendered
    if (!screen->ready_event_sent && texture && g_websocket_client)
    {
        const char *ready_event = "{\"type\":\"ready\"}";
        if (la_websocket_client_send(g_websocket_client, ready_event))
        {
            LOGI("LinkAndroid: Sent ready event to WebSocket server");
            screen->ready_event_sent = true;
        }
    }
}

#ifdef HAVE_SDL3_IMAGE
static bool
sc_screen_load_button_icon(struct sc_screen *screen, struct sc_panel_button *button)
{
    if (button->icon_texture)
    {
        return true; // Already loaded
    }

    if (!screen->icon_root_path || button->icon[0] == '\0')
    {
        return false;
    }

    // Build the icon path from icon_root_path
    // +1 for '/', +4 for '.png', +1 for '\0' = +6
    size_t path_len = strlen(screen->icon_root_path) + strlen(button->icon) + 6;
    char *icon_path = malloc(path_len);
    if (!icon_path)
    {
        LOG_OOM();
        return false;
    }
    snprintf(icon_path, path_len, "%s/%s.png", screen->icon_root_path, button->icon);

    SDL_Surface *icon_surface = IMG_Load(icon_path);
    free(icon_path);
    if (!icon_surface)
    {
        LOGW("Failed to load icon '%s.png': %s", button->icon, SDL_GetError());
        return false;
    }

    SDL_Texture *icon_texture = SDL_CreateTextureFromSurface(screen->renderer, icon_surface);
    SDL_DestroySurface(icon_surface);
    if (!icon_texture)
    {
        LOGW("Failed to create texture for icon '%s'", button->icon);
        return false;
    }

    float icon_w = 0, icon_h = 0;
    SDL_GetTextureSize(icon_texture, &icon_w, &icon_h);
    LOGD("Loaded icon '%s.png': %.0fx%.0f", button->icon, icon_w, icon_h);

    button->icon_texture = icon_texture;
    return true;
}
#endif

// Render the right-side panel with buttons
static void
sc_screen_render_panel(struct sc_screen *screen)
{
    SDL_Renderer *renderer = screen->renderer;
    if (!renderer)
    {
        return;
    }

    // SDL3 rendering uses physical pixel coordinates (no logical presentation set)
    // We need to multiply logical coordinates by the pixel density scale
    float scale = SDL_GetWindowPixelDensity(screen->window);
    if (scale == 0) {
        scale = 1;
    }

    int dw, dh;
    SDL_GetWindowSizeInPixels(screen->window, &dw, &dh);

    int panel_w = sc_screen_get_panel_width_scaled(screen);

    // Convert logical rect positions to physical pixels
    float content_right = (screen->rect.x + screen->rect.w) * scale;

    int panel_x = (int)content_right;
    int panel_y = 0;
    int panel_h = dh;

    // Compute button dimensions in physical pixels
    float hidpi_scale = scale;
    int button_margin = (int)(PANEL_BUTTON_MARGIN * hidpi_scale);
    int button_height = (int)(PANEL_BUTTON_HEIGHT * hidpi_scale);
    int start_y = (int)(PANEL_START_Y * hidpi_scale);

    SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255);
    SDL_FRect panel_rect = {panel_x, panel_y, panel_w, panel_h};
    SDL_RenderFillRect(renderer, &panel_rect);

    int button_width = panel_w - 2 * button_margin;

    // Convert the logical content top to physical pixels for button Y positions
    float content_top = screen->rect.y * scale;

    for (int i = 0; i < screen->panel.button_count; i++)
    {
        int btn_x = panel_x + button_margin;
        int btn_y = (int)content_top + start_y + i * (button_height + button_margin);

        bool is_hovered = (i == screen->panel.hovered_button_index);
        if (is_hovered) {
            SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255);
        } else {
            SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255);
        }
        SDL_FRect button_rect = {btn_x, btn_y, button_width, button_height};
        SDL_RenderFillRect(renderer, &button_rect);

        bool rendered = false;

#ifdef HAVE_SDL3_IMAGE
        if (screen->panel.buttons[i].icon[0] != '\0')
        {
            if (sc_screen_load_button_icon(screen, &screen->panel.buttons[i]))
            {
                SDL_Texture *icon_texture = screen->panel.buttons[i].icon_texture;
                if (icon_texture)
                {
                    float icon_w_f, icon_h_f;
                    SDL_GetTextureSize(icon_texture, &icon_w_f, &icon_h_f);

                    int target_size = button_height - 10 * (int)hidpi_scale;
                    float scale = (float)target_size / (icon_w_f > icon_h_f ? icon_w_f : icon_h_f);
                    int scaled_w = (int)(icon_w_f * scale);
                    int scaled_h = (int)(icon_h_f * scale);

                    int icon_x = btn_x + (button_width - scaled_w) / 2;
                    int icon_y = btn_y + (button_height - scaled_h) / 2;

                    SDL_FRect icon_rect = {(float)icon_x, (float)icon_y, (float)scaled_w, (float)scaled_h};
                    SDL_RenderTexture(renderer, icon_texture, NULL, &icon_rect);
                    rendered = true;
                }
            }
        }
#endif

#ifdef HAVE_SDL3_TTF
        if (!rendered && screen->panel_font && screen->panel.buttons[i].text[0] != '\0')
        {
            SDL_Color text_color = {255, 255, 255, 255};
            const char *btn_text = screen->panel.buttons[i].text;
            SDL_Surface *text_surface = TTF_RenderText_Blended(
                screen->panel_font,
                btn_text,
                strlen(btn_text),
                text_color);

            if (text_surface)
            {
                SDL_Texture *text_texture = SDL_CreateTextureFromSurface(renderer, text_surface);
                if (text_texture)
                {
                    float text_w = 0, text_h = 0;
                    SDL_GetTextureSize(text_texture, &text_w, &text_h);
                    int text_x = btn_x + (button_width - (int)text_w) / 2;
                    int text_y = btn_y + (button_height - (int)text_h) / 2;

                    SDL_FRect text_rect = {(float)text_x, (float)text_y, text_w, text_h};
                    SDL_RenderTexture(renderer, text_texture, NULL, &text_rect);
                    SDL_DestroyTexture(text_texture);
                    rendered = true;
                }
                SDL_DestroySurface(text_surface);
            }
        }
#endif
        (void)rendered;
    }
}

static void
sc_screen_on_resize(struct sc_screen *screen, const SDL_WindowEvent *event) {
    if (!screen->window_shown) {
        return;
    }

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        sc_screen_render(screen, true);
    } else {
        assert(event->type == SDL_EVENT_WINDOW_RESIZED);
        if (screen->flex_display) {
            assert(!(event->data1 & ~0xFFFF));
            assert(!(event->data2 & ~0xFFFF));
            uint16_t width = event->data1;
            uint16_t height = event->data2;

            struct sc_resize_tracker *tracker = &screen->resize_tracker;
            if (tracker->time
                    && sc_tick_now() >= tracker->time + SC_TICK_FROM_MS(3000)) {
                tracker->time = 0;
            }
            if (tracker->time && tracker->size.width == width
                              && tracker->size.height == height) {
                LOGV("Ignore local resize: %" PRIu16 "x%" PRIu16,
                     width, height);
                tracker->time = 0;
            } else {
                sc_controller_resize_display(screen->controller, width, height);
            }
        }
    }
}

#if defined(__APPLE__) || defined(_WIN32)
#define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
static bool
event_watcher(void *data, SDL_Event *event) {
    struct sc_screen *screen = data;

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED
            || event->type == SDL_EVENT_WINDOW_RESIZED) {
        sc_screen_on_resize(screen, &event->window);
    }

    return true;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx,
                          const struct sc_stream_session *session) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);

    struct sc_screen *screen = DOWNCAST(sink);

    // LinkAndroid: Set device size for WebSocket event forwarding
    sc_input_manager_set_device_size(ctx->width, ctx->height);

    assert(session->video.width && session->video.height);
    if (session->video.width > 0xFFFF || session->video.height > 0xFFFF) {
        LOGE("Size too large: %" PRIu32 "x%" PRIu32, session->video.width,
                                                     session->video.height);
        return false;
    }

    struct sc_size *size = malloc(sizeof(*size));
    if (!size) {
        LOG_OOM();
        return false;
    }
    size->width = session->video.width;
    size->height = session->video.height;

    bool ok = sc_push_event_with_data(SC_EVENT_OPEN_WINDOW, size);
    if (!ok) {
        free(size);
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    return true;
}

static void
sc_screen_frame_sink_close(struct sc_frame_sink *sink)
{
    struct sc_screen *screen = DOWNCAST(sink);
    (void)screen;
#ifndef NDEBUG
    screen->open = false;
#endif
}

static bool
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame)
{
    struct sc_screen *screen = DOWNCAST(sink);

    sc_mutex_lock(&screen->mutex);
    bool previous_skipped = sc_frame_buffer_has_frame(&screen->fb);
    bool ok = sc_frame_buffer_push(&screen->fb, frame);
    screen->prevent_auto_resize = screen->current_session.video.client_resized;
    sc_mutex_unlock(&screen->mutex);
    if (!ok) {
        return false;
    }

    if (previous_skipped)
    {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
    }
    else
    {
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok)
        {
            return false;
        }
    }

    return true;
}

static bool
sc_screen_frame_sink_push_session(struct sc_frame_sink *sink,
                                  const struct sc_stream_session *session) {
    struct sc_screen *screen = DOWNCAST(sink);
    screen->current_session = *session;
    return true;
}

bool
sc_screen_init(struct sc_screen *screen,
               const struct sc_screen_params *params) {
    screen->controller = params->controller;

    screen->resize_pending = false;
    screen->window_shown = false;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;
    screen->ready_event_sent = false;
    screen->disconnected = false;
    screen->disconnect_started = false;

    screen->video = params->video;
    screen->camera = params->camera;
    screen->window_aspect_ratio_lock = params->window_aspect_ratio_lock;
    screen->render_fit = params->render_fit;
    screen->flex_display = params->flex_display;

    screen->bg.r = (params->background_color >> 16) & 0xFF;
    screen->bg.g = (params->background_color >> 8) & 0xFF;
    screen->bg.b = params->background_color & 0xFF;

    // Initialize panel configuration
    screen->panel.button_count = 0;
    screen->panel.visible = params->panel_show;
    screen->panel_layout_dirty = false;
    screen->panel_enabled = params->panel_show;
    screen->panel.hovered_button_index = -1;
    memset(screen->panel.buttons, 0, sizeof(screen->panel.buttons));
    screen->panel_font = NULL;

    screen->hand_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    screen->arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    screen->cursor_is_hand = false;
    screen->mouse_button_pressed_outside_panel = false;

    if (!screen->hand_cursor || !screen->arrow_cursor)
    {
        LOGW("Could not create system cursors: %s", SDL_GetError());
    }

    screen->req.x = params->window_x;
    screen->req.y = params->window_y;
    screen->req.width = params->window_width;
    screen->req.height = params->window_height;
    screen->req.fullscreen = params->fullscreen;
    screen->req.start_fps_counter = params->start_fps_counter;
    screen->req.hide_window = params->hide_window;

    screen->prevent_auto_resize = false;

    screen->resize_tracker.time = 0;
    screen->resize_tracker.size.width = 0;
    screen->resize_tracker.size.height = 0;

    bool ok = sc_mutex_init(&screen->mutex);
    if (!ok) {
        return false;
    }

    ok = sc_frame_buffer_init(&screen->fb);
    if (!ok) {
        goto error_destroy_mutex;
    }

    if (!sc_fps_counter_init(&screen->fps_counter)) {
        goto error_destroy_frame_buffer;
    }

    if (screen->video)
    {
        screen->orientation = params->orientation;
        if (screen->orientation != SC_ORIENTATION_0)
        {
            LOGI("Initial display orientation set to %s",
                 sc_orientation_get_name(screen->orientation));
        }
    }

    uint32_t window_flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (params->always_on_top)
    {
        window_flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (params->window_borderless)
    {
        window_flags |= SDL_WINDOW_BORDERLESS;
    }
    if (params->video)
    {
        window_flags |= SDL_WINDOW_RESIZABLE;
    }

    const char *title = params->window_title;
    assert(title);

    int x = SDL_WINDOWPOS_UNDEFINED;
    int y = SDL_WINDOWPOS_UNDEFINED;
    int width = 256;
    int height = 256;
    if (params->window_x != SC_WINDOW_POSITION_UNDEFINED)
    {
        x = params->window_x;
    }
    if (params->window_y != SC_WINDOW_POSITION_UNDEFINED)
    {
        y = params->window_y;
    }
    if (params->window_width)
    {
        width = params->window_width;
    }
    if (params->window_height)
    {
        height = params->window_height;
    }

    screen->window =
        sc_sdl_create_window(title, x, y, width, height, window_flags);
    if (!screen->window) {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    screen->renderer = SDL_CreateRenderer(screen->window, NULL);
    if (!screen->renderer) {
        LOGE("Could not create renderer: %s", SDL_GetError());
        goto error_destroy_window;
    }

#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    screen->gl_context = NULL;

    const char *renderer_name = SDL_GetRendererName(screen->renderer);
    bool use_opengl = renderer_name && !strncmp(renderer_name, "opengl", 6);
    if (use_opengl) {
        bool ok = SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                                      SDL_GL_CONTEXT_PROFILE_CORE);
        if (!ok) {
            LOGW("Could not set a GL Core Profile Context");
        }

        LOGD("Creating OpenGL Core Profile context");
        screen->gl_context = SDL_GL_CreateContext(screen->window);
        if (!screen->gl_context) {
            LOGE("Could not create OpenGL context: %s", SDL_GetError());
            goto error_destroy_renderer;
        }
    }
#endif

    bool mipmaps = params->video;
    ok = sc_texture_init(&screen->tex, screen->renderer, mipmaps);
    if (!ok) {
        goto error_destroy_renderer;
    }

    ok = SDL_StartTextInput(screen->window);
    if (!ok) {
        LOGE("Could not enable text input: %s", SDL_GetError());
        goto error_destroy_texture;
    }

    SDL_Surface *icon = sc_icon_load(SC_ICON_FILENAME_SCRCPY);
    if (icon) {
        if (!SDL_SetWindowIcon(screen->window, icon)) {
            LOGW("Could not set window icon: %s", SDL_GetError());
        }

        if (!params->video) {
            screen->content_size.width = icon->w;
            screen->content_size.height = icon->h;
            ok = sc_texture_set_from_surface(&screen->tex, icon);
            if (!ok) {
                LOGE("Could not set icon: %s", SDL_GetError());
            }
        }

        sc_icon_destroy(icon);
    } else {
        LOGE("Could not load icon");

        if (!params->video) {
            screen->content_size.width = 256;
            screen->content_size.height = 256;
        }
    }

#ifdef HAVE_SDL3_TTF
    if (!TTF_Init())
    {
        LOGW("Could not initialize SDL_ttf: %s", SDL_GetError());
    }
    else
    {
        int ww, wh, dw, dh;
        SDL_GetWindowSize(screen->window, &ww, &wh);
        SDL_GetWindowSizeInPixels(screen->window, &dw, &dh);
        float hidpi_scale = (float)dw / ww;
        float scaled_font_size = PANEL_FONT_SIZE * hidpi_scale;

        LOGI("Font size: base=%d, hidpi_scale=%.2f, scaled=%.1f",
             PANEL_FONT_SIZE, hidpi_scale, scaled_font_size);

        const char *env_font_path = getenv("SCRCPY_FONT_PATH");
        if (env_font_path)
        {
            screen->panel_font = TTF_OpenFont(env_font_path, scaled_font_size);
            if (screen->panel_font)
            {
                LOGI("Loaded custom font from SCRCPY_FONT_PATH: %s", env_font_path);
            }
        }

        if (!screen->panel_font)
        {
            const char *font_search_paths[] = {
                NULL,
                "data/font.ttf",
                "../share/scrcpy/font.ttf",
                NULL
            };

            const char *base_path = SDL_GetBasePath();
            char *base_font_path = NULL;

            if (base_path)
            {
                size_t path_len = strlen(base_path) + strlen("data/font.ttf") + 1;
                base_font_path = malloc(path_len);
                if (base_font_path)
                {
                    snprintf(base_font_path, path_len, "%sdata/font.ttf", base_path);
                    font_search_paths[0] = base_font_path;
                }
            }

            for (int i = 0; font_search_paths[i] != NULL; i++)
            {
                screen->panel_font = TTF_OpenFont(font_search_paths[i], scaled_font_size);
                if (screen->panel_font)
                {
                    LOGI("Loaded custom font: %s", font_search_paths[i]);
                    break;
                }
            }

            if (base_font_path)
            {
                free(base_font_path);
            }
            if (base_path)
            {
                SDL_free((void *)base_path);
            }
        }

        if (!screen->panel_font)
        {
            const char *font_paths[] = {
                "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                "/System/Library/Fonts/Helvetica.ttc",
                "/System/Library/Fonts/PingFang.ttc",
                "/System/Library/Fonts/SFNS.ttf",
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                "C:\\Windows\\Fonts\\seguisym.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\msyh.ttc",
                NULL
            };

            for (int i = 0; font_paths[i] != NULL; i++)
            {
                screen->panel_font = TTF_OpenFont(font_paths[i], scaled_font_size);
                if (screen->panel_font)
                {
                    LOGI("Loaded system font: %s", font_paths[i]);
                    break;
                }
            }
        }

        if (!screen->panel_font)
        {
            LOGW("Could not load any font for panel buttons");
        }
    }
#endif

    screen->icon_root_path = NULL;
    char *env_icon_root = getenv("SCRCPY_ICON_ROOT_PATH");
    if (env_icon_root)
    {
        screen->icon_root_path = strdup(env_icon_root);
        if (screen->icon_root_path)
        {
            LOGI("Icon root path set from SCRCPY_ICON_ROOT_PATH: %s", screen->icon_root_path);
        }
    }

    if (!screen->icon_root_path)
    {
        const char *icon_search_paths[] = {
            NULL,
            "app/data",
            "../share/scrcpy",
            NULL
        };

        const char *base_path = SDL_GetBasePath();
        char *base_icon_path = NULL;

        if (base_path)
        {
            size_t path_len = strlen(base_path) + strlen("data") + 1;
            base_icon_path = malloc(path_len);
            if (base_icon_path)
            {
                snprintf(base_icon_path, path_len, "%sdata", base_path);
                icon_search_paths[0] = base_icon_path;
            }
        }

        for (int i = 0; icon_search_paths[i] != NULL; i++)
        {
            size_t test_path_len = strlen(icon_search_paths[i]) + strlen("/home.png") + 1;
            char *test_path = malloc(test_path_len);
            if (test_path)
            {
                snprintf(test_path, test_path_len, "%s/home.png", icon_search_paths[i]);
                FILE *f = fopen(test_path, "rb");
                free(test_path);
                if (f)
                {
                    fclose(f);
                    screen->icon_root_path = strdup(icon_search_paths[i]);
                    if (screen->icon_root_path)
                    {
                        LOGI("Found icon root path: %s", screen->icon_root_path);
                        break;
                    }
                }
            }
        }

        if (base_icon_path)
        {
            free(base_icon_path);
        }
        if (base_path)
        {
            SDL_free(base_path);
        }
    }

    if (!screen->icon_root_path)
    {
        LOGW("Icon root path not found, icon buttons will not be displayed");
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame)
    {
        LOG_OOM();
        goto error_destroy_texture;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .camera = params->camera,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video) {
        ok = SDL_AddEventWatch(event_watcher, screen);
        if (!ok) {
            LOGW("Could not add event watcher for continuous resizing: %s",
                 SDL_GetError());
        }
    }
#endif

    memset(&screen->current_session, 0, sizeof(screen->current_session));

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
        .push_session = sc_screen_frame_sink_push_session,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (!screen->video && !screen->req.hide_window) {
        screen->window_shown = true;
        sc_sdl_show_window(screen->window);

        if (sc_screen_is_relative_mode(screen)) {
            sc_mouse_capture_set_active(&screen->mc, true);
        }
    }

    return true;

error_destroy_texture:
    sc_texture_destroy(&screen->tex);
error_destroy_renderer:
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    if (screen->gl_context) {
        SDL_GL_DestroyContext(screen->gl_context);
    }
#endif
    SDL_DestroyRenderer(screen->renderer);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);
error_destroy_mutex:
    sc_mutex_destroy(&screen->mutex);

    return false;
}

static void
sc_screen_show_initial_window(struct sc_screen *screen)
{
    // LinkAndroid: Don't show window if hide_window is requested
    if (screen->req.hide_window)
    {
        sc_screen_update_content_rect(screen);
        return;
    }

    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
                ? screen->req.x
                : (int)SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
                ? screen->req.y
                : (int)SDL_WINDOWPOS_CENTERED;
    struct sc_point position = {
        .x = x,
        .y = y,
    };

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                 screen->req.height);

    // LinkAndroid: Add panel width to window size if panel is visible.
    // We do this AFTER get_initial_optimal_size (which computes optimal
    // size for the content only), so the content area remains at the
    // optimal size and the panel is extra width. The window aspect ratio
    // is NOT locked when the panel is visible (see set_aspect_ratio), so
    // SDL won't fight this added width.
    if (screen->panel.visible)
    {
        window_size.width += PANEL_WIDTH;
    }

    if (screen->flex_display
            && window_size.width == screen->content_size.width
            && window_size.height == screen->content_size.height) {
        sc_screen_track_resize(screen, window_size);
    }

    assert(is_windowed(screen));
    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, window_size);
    sc_sdl_set_window_position(screen->window, position);

    if (screen->req.fullscreen)
    {
        sc_screen_toggle_fullscreen(screen);
    }

    if (screen->req.start_fps_counter)
    {
        sc_fps_counter_start(&screen->fps_counter);
    }

    screen->window_shown = true;
    sc_sdl_show_window(screen->window);
    sc_screen_update_content_rect(screen);
}

void
sc_screen_hide_window(struct sc_screen *screen) {
    sc_sdl_hide_window(screen->window);
    screen->window_shown = false;
}

void sc_screen_raise_window(struct sc_screen *screen)
{
    if (!screen->window) {
        LOGW("Cannot raise window: window not initialized");
        return;
    }
    SDL_RaiseWindow(screen->window);
    LOGI("Window raised to front");
}

void sc_screen_set_always_on_top(struct sc_screen *screen, bool enable)
{
    if (!screen->window) {
        LOGW("Cannot set always-on-top: window not initialized");
        return;
    }
    SDL_SetWindowAlwaysOnTop(screen->window, enable);
    LOGI("Window always-on-top: %s", enable ? "enabled" : "disabled");
}

// Called on the main thread to enlarge window for panel without squeezing content.
// The window aspect ratio is NOT locked when panel is visible (see set_aspect_ratio),
// so we only need to widen the window. compute_content_rect() will handle the
// content aspect ratio inside the remaining space.
static void
task_enlarge_window_for_panel(void *userdata) {
    struct sc_screen *screen = userdata;
    if (!screen->window_shown || !screen->panel.visible || screen->panel.button_count == 0) {
        return;
    }
    struct sc_size window_size = sc_sdl_get_window_size(screen->window);
    if (window_size.width < screen->rect.w + PANEL_WIDTH) {
        window_size.width += PANEL_WIDTH;
        sc_sdl_set_window_size(screen->window, window_size);
    }
}

void sc_screen_interrupt(struct sc_screen *screen)
{
    sc_fps_counter_interrupt(&screen->fps_counter);
}

static void
sc_screen_interrupt_disconnect(struct sc_screen *screen) {
    if (screen->disconnect_started) {
        sc_disconnect_interrupt(&screen->disconnect);
    }
}

void
sc_screen_join(struct sc_screen *screen) {
    sc_fps_counter_join(&screen->fps_counter);
    if (screen->disconnect_started) {
        sc_disconnect_join(&screen->disconnect);
    }
}

void sc_screen_destroy(struct sc_screen *screen)
{
#ifndef NDEBUG
    assert(!screen->open);
#endif

    if (screen->disconnect_started) {
        sc_disconnect_destroy(&screen->disconnect);
    }

#ifdef HAVE_SDL3_TTF
    if (screen->panel_font)
    {
        TTF_CloseFont(screen->panel_font);
        screen->panel_font = NULL;
    }
    TTF_Quit();
#endif

    for (int i = 0; i < screen->panel.button_count; i++)
    {
        if (screen->panel.buttons[i].icon_texture)
        {
            SDL_DestroyTexture(screen->panel.buttons[i].icon_texture);
            screen->panel.buttons[i].icon_texture = NULL;
        }
    }

    if (screen->icon_root_path)
    {
        free(screen->icon_root_path);
        screen->icon_root_path = NULL;
    }

    if (screen->hand_cursor)
    {
        SDL_DestroyCursor(screen->hand_cursor);
    }
    if (screen->arrow_cursor)
    {
        SDL_DestroyCursor(screen->arrow_cursor);
    }

    sc_texture_destroy(&screen->tex);
    av_frame_free(&screen->frame);
#ifdef SC_DISPLAY_FORCE_OPENGL_CORE_PROFILE
    SDL_GL_DestroyContext(screen->gl_context);
#endif
    SDL_DestroyRenderer(screen->renderer);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
    sc_mutex_destroy(&screen->mutex);

    SDL_Event event;
    bool has_event =
        sc_dequeue_event(SC_EVENT_DISCONNECTED_ICON_LOADED, &event);
    if (has_event) {
        assert(event.type == SC_EVENT_DISCONNECTED_ICON_LOADED);
        SDL_Surface *dangling_icon = event.user.data1;
        sc_icon_destroy(dangling_icon);
    }

    has_event = sc_dequeue_event(SC_EVENT_OPEN_WINDOW, &event);
    if (has_event) {
        assert(event.type == SC_EVENT_OPEN_WINDOW);
        struct sc_size * size = event.user.data1;
        free(size);
    }
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size)
{
    assert(screen->video);

    struct sc_size target_size = new_content_size;
    if (!screen->flex_display) {
        struct sc_size window_size = sc_sdl_get_window_size(screen->window);
        target_size.width = (uint32_t) window_size.width * target_size.width
                          / old_content_size.width;
        target_size.height = (uint32_t) window_size.height * target_size.height
                           / old_content_size.height;
    }
    target_size = get_optimal_size(target_size, new_content_size, true);
    assert(is_windowed(screen));
    set_aspect_ratio(screen, new_content_size);
    sc_sdl_set_window_size(screen->window, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size,
                 bool resize) {
    assert(screen->video);

    if (resize) {
        if (is_windowed(screen)) {
            resize_for_content(screen, screen->content_size, new_content_size);
        } else if (screen->flex_display) {
            struct sc_size size = sc_sdl_get_window_size(screen->window);
            sc_controller_resize_display(screen->controller, size.width, size.height);
        } else if (!screen->resize_pending) {
            screen->windowed_content_size = screen->content_size;
            screen->resize_pending = true;
        }
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen)
{
    assert(screen->video);

    assert(is_windowed(screen));
    if (screen->resize_pending) {
        resize_for_content(screen, screen->windowed_content_size,
                           screen->content_size);
        screen->resize_pending = false;
    }
}

void sc_screen_set_orientation(struct sc_screen *screen,
                               enum sc_orientation orientation)
{
    assert(screen->video);

    if (orientation == screen->orientation)
    {
        return;
    }

    struct sc_size new_content_size =
        get_oriented_size(screen->frame_size, orientation);

    set_content_size(screen, new_content_size, true);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen, bool can_resize) {
    assert(screen->video);
    if (!screen->window_shown) {
        return false;
    }

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    AVFrame *frame = screen->frame;
    struct sc_size new_frame_size = {frame->width, frame->height};

    if (!new_frame_size.width || !new_frame_size.height) {
        LOGE("Invalid frame size: %" PRIu32 "x%" PRIu32,
             new_frame_size.width, new_frame_size.height);
        return false;
    }

    if (screen->frame_size.width != new_frame_size.width
            || screen->frame_size.height != new_frame_size.height) {

        screen->frame_size = new_frame_size;

        struct sc_size new_content_size =
            get_oriented_size(new_frame_size, screen->orientation);

        if (screen->flex_display) {
            sc_screen_track_resize(screen, new_content_size);
        }

        set_content_size(screen, new_content_size, can_resize);
        sc_screen_update_content_rect(screen);
    }

    bool ok = sc_texture_set_from_frame(&screen->tex, frame);
    if (!ok) {
        return false;
    }

    sc_screen_render(screen, false);
    return true;
}

static bool
sc_screen_update_frame(struct sc_screen *screen)
{
    assert(screen->video);

    if (screen->paused)
    {
        if (!screen->resume_frame)
        {
            screen->resume_frame = av_frame_alloc();
            if (!screen->resume_frame)
            {
                LOG_OOM();
                return false;
            }
        }
        else
        {
            av_frame_unref(screen->resume_frame);
        }
        sc_mutex_lock(&screen->mutex);
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);
        sc_mutex_unlock(&screen->mutex);
        return true;
    }

    av_frame_unref(screen->frame);
    sc_mutex_lock(&screen->mutex);
    sc_frame_buffer_consume(&screen->fb, screen->frame);
    bool can_resize = !screen->prevent_auto_resize;
    sc_mutex_unlock(&screen->mutex);
    return sc_screen_apply_frame(screen, can_resize);
}

void sc_screen_set_paused(struct sc_screen *screen, bool paused)
{
    assert(screen->video);
    if (!screen->window_shown) {
        return;
    }

    if (!paused && !screen->paused)
    {
        return;
    }

    if (screen->paused && screen->resume_frame)
    {
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        bool ok = sc_screen_apply_frame(screen, true);
        if (!ok) {
            LOGE("Resume frame update failed");
        }
    }

    if (!paused)
    {
        LOGI("Display screen unpaused");
    }
    else if (!screen->paused)
    {
        LOGI("Display screen paused");
    }
    else
    {
        LOGI("Display screen re-paused");
    }

    screen->paused = paused;
}

void sc_screen_toggle_fullscreen(struct sc_screen *screen)
{
    assert(screen->video);

    bool req_fullscreen =
        !(SDL_GetWindowFlags(screen->window) & SDL_WINDOW_FULLSCREEN);

    bool ok = SDL_SetWindowFullscreen(screen->window, req_fullscreen);
    if (!ok) {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    LOGD("Requested %s mode", req_fullscreen ? "fullscreen" : "windowed");
}

void sc_screen_resize_to_fit(struct sc_screen *screen)
{
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    if (screen->render_fit == SC_RENDER_FIT_STRETCHED) {
        return;
    }

    struct sc_size window_size = sc_sdl_get_window_size(screen->window);

    if (screen->render_fit == SC_RENDER_FIT_UNSCALED) {
        struct sc_size content_size = screen->content_size;
        set_aspect_ratio(screen, content_size);
        sc_sdl_set_window_size(screen->window, content_size);

        int32_t x_offset = 0;
        if (content_size.width < window_size.width) {
            x_offset = (window_size.width - content_size.width) / 2;
        }
        int32_t y_offset = 0;
        if (content_size.height < window_size.height) {
            y_offset = (window_size.height - content_size.height) / 2;
        }
        assert(x_offset >= 0 && y_offset >= 0);
        if (x_offset || y_offset) {
            struct sc_point pos = sc_sdl_get_window_position(screen->window);
            pos.x += x_offset;
            pos.y += y_offset;
            sc_sdl_set_window_position(screen->window, pos);
        }

        LOGD("Resized to content size: %ux%u", content_size.width,
                                               content_size.height);
        return;
    }

    assert(screen->render_fit == SC_RENDER_FIT_LETTERBOX);

    struct sc_point point = sc_sdl_get_window_position(screen->window);

    struct sc_size optimal_size =
        get_optimal_size(window_size, screen->content_size, false);

    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);

    struct sc_point new_position = {
        .x = point.x + (window_size.width - optimal_size.width) / 2,
        .y = point.y + (window_size.height - optimal_size.height) / 2,
    };

    set_aspect_ratio(screen, screen->content_size);
    sc_sdl_set_window_size(screen->window, optimal_size);
    sc_sdl_set_window_position(screen->window, new_position);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
         optimal_size.height);
}

void sc_screen_resize_to_pixel_perfect(struct sc_screen *screen)
{
    assert(screen->video);

    if (!is_windowed(screen)) {
        return;
    }

    struct sc_size content_size = screen->content_size;
    set_aspect_ratio(screen, content_size);
    sc_sdl_set_window_size(screen->window, content_size);
    LOGD("Resized to pixel-perfect: %ux%u", content_size.width,
         content_size.height);
}

static void
sc_disconnect_on_icon_loaded(struct sc_disconnect *d, SDL_Surface *icon,
                             void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event_with_data(SC_EVENT_DISCONNECTED_ICON_LOADED, icon);
    if (!ok) {
        sc_icon_destroy(icon);
    }
}

static void
sc_disconnect_on_timeout(struct sc_disconnect *d, void *userdata) {
    (void) d;
    (void) userdata;

    bool ok = sc_push_event(SC_EVENT_DISCONNECTED_TIMEOUT);
    (void) ok;
}

void
sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event) {
    switch (event->type) {
        case SC_EVENT_OPEN_WINDOW: {
            struct sc_size *size = event->user.data1;
            assert(size);

            screen->frame_size = *size;
            free(size);
            screen->content_size = get_oriented_size(screen->frame_size,
                                                     screen->orientation);
            sc_screen_show_initial_window(screen);

            if (sc_screen_is_relative_mode(screen)) {
                sc_mouse_capture_set_active(&screen->mc, true);
            }

            sc_screen_render(screen, false);
            return;
        }
        case SC_EVENT_NEW_FRAME: {
            if (!screen->window_shown) {
                return;
            }
            bool ok = sc_screen_update_frame(screen);
            if (!ok) {
                LOGE("Frame update failed\n");
            }
            return;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
            sc_screen_render(screen, true);
            return;
#ifndef CONTINUOUS_RESIZING_WORKAROUND
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            sc_screen_on_resize(screen, &event->window);
            return;
#endif
        case SDL_EVENT_WINDOW_RESTORED:
            if (screen->video && is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            LOGD("Switched to fullscreen mode");
            assert(screen->video);
            return;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            LOGD("Switched to windowed mode");
            assert(screen->video);
            if (is_windowed(screen)) {
                apply_pending_resize(screen);
                sc_screen_render(screen, true);
            }
            return;
        case SC_EVENT_DEVICE_DISCONNECTED:
            assert(!screen->disconnected);
            screen->disconnected = true;
            if (!screen->window_shown) {
                return;
            }

            sc_input_manager_handle_event(&screen->im, event);

            sc_texture_reset(&screen->tex);
            sc_screen_render(screen, true);

            sc_tick deadline = sc_tick_now() + SC_TICK_FROM_SEC(2);
            static const struct sc_disconnect_callbacks cbs = {
                .on_icon_loaded = sc_disconnect_on_icon_loaded,
                .on_timeout = sc_disconnect_on_timeout,
            };
            bool ok =
                sc_disconnect_start(&screen->disconnect, deadline, &cbs, NULL);
            if (ok) {
                screen->disconnect_started = true;
            }

            return;
    }

    if (sc_screen_is_relative_mode(screen) && sc_mouse_capture_handle_event(&screen->mc, event))
    {
        return;
    }

    // LinkAndroid: Handle mouse motion for cursor change on panel hover
    if (event->type == SDL_EVENT_MOUSE_MOTION &&
        screen->panel.visible &&
        screen->panel.button_count > 0 &&
        screen->hand_cursor &&
        screen->arrow_cursor)
    {
        // SDL3 event coords are logical points; panel rendering uses physical
        // pixels, but hit‑testing is done in logical points for consistency
        // with the rest of the upstream code.
        int32_t x = event->motion.x;
        int32_t y = event->motion.y;

        // Panel left edge in logical points
        int panel_x = screen->rect.x + screen->rect.w;
        int panel_w = PANEL_WIDTH; // logical points
        bool over_button = false;
        int new_hovered_index = -1;

        if (x >= panel_x && x < panel_x + panel_w)
        {
            int button_margin = PANEL_BUTTON_MARGIN;
            int button_height = PANEL_BUTTON_HEIGHT;
            int button_width = panel_w - 2 * button_margin;
            int start_y = PANEL_START_Y;

            for (int i = 0; i < screen->panel.button_count; i++)
            {
                int btn_x = panel_x + button_margin;
                int btn_y = screen->rect.y + start_y + i * (button_height + button_margin);

                if (x >= btn_x && x < btn_x + button_width &&
                    y >= btn_y && y < btn_y + button_height)
                {
                    over_button = true;
                    new_hovered_index = i;
                    break;
                }
            }
        }

        if (new_hovered_index != screen->panel.hovered_button_index)
        {
            screen->panel.hovered_button_index = new_hovered_index;

            if (screen->video && screen->window_shown) {
                sc_screen_render(screen, false);
            }
        }

        if (over_button && !screen->cursor_is_hand)
        {
            SDL_SetCursor(screen->hand_cursor);
            screen->cursor_is_hand = true;
        }
        else if (!over_button && screen->cursor_is_hand)
        {
            SDL_SetCursor(screen->arrow_cursor);
            screen->cursor_is_hand = false;
        }
    }

    // LinkAndroid: Handle panel button clicks
    if ((event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) &&
        screen->panel.visible &&
        screen->panel.button_count > 0)
    {
        // SDL3 event coords are logical points
        int32_t x = event->button.x;
        int32_t y = event->button.y;

        int panel_x = screen->rect.x + screen->rect.w; // logical points
        int panel_w = PANEL_WIDTH;                      // logical points

        bool in_panel = x >= panel_x && x < panel_x + panel_w;

        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        {
            if (in_panel)
            {
                int button_margin = PANEL_BUTTON_MARGIN;
                int button_height = PANEL_BUTTON_HEIGHT;
                int button_width = panel_w - 2 * button_margin;
                int start_y = PANEL_START_Y;

                for (int i = 0; i < screen->panel.button_count; i++)
                {
                    int btn_x = panel_x + button_margin;
                    int btn_y = screen->rect.y + start_y + i * (button_height + button_margin);

                    if (x >= btn_x && x < btn_x + button_width &&
                        y >= btn_y && y < btn_y + button_height)
                    {
                        sc_screen_send_panel_click(screen, screen->panel.buttons[i].id);

                        if (screen->video && screen->window_shown) {
                            sc_screen_render(screen, false);
                        }

                        return;
                    }
                }
                return;
            }
            else
            {
                screen->mouse_button_pressed_outside_panel = true;
            }
        }
        else // SDL_EVENT_MOUSE_BUTTON_UP
        {
            if (in_panel)
            {
                sc_input_manager_handle_event(&screen->im, event);
                screen->mouse_button_pressed_outside_panel = false;
                return;
            }
            else
            {
                screen->mouse_button_pressed_outside_panel = false;
            }
        }
    }

    // LinkAndroid: Filter mouse motion events in panel area
    if (event->type == SDL_EVENT_MOUSE_MOTION &&
        screen->panel.visible)
    {
        // SDL3 event coords are logical points
        int32_t x = event->motion.x;
        int32_t y = event->motion.y;

        int panel_x = screen->rect.x + screen->rect.w; // logical points
        int panel_w = PANEL_WIDTH;                      // logical points

        if (x >= panel_x && x < panel_x + panel_w)
        {
            if (screen->mouse_button_pressed_outside_panel)
            {
                SDL_Event release_event = *event;
                release_event.type = SDL_EVENT_MOUSE_BUTTON_UP;
                release_event.button.button = SDL_BUTTON_LEFT;
                release_event.button.clicks = 1;

                sc_input_manager_handle_event(&screen->im, &release_event);

                screen->mouse_button_pressed_outside_panel = false;

                LOGD("Sent synthetic mouse release event when entering panel area");
            }

            return;
        }
    }

    sc_input_manager_handle_event(&screen->im, event);
}

void
sc_screen_handle_disconnection(struct sc_screen *screen) {
    if (!screen->window_shown) {
        return;
    }

    if (!screen->disconnect_started) {
        return;
    }

    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_EVENT_WINDOW_EXPOSED:
                sc_screen_render(screen, true);
                break;
            case SC_EVENT_DISCONNECTED_ICON_LOADED: {
                SDL_Surface *icon_disconnected = event.user.data1;
                assert(icon_disconnected);

                bool ok = sc_texture_set_from_surface(&screen->tex,
                                                      icon_disconnected);
                if (ok) {
                    screen->content_size.width = icon_disconnected->w;
                    screen->content_size.height = icon_disconnected->h;
                    sc_screen_render(screen, true);
                } else {
                    LOGE("Could not set disconnected icon");
                }

                sc_icon_destroy(icon_disconnected);
                break;
            }
            case SC_EVENT_DISCONNECTED_TIMEOUT:
                LOGD("Closing after device disconnection");
                return;
            case SDL_EVENT_QUIT:
                LOGD("User requested to quit");
                sc_screen_interrupt_disconnect(screen);
                return;
            default:
                sc_input_manager_handle_event(&screen->im, &event);
        }
    }
}

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y) {
    assert(screen->video);

    // SDL3 event coordinates are in logical points (same as screen->rect).
    // No HiDPI conversion needed — upstream scrcpy removed hidpi_scale_coords
    // for this reason (commit aecd902e).

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    assert(screen->rect.w && screen->rect.h);

    x = (int64_t)(x - screen->rect.x) * w / (int32_t)screen->rect.w;
    y = (int64_t)(y - screen->rect.y) * h / (int32_t)screen->rect.h;

    struct sc_point result;
    switch (orientation)
    {
    case SC_ORIENTATION_0:
        result.x = x;
        result.y = y;
        break;
    case SC_ORIENTATION_90:
        result.x = y;
        result.y = w - x;
        break;
    case SC_ORIENTATION_180:
        result.x = w - x;
        result.y = h - y;
        break;
    case SC_ORIENTATION_270:
        result.x = h - y;
        result.y = x;
        break;
    case SC_ORIENTATION_FLIP_0:
        result.x = w - x;
        result.y = y;
        break;
    case SC_ORIENTATION_FLIP_90:
        result.x = h - y;
        result.y = w - x;
        break;
    case SC_ORIENTATION_FLIP_180:
        result.x = x;
        result.y = h - y;
        break;
    default:
        assert(orientation == SC_ORIENTATION_FLIP_270);
        result.x = y;
        result.y = x;
        break;
    }

    return result;
}

static struct sc_point
sc_screen_convert_drawable_to_frame_coords(struct sc_screen *screen,
                                           int32_t x, int32_t y)
{
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    assert(screen->rect.w && screen->rect.h);

    x = (int64_t)(x - screen->rect.x) * w / (int32_t)screen->rect.w;
    y = (int64_t)(y - screen->rect.y) * h / (int32_t)screen->rect.h;

    struct sc_point result;
    switch (orientation)
    {
    case SC_ORIENTATION_0:
        result.x = x;
        result.y = y;
        break;
    case SC_ORIENTATION_90:
        result.x = y;
        result.y = w - x;
        break;
    case SC_ORIENTATION_180:
        result.x = w - x;
        result.y = h - y;
        break;
    case SC_ORIENTATION_270:
        result.x = h - y;
        result.y = x;
        break;
    case SC_ORIENTATION_FLIP_0:
        result.x = w - x;
        result.y = y;
        break;
    case SC_ORIENTATION_FLIP_90:
        result.x = h - y;
        result.y = w - x;
        break;
    case SC_ORIENTATION_FLIP_180:
        result.x = x;
        result.y = h - y;
        break;
    default:
        assert(orientation == SC_ORIENTATION_FLIP_270);
        result.x = y;
        result.y = x;
        break;
    }

    return result;
}

void sc_screen_update_panel(struct sc_screen *screen, const char *json)
{
    if (!json)
    {
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root)
    {
        LOGE("Failed to parse panel JSON");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!cJSON_IsString(type_item) || !cJSON_IsObject(data_item))
    {
        LOGE("Invalid panel JSON format");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type_item->valuestring, "panel") != 0)
    {
        cJSON_Delete(root);
        return;
    }

    if (!screen->panel_enabled)
    {
        LOGD("Panel data received but panel is disabled (use --linkandroid-panel-show to enable)");
        cJSON_Delete(root);
        return;
    }

    cJSON *buttons_array = cJSON_GetObjectItemCaseSensitive(data_item, "buttons");
    if (!cJSON_IsArray(buttons_array))
    {
        LOGE("Panel data missing buttons array");
        cJSON_Delete(root);
        return;
    }

    for (int i = 0; i < screen->panel.button_count; i++)
    {
        if (screen->panel.buttons[i].icon_texture)
        {
            SDL_DestroyTexture(screen->panel.buttons[i].icon_texture);
            screen->panel.buttons[i].icon_texture = NULL;
        }
    }
    screen->panel.button_count = 0;
    screen->panel.visible = true;
    screen->panel.hovered_button_index = -1;

    int array_size = cJSON_GetArraySize(buttons_array);
    int count = 0;

    for (int i = 0; i < array_size && count < SC_MAX_PANEL_BUTTONS; i++)
    {
        cJSON *button_item = cJSON_GetArrayItem(buttons_array, i);
        if (!cJSON_IsObject(button_item))
        {
            continue;
        }

        cJSON *id_item = cJSON_GetObjectItemCaseSensitive(button_item, "id");
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(button_item, "text");
        cJSON *icon_item = cJSON_GetObjectItemCaseSensitive(button_item, "icon");

        if (!text_item)
        {
            cJSON *child = button_item->child;
            while (child)
            {
                if (cJSON_IsString(child) && strcmp(child->string, "id") != 0 && strcmp(child->string, "icon") != 0)
                {
                    text_item = child;
                    break;
                }
                child = child->next;
            }
        }

        if (cJSON_IsString(id_item))
        {
            strncpy(screen->panel.buttons[count].id, id_item->valuestring,
                    sizeof(screen->panel.buttons[count].id) - 1);
            screen->panel.buttons[count].id[sizeof(screen->panel.buttons[count].id) - 1] = '\0';

            screen->panel.buttons[count].text[0] = '\0';
            screen->panel.buttons[count].icon[0] = '\0';
            screen->panel.buttons[count].icon_texture = NULL;

            if (cJSON_IsString(text_item))
            {
                strncpy(screen->panel.buttons[count].text, text_item->valuestring,
                        sizeof(screen->panel.buttons[count].text) - 1);
                screen->panel.buttons[count].text[sizeof(screen->panel.buttons[count].text) - 1] = '\0';
            }

            if (cJSON_IsString(icon_item))
            {
                strncpy(screen->panel.buttons[count].icon, icon_item->valuestring,
                        sizeof(screen->panel.buttons[count].icon) - 1);
                screen->panel.buttons[count].icon[sizeof(screen->panel.buttons[count].icon) - 1] = '\0';
            }

            count++;
        }
    }

    screen->panel.button_count = count;
    if (count == 0) {
        screen->panel.visible = false;
    }
    screen->panel_layout_dirty = true;
    LOGI("Panel updated with %d buttons", count);

    // If the panel became (or was already) visible, dispatch a task to the
    // main thread to enlarge the window and update the aspect ratio.
    // SDL window operations must be on the main thread (macOS AppKit enforces
    // this, and it crashes otherwise).
    if (screen->window_shown && count > 0) {
        sc_run_on_main_thread(task_enlarge_window_for_panel, screen, false);
    }

    cJSON_Delete(root);
}

void sc_screen_send_panel_click(struct sc_screen *screen, const char *button_id)
{
    (void)screen;

    if (!button_id || !g_websocket_client)
    {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root)
    {
        return;
    }

    cJSON *type_value = cJSON_CreateString("panel_button_click");
    if (type_value)
    {
        cJSON_AddItemToObject(root, "type", type_value);
    }

    cJSON *data = cJSON_CreateObject();
    if (data)
    {
        cJSON *id_value = cJSON_CreateString(button_id);
        if (id_value)
        {
            cJSON_AddItemToObject(data, "id", id_value);
        }
        cJSON_AddItemToObject(root, "data", data);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str)
    {
        LOGI("Sending panel click: %s", button_id);
        la_websocket_client_send(g_websocket_client, json_str);
        cJSON_free(json_str);
    }

    cJSON_Delete(root);
}
