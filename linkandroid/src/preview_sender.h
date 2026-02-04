#ifndef LA_PREVIEW_SENDER_H
#define LA_PREVIEW_SENDER_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

struct la_websocket_client;
struct sc_screen;

struct la_preview_sender
{
    struct la_websocket_client *ws_client;
    struct sc_screen *screen;
    uint32_t interval_ms; // Preview interval in milliseconds
    uint8_t ratio;        // Preview resolution ratio (1-100, 100 = original)
    bool running;
    pthread_t thread;
    bool thread_started;
};

/**
 * Initialize preview sender
 *
 * @param sender Preview sender instance
 * @param ws_client WebSocket client for sending previews
 * @param screen Screen object to capture from
 * @param interval_ms Preview interval in milliseconds
 * @param ratio Preview resolution ratio (1-100, 100 = original)
 * @return true on success, false on failure
 */
bool la_preview_sender_init(struct la_preview_sender *sender,
                            struct la_websocket_client *ws_client,
                            struct sc_screen *screen,
                            uint32_t interval_ms,
                            uint8_t ratio);

/**
 * Start preview sender thread
 *
 * @param sender Preview sender instance
 * @return true on success, false on failure
 */
bool la_preview_sender_start(struct la_preview_sender *sender);

/**
 * Stop preview sender thread
 *
 * @param sender Preview sender instance
 */
void la_preview_sender_stop(struct la_preview_sender *sender);

/**
 * Destroy preview sender and free resources
 *
 * @param sender Preview sender instance
 */
void la_preview_sender_destroy(struct la_preview_sender *sender);

#endif
