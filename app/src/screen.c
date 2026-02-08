#include "screen.h"

#include <assert.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef HAVE_SDL2_TTF
#include <SDL2/SDL_ttf.h>
#endif

#include "events.h"
#include "icon.h"
#include "options.h"
#include "util/log.h"

// LinkAndroid: WebSocket event forwarding
#include "input_manager.h"
#include "../../linkandroid/src/json/cJSON.h"
#include "../../linkandroid/src/websocket_client.h"

// External declaration for WebSocket client
extern struct la_websocket_client *g_websocket_client;

#define DISPLAY_MARGINS 96
#define PANEL_WIDTH 50 // Fixed width for right panel in pixels
#define PANEL_BUTTON_HEIGHT 45
#define PANEL_START_Y 10
#define PANEL_BUTTON_MARGIN 10
#define PANEL_FONT_SIZE 20

#define DOWNCAST(SINK) container_of(SINK, struct sc_screen, frame_sink)

// Forward declarations
static void sc_screen_render_panel(struct sc_screen *screen);

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

// get the window size in a struct sc_size
static struct sc_size
get_window_size(const struct sc_screen *screen)
{
    int width;
    int height;
    SDL_GetWindowSize(screen->window, &width, &height);

    struct sc_size size;
    size.width = width;
    size.height = height;
    return size;
}

static struct sc_point
get_window_position(const struct sc_screen *screen)
{
    int x;
    int y;
    SDL_GetWindowPosition(screen->window, &x, &y);

    struct sc_point point;
    point.x = x;
    point.y = y;
    return point;
}

// set the window size to be applied when fullscreen is disabled
static void
set_window_size(struct sc_screen *screen, struct sc_size new_size)
{
    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    SDL_SetWindowSize(screen->window, new_size.width, new_size.height);
}

// get the preferred display bounds (i.e. the screen bounds with some margins)
static bool
get_preferred_display_bounds(struct sc_size *bounds)
{
    SDL_Rect rect;
    if (SDL_GetDisplayUsableBounds(0, &rect))
    {
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
    return current_size.height == current_size.width * content_size.height / content_size.width || current_size.width == current_size.height * content_size.width / content_size.height;
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

    bool keep_width = content_size.width * window_size.height > content_size.height * window_size.width;
    if (keep_width)
    {
        // remove black borders on top and bottom
        window_size.height = content_size.height * window_size.width / content_size.width;
    }
    else
    {
        // remove black borders on left and right (or none at all if it already
        // fits)
        window_size.width = content_size.width * window_size.height / content_size.height;
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

static inline bool
sc_screen_is_relative_mode(struct sc_screen *screen)
{
    // screen->im.mp may be NULL if --no-control
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
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);
    float hidpi_scale = (float)dw / ww;
    return (int)(PANEL_WIDTH * hidpi_scale);
}

static void
sc_screen_update_content_rect(struct sc_screen *screen)
{
    assert(screen->video);

    int dw;
    int dh;
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    struct sc_size content_size = screen->content_size;
    // The drawable size is the window size * the HiDPI scale
    struct sc_size drawable_size = {dw, dh};

    SDL_Rect *rect = &screen->rect;

    // Get HiDPI-scaled panel width
    int panel_width = sc_screen_get_panel_width_scaled(screen);

    // Calculate the effective drawable size (excluding panel)
    int effective_width = drawable_size.width - panel_width;

    // Create a temporary size for layout calculation
    struct sc_size layout_size = {effective_width, drawable_size.height};

    if (is_optimal_size(layout_size, content_size))
    {
        // Video fits perfectly in the available space
        rect->w = effective_width;
        rect->h = drawable_size.height;
        // Center the combined (video + panel) area
        rect->x = (drawable_size.width - effective_width - panel_width) / 2;
        rect->y = 0;
        return;
    }

    bool keep_width = content_size.width * layout_size.height > content_size.height * layout_size.width;
    if (keep_width)
    {
        // Video width fills the effective width, scale height proportionally
        rect->w = effective_width;
        rect->h = effective_width * content_size.height / content_size.width;

        // Center the combined area horizontally and video vertically
        rect->x = (drawable_size.width - effective_width - panel_width) / 2;
        rect->y = (drawable_size.height - rect->h) / 2;
    }
    else
    {
        // Video height fills the drawable height, scale width proportionally
        rect->h = drawable_size.height;
        rect->w = drawable_size.height * content_size.width / content_size.height;

        // Center the combined (video + panel) area
        int total_width = rect->w + panel_width;
        rect->x = (drawable_size.width - total_width) / 2;
        rect->y = 0;
    }
}

// render the texture to the renderer
//
// Set the update_content_rect flag if the window or content size may have
// changed, so that the content rectangle is recomputed
static void
sc_screen_render(struct sc_screen *screen, bool update_content_rect)
{
    assert(screen->video);

    if (update_content_rect)
    {
        sc_screen_update_content_rect(screen);
    }

    enum sc_display_result res =
        sc_display_render(&screen->display, &screen->rect, screen->orientation);
    (void)res; // any error already logged

    // Render panel if visible
    if (screen->panel.visible && screen->panel.button_count > 0)
    {
        sc_screen_render_panel(screen);
    }

    // Present the final rendered frame (after all overlays)
    SDL_RenderPresent(screen->display.renderer);

    // LinkAndroid: Send ready event after the first frame is completely rendered and presented
    if (!screen->ready_event_sent && screen->has_frame && g_websocket_client)
    {
        const char *ready_event = "{\"type\":\"ready\"}";
        la_websocket_client_send(g_websocket_client, ready_event);
        LOGI("LinkAndroid: Sent ready event to WebSocket server");
        screen->ready_event_sent = true;
    }
}

// Render the right-side panel with buttons
static void
sc_screen_render_panel(struct sc_screen *screen)
{
    SDL_Renderer *renderer = screen->display.renderer;
    if (!renderer)
    {
        LOGW("Panel render: No renderer available");
        return;
    }

    // LOGI("Rendering panel: buttons=%d, visible=%d, font=%p",
    //      screen->panel.button_count, screen->panel.visible, screen->panel_font);

    int ww, wh, dw, dh;
    SDL_GetWindowSize(screen->window, &ww, &wh);
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);
    
    // Calculate HiDPI scale factor
    float hidpi_scale = (float)dw / ww;

    // Get HiDPI-scaled panel dimensions
    int panel_w = sc_screen_get_panel_width_scaled(screen);
    int button_margin = PANEL_BUTTON_MARGIN;
    int button_height = PANEL_BUTTON_HEIGHT;
    int start_y = PANEL_START_Y;

    // Calculate panel position (right side of video rect)
    int panel_x = screen->rect.x + screen->rect.w;
    int panel_y = 0;
    int panel_h = dh;

    // Draw panel background
    SDL_SetRenderDrawColor(renderer, 45, 45, 45, 255); // Dark gray #2D2D2D
    SDL_Rect panel_rect = {panel_x, panel_y, panel_w, panel_h};
    SDL_RenderFillRect(renderer, &panel_rect);

    // Button layout
    int button_width = panel_w - 2 * button_margin;

    for (int i = 0; i < screen->panel.button_count; i++)
    {
        int btn_x = panel_x + button_margin;
        int btn_y = screen->rect.y + start_y + i * (button_height + button_margin);

        // Draw button background (rounded rectangle)
        // For simplicity, draw a regular rectangle first
        // TODO: Add rounded corners using OpenGL shader
        SDL_SetRenderDrawColor(renderer, 60, 60, 60, 255); // Button bg #3C3C3C
        SDL_Rect button_rect = {btn_x, btn_y, button_width, button_height};
        SDL_RenderFillRect(renderer, &button_rect);

        // Draw button border
        SDL_SetRenderDrawColor(renderer, 80, 80, 80, 255); // Border #505050
        SDL_RenderDrawRect(renderer, &button_rect);

#ifdef HAVE_SDL2_TTF
        // Render button text
        if (screen->panel_font && screen->panel.buttons[i].text[0] != '\0')
        {
            SDL_Color text_color = {255, 255, 255, 255}; // White text
            SDL_Surface *text_surface = TTF_RenderUTF8_Blended(
                screen->panel_font,
                screen->panel.buttons[i].text,
                text_color);

            if (text_surface)
            {
                SDL_Texture *text_texture = SDL_CreateTextureFromSurface(renderer, text_surface);
                if (text_texture)
                {
                    // Center text in button
                    int text_w = text_surface->w;
                    int text_h = text_surface->h;
                    int text_x = btn_x + (button_width - text_w) / 2;
                    int text_y = btn_y + (button_height - text_h) / 2;

                    SDL_Rect text_rect = {text_x, text_y, text_w, text_h};
                    SDL_RenderCopy(renderer, text_texture, NULL, &text_rect);
                    SDL_DestroyTexture(text_texture);
                }
                SDL_FreeSurface(text_surface);
            }
        }
#else
        // Without SDL2_ttf, just show button ID as fallback
        // This is a simple placeholder; in production you might want to
        // implement a basic bitmap font or use another text rendering method
        (void)screen->panel.buttons[i].text; // Suppress unused warning
#endif
    }

    // NOTE: Do NOT call SDL_RenderPresent here!
    // The screen render function (sc_screen_render) will handle presenting.
    // This allows us to render overlays (like the panel) before presenting.
}

static void
sc_screen_render_novideo(struct sc_screen *screen)
{
    enum sc_display_result res =
        sc_display_render(&screen->display, NULL, SC_ORIENTATION_0);
    (void)res; // any error already logged

    // Present the frame
    SDL_RenderPresent(screen->display.renderer);
}

#if defined(__APPLE__) || defined(_WIN32)
#define CONTINUOUS_RESIZING_WORKAROUND
#endif

#ifdef CONTINUOUS_RESIZING_WORKAROUND
// On Windows and MacOS, resizing blocks the event loop, so resizing events are
// not triggered. As a workaround, handle them in an event handler.
//
// <https://bugzilla.libsdl.org/show_bug.cgi?id=2077>
// <https://stackoverflow.com/a/40693139/1987178>
static int
event_watcher(void *data, SDL_Event *event)
{
    struct sc_screen *screen = data;
    assert(screen->video);

    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_RESIZED)
    {
        // In practice, it seems to always be called from the same thread in
        // that specific case. Anyway, it's just a workaround.
        sc_screen_render(screen, true);
    }
    return 0;
}
#endif

static bool
sc_screen_frame_sink_open(struct sc_frame_sink *sink,
                          const AVCodecContext *ctx)
{
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);
    (void)ctx;

    struct sc_screen *screen = DOWNCAST(sink);

    if (ctx->width <= 0 || ctx->width > 0xFFFF || ctx->height <= 0 || ctx->height > 0xFFFF)
    {
        LOGE("Invalid video size: %dx%d", ctx->width, ctx->height);
        return false;
    }

    assert(ctx->width > 0 && ctx->width <= 0xFFFF);
    assert(ctx->height > 0 && ctx->height <= 0xFFFF);
    // screen->frame_size is never used before the event is pushed, and the
    // event acts as a memory barrier so it is safe without mutex
    screen->frame_size.width = ctx->width;
    screen->frame_size.height = ctx->height;

    // LinkAndroid: Set device size for WebSocket event forwarding
    sc_input_manager_set_device_size(ctx->width, ctx->height);

    // Post the event on the UI thread (the texture must be created from there)
    bool ok = sc_push_event(SC_EVENT_SCREEN_INIT_SIZE);
    if (!ok)
    {
        return false;
    }

#ifndef NDEBUG
    screen->open = true;
#endif

    // nothing to do, the screen is already open on the main thread
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

    // nothing to do, the screen lifecycle is not managed by the frame producer
}

static bool
sc_screen_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame)
{
    struct sc_screen *screen = DOWNCAST(sink);
    assert(screen->video);

    bool previous_skipped;
    bool ok = sc_frame_buffer_push(&screen->fb, frame, &previous_skipped);
    if (!ok)
    {
        return false;
    }

    if (previous_skipped)
    {
        sc_fps_counter_add_skipped_frame(&screen->fps_counter);
        // The SC_EVENT_NEW_FRAME triggered for the previous frame will consume
        // this new frame instead
    }
    else
    {
        // Post the event on the UI thread
        bool ok = sc_push_event(SC_EVENT_NEW_FRAME);
        if (!ok)
        {
            return false;
        }
    }

    return true;
}

bool sc_screen_init(struct sc_screen *screen,
                    const struct sc_screen_params *params)
{
    screen->resize_pending = false;
    screen->has_frame = false;
    screen->fullscreen = false;
    screen->maximized = false;
    screen->minimized = false;
    screen->paused = false;
    screen->resume_frame = NULL;
    screen->orientation = SC_ORIENTATION_0;
    screen->ready_event_sent = false; // LinkAndroid: Initialize ready event flag

    screen->video = params->video;

    // Initialize panel configuration
    screen->panel.button_count = 0;
    // Set panel visibility based on startup parameter
    screen->panel.visible = params->panel_show;
    memset(screen->panel.buttons, 0, sizeof(screen->panel.buttons));
    screen->panel_font = NULL;

#ifdef HAVE_SDL2_TTF
    // Initialize SDL_ttf for text rendering
    if (TTF_Init() == -1)
    {
        LOGW("Could not initialize SDL_ttf: %s", TTF_GetError());
    }
    else
    {
        // First try environment variable
        char *env_font_path = getenv("SCRCPY_FONT_PATH");
        if (env_font_path)
        {
            screen->panel_font = TTF_OpenFont(env_font_path, PANEL_FONT_SIZE);
            if (screen->panel_font)
            {
                LOGI("Loaded custom font from SCRCPY_FONT_PATH: %s", env_font_path);
            }
        }
        
        if (!screen->panel_font)
        {
            // Try to load the custom font from multiple possible locations
            const char *font_search_paths[] = {
                NULL, // Will be set to base_path/data/font.ttf
                "data/font.ttf", // Relative to current directory
                "../share/scrcpy/font.ttf", // Relative to bin directory (Linux/macOS install)
                NULL
            };
        
        char *base_path = SDL_GetBasePath();
        char *base_font_path = NULL;
        
        if (base_path)
        {
            // Construct path to base_path/data/font.ttf
            size_t path_len = strlen(base_path) + strlen("data/font.ttf") + 1;
            base_font_path = malloc(path_len);
            if (base_font_path)
            {
                snprintf(base_font_path, path_len, "%sdata/font.ttf", base_path);
                font_search_paths[0] = base_font_path;
            }
        }
        
        // Try each path
        for (int i = 0; font_search_paths[i] != NULL; i++)
        {
            screen->panel_font = TTF_OpenFont(font_search_paths[i], PANEL_FONT_SIZE);
            if (screen->panel_font)
            {
                LOGI("Loaded custom font: %s", font_search_paths[i]);
                break;
            }
        }
        
            // Cleanup
            if (base_font_path)
            {
                free(base_font_path);
            }
            if (base_path)
            {
                SDL_free(base_path);
            }
        }

        // If custom font failed, try system fonts as fallback
        if (!screen->panel_font)
        {
            // Try to load a system font that supports Unicode/Emoji
            // Note: Use regular text fonts for now. Apple Color Emoji.ttc doesn't work
            // well with TTF_RenderUTF8_Blended (it's a colored font, needs special handling)
            const char *font_paths[] = {
                // macOS fonts - use text fonts that have emoji fallback
                "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
                "/System/Library/Fonts/Helvetica.ttc",
                "/System/Library/Fonts/PingFang.ttc",
                // Alternative: use SF Pro which has better emoji support
                "/System/Library/Fonts/SFNS.ttf",
                // Linux fonts
                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
                "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                // Windows fonts
                "C:\\Windows\\Fonts\\seguisym.ttf", // Segoe UI Symbol
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\msyh.ttc",
                NULL};

            for (int i = 0; font_paths[i] != NULL; i++)
            {
                screen->panel_font = TTF_OpenFont(font_paths[i], PANEL_FONT_SIZE);
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
#else
    LOGW("SDL2_ttf not available, panel button text rendering disabled");
#endif

    // Initialize cursors for panel button hover
    screen->hand_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
    screen->arrow_cursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
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
    screen->req.hide_window = params->hide_window; // LinkAndroid

    bool ok = sc_frame_buffer_init(&screen->fb);
    if (!ok)
    {
        return false;
    }

    if (!sc_fps_counter_init(&screen->fps_counter))
    {
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

    uint32_t window_flags = SDL_WINDOW_ALLOW_HIGHDPI;
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
        // The window will be shown on first frame
        window_flags |= SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
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

    // The window will be positioned and sized on first video frame
    screen->window = SDL_CreateWindow(title, x, y, width, height, window_flags);
    if (!screen->window)
    {
        LOGE("Could not create window: %s", SDL_GetError());
        goto error_destroy_fps_counter;
    }

    SDL_Surface *icon = scrcpy_icon_load();
    if (icon)
    {
        SDL_SetWindowIcon(screen->window, icon);
    }
    else if (params->video)
    {
        // just a warning
        LOGW("Could not load icon");
    }
    else
    {
        // without video, the icon is used as window content, it must be present
        LOGE("Could not load icon");
        goto error_destroy_window;
    }

    SDL_Surface *icon_novideo = params->video ? NULL : icon;
    bool mipmaps = params->video && params->mipmaps;
    ok = sc_display_init(&screen->display, screen->window, icon_novideo,
                         mipmaps);
    if (icon)
    {
        scrcpy_icon_destroy(icon);
    }
    if (!ok)
    {
        goto error_destroy_window;
    }

    screen->frame = av_frame_alloc();
    if (!screen->frame)
    {
        LOG_OOM();
        goto error_destroy_display;
    }

    struct sc_input_manager_params im_params = {
        .controller = params->controller,
        .fp = params->fp,
        .screen = screen,
        .kp = params->kp,
        .mp = params->mp,
        .gp = params->gp,
        .mouse_bindings = params->mouse_bindings,
        .legacy_paste = params->legacy_paste,
        .clipboard_autosync = params->clipboard_autosync,
        .shortcut_mods = params->shortcut_mods,
    };

    sc_input_manager_init(&screen->im, &im_params);

    // Initialize even if not used for simplicity
    sc_mouse_capture_init(&screen->mc, screen->window, params->shortcut_mods);

#ifdef CONTINUOUS_RESIZING_WORKAROUND
    if (screen->video)
    {
        SDL_AddEventWatch(event_watcher, screen);
    }
#endif

    static const struct sc_frame_sink_ops ops = {
        .open = sc_screen_frame_sink_open,
        .close = sc_screen_frame_sink_close,
        .push = sc_screen_frame_sink_push,
    };

    screen->frame_sink.ops = &ops;

#ifndef NDEBUG
    screen->open = false;
#endif

    if (!screen->video && sc_screen_is_relative_mode(screen))
    {
        // Capture mouse immediately if video mirroring is disabled
        sc_mouse_capture_set_active(&screen->mc, true);
    }

    return true;

error_destroy_display:
    sc_display_destroy(&screen->display);
error_destroy_window:
    SDL_DestroyWindow(screen->window);
error_destroy_fps_counter:
    sc_fps_counter_destroy(&screen->fps_counter);
error_destroy_frame_buffer:
    sc_frame_buffer_destroy(&screen->fb);

    return false;
}

static void
sc_screen_show_initial_window(struct sc_screen *screen)
{
    // LinkAndroid: Don't show window if hide_window is requested
    if (screen->req.hide_window)
    {
        // Skip showing the window, but still update content rect
        sc_screen_update_content_rect(screen);
        return;
    }

    int x = screen->req.x != SC_WINDOW_POSITION_UNDEFINED
                ? screen->req.x
                : (int)SDL_WINDOWPOS_CENTERED;
    int y = screen->req.y != SC_WINDOW_POSITION_UNDEFINED
                ? screen->req.y
                : (int)SDL_WINDOWPOS_CENTERED;

    struct sc_size window_size =
        get_initial_optimal_size(screen->content_size, screen->req.width,
                                 screen->req.height);

    // Add panel width to window size if panel is visible
    // The panel width is in logical pixels (not HiDPI scaled)
    if (screen->panel.visible)
    {
        window_size.width += PANEL_WIDTH;
    }

    set_window_size(screen, window_size);
    SDL_SetWindowPosition(screen->window, x, y);

    if (screen->req.fullscreen)
    {
        sc_screen_toggle_fullscreen(screen);
    }

    if (screen->req.start_fps_counter)
    {
        sc_fps_counter_start(&screen->fps_counter);
    }

    SDL_ShowWindow(screen->window);
    sc_screen_update_content_rect(screen);
}

void sc_screen_hide_window(struct sc_screen *screen)
{
    SDL_HideWindow(screen->window);
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
#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_SetWindowAlwaysOnTop(screen->window, enable ? SDL_TRUE : SDL_FALSE);
    LOGI("Window always-on-top: %s", enable ? "enabled" : "disabled");
#else
    (void) enable;
    LOGW("SDL_SetWindowAlwaysOnTop requires SDL >= 2.0.16");
#endif
}

void sc_screen_interrupt(struct sc_screen *screen)
{
    sc_fps_counter_interrupt(&screen->fps_counter);
}

void sc_screen_join(struct sc_screen *screen)
{
    sc_fps_counter_join(&screen->fps_counter);
}

void sc_screen_destroy(struct sc_screen *screen)
{
#ifndef NDEBUG
    assert(!screen->open);
#endif

#ifdef HAVE_SDL2_TTF
    // Cleanup panel font
    if (screen->panel_font)
    {
        TTF_CloseFont(screen->panel_font);
        screen->panel_font = NULL;
    }
    TTF_Quit();
#endif

    // Cleanup cursors
    if (screen->hand_cursor)
    {
        SDL_FreeCursor(screen->hand_cursor);
    }
    if (screen->arrow_cursor)
    {
        SDL_FreeCursor(screen->arrow_cursor);
    }

    sc_display_destroy(&screen->display);
    av_frame_free(&screen->frame);
    SDL_DestroyWindow(screen->window);
    sc_fps_counter_destroy(&screen->fps_counter);
    sc_frame_buffer_destroy(&screen->fb);
}

static void
resize_for_content(struct sc_screen *screen, struct sc_size old_content_size,
                   struct sc_size new_content_size)
{
    assert(screen->video);

    struct sc_size window_size = get_window_size(screen);
    struct sc_size target_size = {
        .width = (uint32_t)window_size.width * new_content_size.width / old_content_size.width,
        .height = (uint32_t)window_size.height * new_content_size.height / old_content_size.height,
    };
    target_size = get_optimal_size(target_size, new_content_size, true);
    set_window_size(screen, target_size);
}

static void
set_content_size(struct sc_screen *screen, struct sc_size new_content_size)
{
    assert(screen->video);

    if (!screen->fullscreen && !screen->maximized && !screen->minimized)
    {
        resize_for_content(screen, screen->content_size, new_content_size);
    }
    else if (!screen->resize_pending)
    {
        // Store the windowed size to be able to compute the optimal size once
        // fullscreen/maximized/minimized are disabled
        screen->windowed_content_size = screen->content_size;
        screen->resize_pending = true;
    }

    screen->content_size = new_content_size;
}

static void
apply_pending_resize(struct sc_screen *screen)
{
    assert(screen->video);

    assert(!screen->fullscreen);
    assert(!screen->maximized);
    assert(!screen->minimized);
    if (screen->resize_pending)
    {
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

    set_content_size(screen, new_content_size);

    screen->orientation = orientation;
    LOGI("Display orientation set to %s", sc_orientation_get_name(orientation));

    sc_screen_render(screen, true);
}

static bool
sc_screen_init_size(struct sc_screen *screen)
{
    // Before first frame
    assert(!screen->has_frame);

    // The requested size is passed via screen->frame_size

    struct sc_size content_size =
        get_oriented_size(screen->frame_size, screen->orientation);
    screen->content_size = content_size;

    enum sc_display_result res =
        sc_display_set_texture_size(&screen->display, screen->frame_size);
    return res != SC_DISPLAY_RESULT_ERROR;
}

// recreate the texture and resize the window if the frame size has changed
static enum sc_display_result
prepare_for_frame(struct sc_screen *screen, struct sc_size new_frame_size)
{
    assert(screen->video);

    if (screen->frame_size.width == new_frame_size.width && screen->frame_size.height == new_frame_size.height)
    {
        return SC_DISPLAY_RESULT_OK;
    }

    // frame dimension changed
    screen->frame_size = new_frame_size;

    struct sc_size new_content_size =
        get_oriented_size(new_frame_size, screen->orientation);
    set_content_size(screen, new_content_size);

    sc_screen_update_content_rect(screen);

    return sc_display_set_texture_size(&screen->display, screen->frame_size);
}

static bool
sc_screen_apply_frame(struct sc_screen *screen)
{
    assert(screen->video);

    sc_fps_counter_add_rendered_frame(&screen->fps_counter);

    AVFrame *frame = screen->frame;
    struct sc_size new_frame_size = {frame->width, frame->height};
    enum sc_display_result res = prepare_for_frame(screen, new_frame_size);
    if (res == SC_DISPLAY_RESULT_ERROR)
    {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING)
    {
        // Not an error, but do not continue
        return true;
    }

    res = sc_display_update_texture(&screen->display, frame);
    if (res == SC_DISPLAY_RESULT_ERROR)
    {
        return false;
    }
    if (res == SC_DISPLAY_RESULT_PENDING)
    {
        // Not an error, but do not continue
        return true;
    }

    if (!screen->has_frame)
    {
        screen->has_frame = true;
        // this is the very first frame, show the window
        sc_screen_show_initial_window(screen);

        if (sc_screen_is_relative_mode(screen))
        {
            // Capture mouse on start
            sc_mouse_capture_set_active(&screen->mc, true);
        }
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
        sc_frame_buffer_consume(&screen->fb, screen->resume_frame);
        return true;
    }

    av_frame_unref(screen->frame);
    sc_frame_buffer_consume(&screen->fb, screen->frame);
    return sc_screen_apply_frame(screen);
}

void sc_screen_set_paused(struct sc_screen *screen, bool paused)
{
    assert(screen->video);

    if (!paused && !screen->paused)
    {
        // nothing to do
        return;
    }

    if (screen->paused && screen->resume_frame)
    {
        // If display screen was paused, refresh the frame immediately, even if
        // the new state is also paused.
        av_frame_free(&screen->frame);
        screen->frame = screen->resume_frame;
        screen->resume_frame = NULL;
        sc_screen_apply_frame(screen);
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

    uint32_t new_mode = screen->fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (SDL_SetWindowFullscreen(screen->window, new_mode))
    {
        LOGW("Could not switch fullscreen mode: %s", SDL_GetError());
        return;
    }

    screen->fullscreen = !screen->fullscreen;
    if (!screen->fullscreen && !screen->maximized && !screen->minimized)
    {
        apply_pending_resize(screen);
    }

    LOGD("Switched to %s mode", screen->fullscreen ? "fullscreen" : "windowed");
    sc_screen_render(screen, true);
}

void sc_screen_resize_to_fit(struct sc_screen *screen)
{
    assert(screen->video);

    if (screen->fullscreen || screen->maximized || screen->minimized)
    {
        return;
    }

    struct sc_point point = get_window_position(screen);
    struct sc_size window_size = get_window_size(screen);

    struct sc_size optimal_size =
        get_optimal_size(window_size, screen->content_size, false);

    // Center the window related to the device screen
    assert(optimal_size.width <= window_size.width);
    assert(optimal_size.height <= window_size.height);
    uint32_t new_x = point.x + (window_size.width - optimal_size.width) / 2;
    uint32_t new_y = point.y + (window_size.height - optimal_size.height) / 2;

    SDL_SetWindowSize(screen->window, optimal_size.width, optimal_size.height);
    SDL_SetWindowPosition(screen->window, new_x, new_y);
    LOGD("Resized to optimal size: %ux%u", optimal_size.width,
         optimal_size.height);
}

void sc_screen_resize_to_pixel_perfect(struct sc_screen *screen)
{
    assert(screen->video);

    if (screen->fullscreen || screen->minimized)
    {
        return;
    }

    if (screen->maximized)
    {
        SDL_RestoreWindow(screen->window);
        screen->maximized = false;
    }

    struct sc_size content_size = screen->content_size;
    SDL_SetWindowSize(screen->window, content_size.width, content_size.height);
    LOGD("Resized to pixel-perfect: %ux%u", content_size.width,
         content_size.height);
}

bool sc_screen_handle_event(struct sc_screen *screen, const SDL_Event *event)
{
    switch (event->type)
    {
    case SC_EVENT_SCREEN_INIT_SIZE:
    {
        // The initial size is passed via screen->frame_size
        bool ok = sc_screen_init_size(screen);
        if (!ok)
        {
            LOGE("Could not initialize screen size");
            return false;
        }
        return true;
    }
    case SC_EVENT_NEW_FRAME:
    {
        bool ok = sc_screen_update_frame(screen);
        if (!ok)
        {
            LOGE("Frame update failed\n");
            return false;
        }
        return true;
    }
    case SDL_WINDOWEVENT:
        if (!screen->video && event->window.event == SDL_WINDOWEVENT_EXPOSED)
        {
            sc_screen_render_novideo(screen);
        }

        // !video implies !has_frame
        assert(screen->video || !screen->has_frame);
        if (!screen->has_frame)
        {
            // Do nothing
            return true;
        }
        switch (event->window.event)
        {
        case SDL_WINDOWEVENT_EXPOSED:
            sc_screen_render(screen, true);
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            sc_screen_render(screen, true);
            break;
        case SDL_WINDOWEVENT_MAXIMIZED:
            screen->maximized = true;
            break;
        case SDL_WINDOWEVENT_MINIMIZED:
            screen->minimized = true;
            break;
        case SDL_WINDOWEVENT_RESTORED:
            if (screen->fullscreen)
            {
                // On Windows, in maximized+fullscreen, disabling
                // fullscreen mode unexpectedly triggers the "restored"
                // then "maximized" events, leaving the window in a
                // weird state (maximized according to the events, but
                // not maximized visually).
                break;
            }
            screen->maximized = false;
            screen->minimized = false;
            apply_pending_resize(screen);
            sc_screen_render(screen, true);
            break;
        }
        return true;
    }

    if (sc_screen_is_relative_mode(screen) && sc_mouse_capture_handle_event(&screen->mc, event))
    {
        // The mouse capture handler consumed the event
        return true;
    }

    // Handle mouse motion for cursor change on panel hover
    if (event->type == SDL_MOUSEMOTION &&
        screen->panel.visible &&
        screen->panel.button_count > 0 &&
        screen->hand_cursor &&
        screen->arrow_cursor)
    {

        // Get mouse position in drawable coordinates
        int32_t x = event->motion.x;
        int32_t y = event->motion.y;
        sc_screen_hidpi_scale_coords(screen, &x, &y);

        // Get HiDPI scale factor for button dimensions
        int ww, wh, dw, dh;
        SDL_GetWindowSize(screen->window, &ww, &wh);
        SDL_GL_GetDrawableSize(screen->window, &dw, &dh);
        float hidpi_scale = (float)dw / ww;

        // Check if mouse is over panel area
        int panel_x = screen->rect.x + screen->rect.w;
        int panel_w = sc_screen_get_panel_width_scaled(screen);
        bool over_button = false;

        if (x >= panel_x && x < panel_x + panel_w)
        {
            // Mouse is in panel, check if over a button
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
                    break;
                }
            }
        }

        // Change cursor if needed
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

    // Check if the event is a panel button click or any mouse event in panel area
    if ((event->type == SDL_MOUSEBUTTONDOWN || event->type == SDL_MOUSEBUTTONUP) &&
        screen->panel.visible &&
        screen->panel.button_count > 0)
    {

        // Get mouse position in drawable coordinates
        int32_t x = event->button.x;
        int32_t y = event->button.y;
        sc_screen_hidpi_scale_coords(screen, &x, &y);

        // Get HiDPI scale factor for button dimensions
        int ww, wh, dw, dh;
        SDL_GetWindowSize(screen->window, &ww, &wh);
        SDL_GL_GetDrawableSize(screen->window, &dw, &dh);
        float hidpi_scale = (float)dw / ww;

        // Check if click is in panel area (right of video rect)
        int panel_x = screen->rect.x + screen->rect.w;
        int panel_w = sc_screen_get_panel_width_scaled(screen);

        bool in_panel = x >= panel_x && x < panel_x + panel_w;

        if (event->type == SDL_MOUSEBUTTONDOWN)
        {
            if (in_panel)
            {
                // Mouse down in panel area - check which button was clicked
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
                        // Button clicked!
                        sc_screen_send_panel_click(screen, screen->panel.buttons[i].id);
                        return true; // Consume the event
                    }
                }
                // Mouse down in panel area (not on button), consume the event
                return true;
            }
            else
            {
                // Mouse down outside panel area, track this
                screen->mouse_button_pressed_outside_panel = true;
            }
        }
        else // SDL_MOUSEBUTTONUP
        {
            if (in_panel)
            {
                // SDL_MOUSEBUTTONUP in panel area
                // We need to let the input_manager handle this event first to ensure
                // proper button state cleanup (especially if the button was pressed
                // in the video area and released in the panel area).
                // After input_manager processes it, we'll consume the event.
                sc_input_manager_handle_event(&screen->im, event);
                screen->mouse_button_pressed_outside_panel = false;
                return true; // Consume the event after processing
            }
            else
            {
                // Mouse up outside panel area
                screen->mouse_button_pressed_outside_panel = false;
            }
        }
    }

    // Also filter out mouse motion events in panel area
    if (event->type == SDL_MOUSEMOTION &&
        screen->panel.visible)
    {
        // Get mouse position in drawable coordinates
        int32_t x = event->motion.x;
        int32_t y = event->motion.y;
        sc_screen_hidpi_scale_coords(screen, &x, &y);

        // Check if motion is in panel area (right of video rect)
        int panel_x = screen->rect.x + screen->rect.w;
        int panel_w = sc_screen_get_panel_width_scaled(screen);

        if (x >= panel_x && x < panel_x + panel_w)
        {
            // Mouse entered panel area while button was pressed outside
            // Send a button release event to complete the gesture on the device
            if (screen->mouse_button_pressed_outside_panel)
            {
                // Create a synthetic MOUSEBUTTONUP event
                SDL_Event release_event = *event;
                release_event.type = SDL_MOUSEBUTTONUP;
                release_event.button.state = SDL_RELEASED;
                release_event.button.button = SDL_BUTTON_LEFT;
                release_event.button.clicks = 1;
                
                // Send the release event to input_manager
                sc_input_manager_handle_event(&screen->im, &release_event);
                
                // Clear the tracking flag
                screen->mouse_button_pressed_outside_panel = false;
                
                LOGD("Sent synthetic mouse release event when entering panel area");
            }
            
            // Mouse motion in panel area, don't send to device
            // (but don't consume - let cursor handling work)
            // Actually, we should consume it to prevent touch_move events
            return true;
        }
    }

    sc_input_manager_handle_event(&screen->im, event);
    return true;
}

struct sc_point
sc_screen_convert_drawable_to_frame_coords(struct sc_screen *screen,
                                           int32_t x, int32_t y)
{
    assert(screen->video);

    enum sc_orientation orientation = screen->orientation;

    int32_t w = screen->content_size.width;
    int32_t h = screen->content_size.height;

    // screen->rect must be initialized to avoid a division by zero
    assert(screen->rect.w && screen->rect.h);

    x = (int64_t)(x - screen->rect.x) * w / screen->rect.w;
    y = (int64_t)(y - screen->rect.y) * h / screen->rect.h;

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

struct sc_point
sc_screen_convert_window_to_frame_coords(struct sc_screen *screen,
                                         int32_t x, int32_t y)
{
    sc_screen_hidpi_scale_coords(screen, &x, &y);
    return sc_screen_convert_drawable_to_frame_coords(screen, x, y);
}

void sc_screen_hidpi_scale_coords(struct sc_screen *screen, int32_t *x, int32_t *y)
{
    // take the HiDPI scaling (dw/ww and dh/wh) into account
    int ww, wh, dw, dh;
    SDL_GetWindowSize(screen->window, &ww, &wh);
    SDL_GL_GetDrawableSize(screen->window, &dw, &dh);

    // scale for HiDPI (64 bits for intermediate multiplications)
    *x = (int64_t)*x * dw / ww;
    *y = (int64_t)*y * dh / wh;
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

    // Ignore panel data if --linkandroid-panel-show was not specified
    if (!screen->panel.visible)
    {
        LOGD("Panel data received but panel display is disabled (use --linkandroid-panel-show to enable)");
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

    // Clear existing buttons
    screen->panel.button_count = 0;
    screen->panel.visible = true;

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

        // Support old format with direct button text as well
        if (!text_item)
        {
            // Try to get the first string value as text
            cJSON *child = button_item->child;
            while (child)
            {
                if (cJSON_IsString(child) && strcmp(child->string, "id") != 0)
                {
                    text_item = child;
                    break;
                }
                child = child->next;
            }
        }

        if (cJSON_IsString(id_item) && cJSON_IsString(text_item))
        {
            strncpy(screen->panel.buttons[count].id, id_item->valuestring,
                    sizeof(screen->panel.buttons[count].id) - 1);
            screen->panel.buttons[count].id[sizeof(screen->panel.buttons[count].id) - 1] = '\0';

            strncpy(screen->panel.buttons[count].text, text_item->valuestring,
                    sizeof(screen->panel.buttons[count].text) - 1);
            screen->panel.buttons[count].text[sizeof(screen->panel.buttons[count].text) - 1] = '\0';

            count++;
        }
    }

    screen->panel.button_count = count;
    LOGI("Panel updated with %d buttons", count);

    cJSON_Delete(root);

    // Trigger a re-render with updated layout
    if (screen->video)
    {
        sc_screen_render(screen, true);
    }
}

void sc_screen_send_panel_click(struct sc_screen *screen, const char *button_id)
{
    (void)screen; // Suppress unused warning

    if (!button_id || !g_websocket_client)
    {
        return;
    }

    // Create JSON: {"type":"panel_button_click","data":{"id":"xxx"}}
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
