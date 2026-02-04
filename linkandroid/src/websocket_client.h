#ifndef LA_WEBSOCKET_CLIENT_H
#define LA_WEBSOCKET_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

struct la_websocket_client;
struct sc_control_msg;

// Callback function type for receiving JSON events from WebSocket server
typedef void (*la_websocket_on_message_cb)(const char *json, void *userdata);

/**
 * Initialize WebSocket client and connect to server
 * 
 * @param url WebSocket URL (e.g., "ws://127.0.0.1:6000/scrcpy")
 * @param on_message Callback function for received messages
 * @param userdata User data passed to callback
 * @return WebSocket client instance, or NULL on failure
 */
struct la_websocket_client *
la_websocket_client_init(const char *url, la_websocket_on_message_cb on_message, 
                         void *userdata);

/**
 * Send JSON event to WebSocket server
 * 
 * @param client WebSocket client instance
 * @param json JSON string to send
 * @return true on success, false on failure
 */
bool
la_websocket_client_send(struct la_websocket_client *client, const char *json);

/**
 * Send control message as JSON to WebSocket server (or print to stdout)
 * 
 * @param client WebSocket client instance (can be NULL)
 * @param msg Control message to serialize
 * @param device_width Device screen width
 * @param device_height Device screen height
 */
void
la_websocket_client_send_event(struct la_websocket_client *client,
                                const struct sc_control_msg *msg,
                                uint16_t device_width,
                                uint16_t device_height);

/**
 * Deserialize JSON string to control message
 *
 * @param json_str JSON string to parse
 * @param msg Pointer to control message struct to fill
 * @return true on success, false on failure
 */
bool
la_websocket_deserialize_event(const char *json_str, struct sc_control_msg *msg);

/**
 * Check if WebSocket client is connected
 * 
 * @param client WebSocket client instance
 * @return true if connected, false otherwise
 */
bool
la_websocket_client_is_connected(struct la_websocket_client *client);

/**
 * Destroy WebSocket client and close connection
 * 
 * @param client WebSocket client instance
 */
void
la_websocket_client_destroy(struct la_websocket_client *client);

#endif
