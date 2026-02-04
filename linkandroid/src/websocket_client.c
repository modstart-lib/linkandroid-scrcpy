#define _POSIX_C_SOURCE 200809L

#include "websocket_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <libwebsockets.h>
#include <SDL2/SDL_log.h>

#include "json/cJSON.h"
#include "../../app/src/control_msg.h"
#include "../../app/src/util/log.h"

#define MAX_PAYLOAD_SIZE 4096
#define RECONNECT_DELAY_MS 3000

struct message_node
{
    char *payload;
    size_t len;
    struct message_node *next;
};

struct la_websocket_client
{
    char *url;
    char *protocol;
    char *path;
    char *address;
    int port;
    bool connected;
    bool running;
    la_websocket_on_message_cb on_message;
    void *userdata;
    pthread_t thread;
    bool thread_started;
    struct lws_context *context;
    struct lws *wsi;
    pthread_mutex_t lock;
    char send_buffer[LWS_PRE + MAX_PAYLOAD_SIZE];

    int send_len;
    // Output queue
    struct message_node *queue_head;
    struct message_node *queue_tail;
};

// Forward declaration
static void *websocket_thread(void *arg);

// Parse WebSocket URL (ws://host:port/path)
static bool parse_websocket_url(const char *url, char **protocol, char **address,
                                int *port, char **path)
{
    // Default values
    *protocol = strdup("ws");
    *port = 80;
    *path = strdup("/");

    // Check protocol
    if (strncmp(url, "ws://", 5) == 0)
    {
        url += 5;
        *port = 80;
    }
    else if (strncmp(url, "wss://", 6) == 0)
    {
        url += 6;
        *port = 443;
    }
    else
    {
        LOGE("Invalid WebSocket URL protocol, must start with ws:// or wss://");
        return false;
    }

    // Find end of host (either : or / or end of string)
    const char *port_start = strchr(url, ':');
    const char *path_start = strchr(url, '/');

    // Extract address
    size_t addr_len;
    if (port_start && (!path_start || port_start < path_start))
    {
        addr_len = port_start - url;
    }
    else if (path_start)
    {
        addr_len = path_start - url;
    }
    else
    {
        addr_len = strlen(url);
    }

    *address = malloc(addr_len + 1);
    strncpy(*address, url, addr_len);
    (*address)[addr_len] = '\0';

    // Extract port if present
    if (port_start && (!path_start || port_start < path_start))
    {
        *port = atoi(port_start + 1);
    }

    // Extract path if present
    if (path_start)
    {
        free(*path);
        *path = strdup(path_start);
    }

    return true;
}

static int websocket_callback(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user, void *in, size_t len)
{
    // Handle cases where wsi is NULL (e.g. system-wide callbacks like WAIT_CANCELLED)
    if (!wsi)
    {
        return 0;
    }

    struct la_websocket_client *client =
        (struct la_websocket_client *)lws_context_user(lws_get_context(wsi));

    if (!client)
    {
        return 0;
    }

    switch (reason)
    {
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
        // Triggered by lws_cancel_service() from another thread
        // This is the thread-safe way to request a write callback
        if (client && client->wsi)
        {
            lws_callback_on_writable(client->wsi);
        }
        break;

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        LOGI("LinkAndroid WebSocket connection established");
        pthread_mutex_lock(&client->lock);
        client->connected = true;
        pthread_mutex_unlock(&client->lock);
        // Force a writable check just in case we have data queued
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        // Received message from server
        if (client->on_message)
        {
            char *msg = malloc(len + 1);
            if (msg)
            {
                memcpy(msg, in, len);
                msg[len] = '\0';
                client->on_message(msg, client->userdata);
                free(msg);
            }
        }
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        pthread_mutex_lock(&client->lock);

        // Loop to send all queued messages (or as many as possible)
        // Note: lws_write might return less than requested, or we might want to yield
        // back to the event loop. For simplicity in this client, we try to send one by one.
        // Ideally we should handle partial writes, but for small control messages
        // full writes are expected.
        if (client->queue_head)
        {
            struct message_node *node = client->queue_head;

            // Payload was allocated with LWS_PRE padding
            // node->payload points to the start of the allocation
            // The actual data starts at node->payload + LWS_PRE
            unsigned char *data_ptr = (unsigned char *)(node->payload + LWS_PRE);

            int written = lws_write(wsi, data_ptr, node->len, LWS_WRITE_TEXT);

            if (written < 0)
            {
                LOGE("WebSocket write failed");
                // On error, maybe we should clear queue or retry?
                // For now, pop it to avoid infinite loop
            }
            else if ((size_t)written < node->len)
            {
                LOGE("WebSocket partial write: %d/%zu", written, node->len);
                // In a robust implementation, we would adjust the node to send the rest later.
            }

            // Remove from queue
            client->queue_head = node->next;
            if (!client->queue_head)
            {
                client->queue_tail = NULL;
            }

            free(node->payload);
            free(node);

            // If there are more messages, schedule another write
            if (client->queue_head)
            {
                lws_callback_on_writable(wsi);
            }
        }

        pthread_mutex_unlock(&client->lock);
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        LOGE("LinkAndroid WebSocket connection error: %s",
             in ? (char *)in : "unknown");
        pthread_mutex_lock(&client->lock);
        client->connected = false;
        client->wsi = NULL;
        pthread_mutex_unlock(&client->lock);
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        LOGI("LinkAndroid WebSocket connection closed");
        pthread_mutex_lock(&client->lock);
        client->connected = false;
        client->wsi = NULL;
        pthread_mutex_unlock(&client->lock);
        break;

    default:
        break;
    }

    return 0;
}

// WebSocket protocols
static struct lws_protocols protocols[] = {
    {
        "default",
        websocket_callback,
        0,
        MAX_PAYLOAD_SIZE,
    },
    {NULL, NULL, 0, 0} // terminator
};

// WebSocket thread function
static void *websocket_thread(void *arg)
{
    struct la_websocket_client *client = (struct la_websocket_client *)arg;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    // Minimal options: only SSL init, disable all server features
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = client;

    // Disable libwebsockets logging to reduce noise
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    client->context = lws_create_context(&info);
    if (!client->context)
    {
        LOGE("Failed to create libwebsockets context");
        return NULL;
    }

    // Connect only once (no automatic reconnection)
    bool connection_attempted = false;

    while (client->running)
    {
        // Try to connect only once at startup
        if (!connection_attempted && !client->wsi)
        {
            struct lws_client_connect_info ccinfo;
            memset(&ccinfo, 0, sizeof(ccinfo));

            ccinfo.context = client->context;
            ccinfo.address = client->address;
            ccinfo.port = client->port;
            ccinfo.path = client->path;
            ccinfo.host = client->address;
            ccinfo.origin = client->address;
            ccinfo.protocol = protocols[0].name;
            ccinfo.ssl_connection = 0; // Use 0 for ws://, LCCSCF_USE_SSL for wss://

            LOGI("LinkAndroid attempting to connect to %s:%d%s",
                 client->address, client->port, client->path);

            client->wsi = lws_client_connect_via_info(&ccinfo);
            if (!client->wsi)
            {
                LOGE("Failed to initiate WebSocket connection");
                // Still mark as attempted to prevent retry loop
            }
            connection_attempted = true;
        }

        // Service the connection with shorter timeout for faster shutdown
        lws_service(client->context, 50);

        // Check if we have pending messages to send
        // This is safe because we are in the service thread
        pthread_mutex_lock(&client->lock);
        if (client->connected && client->wsi && client->queue_head)
        {
            // Request a write callback in the next service loop
            lws_callback_on_writable(client->wsi);
        }
        pthread_mutex_unlock(&client->lock);
    }

    // Cleanup: destroy context immediately without waiting
    if (client->context)
    {
        lws_context_destroy(client->context);
        client->context = NULL;
    }

    return NULL;
}

// Helper function to serialize control message to JSON
static char *
serialize_event_to_json(const struct sc_control_msg *msg,
                        uint16_t device_width,
                        uint16_t device_height)
{
    // Allocate a buffer for JSON string (max 512 bytes should be enough)
    char *json_str = malloc(512);
    if (!json_str)
    {
        LOGE("Failed to allocate JSON buffer");
        return NULL;
    }

    switch (msg->type)
    {
    case SC_CONTROL_MSG_TYPE_INJECT_KEYCODE:
    {
        const char *action = msg->inject_keycode.action == AKEY_EVENT_ACTION_DOWN
                                 ? "down"
                                 : "up";
        snprintf(json_str, 512,
                 "{\"type\":\"key\",\"data\":{\"action\":\"%s\","
                 "\"keycode\":%d,\"repeat\":%d,\"metastate\":%d,"
                 "\"width\":%u,\"height\":%u}}",
                 action, msg->inject_keycode.keycode,
                 msg->inject_keycode.repeat, msg->inject_keycode.metastate,
                 device_width, device_height);
        break;
    }
    case SC_CONTROL_MSG_TYPE_INJECT_TEXT:
    {
        // For text, we need to escape special characters
        snprintf(json_str, 512,
                 "{\"type\":\"text\",\"data\":{\"text\":\"%s\","
                 "\"width\":%u,\"height\":%u}}",
                 msg->inject_text.text, device_width, device_height);
        break;
    }
    case SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT:
    {
        const char *type_str;
        switch (msg->inject_touch_event.action)
        {
        case AMOTION_EVENT_ACTION_DOWN:
            type_str = "touch_down";
            break;
        case AMOTION_EVENT_ACTION_UP:
            type_str = "touch_up";
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            type_str = "touch_move";
            break;
        default:
            // Skip unsupported touch actions (e.g., hover events action=7)
            free(json_str);
            return NULL;
        }

        // Convert pointer_id to string to avoid JavaScript precision issues
        char pointer_id_str[32];
        snprintf(pointer_id_str, sizeof(pointer_id_str), "%llu",
                 (unsigned long long)msg->inject_touch_event.pointer_id);

        // Use absolute pixel coordinates (integers)
        int x = (int)msg->inject_touch_event.position.point.x;
        int y = (int)msg->inject_touch_event.position.point.y;

        snprintf(json_str, 512,
                 "{\"type\":\"%s\",\"data\":{\"pointer_id\":\"%s\","
                 "\"x\":%d,\"y\":%d,\"pressure\":%.6f,"
                 "\"width\":%u,\"height\":%u}}",
                 type_str, pointer_id_str,
                 x, y, msg->inject_touch_event.pressure,
                 device_width, device_height);
        break;
    }
    case SC_CONTROL_MSG_TYPE_INJECT_SCROLL_EVENT:
    {
        // Use absolute pixel coordinates (integers)
        int x = (int)msg->inject_scroll_event.position.point.x;
        int y = (int)msg->inject_scroll_event.position.point.y;

        // Determine if it's horizontal or vertical scroll
        if (msg->inject_scroll_event.hscroll != 0)
        {
            snprintf(json_str, 512,
                     "{\"type\":\"scroll_h\",\"data\":{\"x\":%d,\"y\":%d,"
                     "\"hscroll\":%.6f,\"width\":%u,\"height\":%u}}",
                     x, y, (float)msg->inject_scroll_event.hscroll,
                     device_width, device_height);
        }
        else
        {
            snprintf(json_str, 512,
                     "{\"type\":\"scroll_v\",\"data\":{\"x\":%d,\"y\":%d,"
                     "\"vscroll\":%.6f,\"width\":%u,\"height\":%u}}",
                     x, y, (float)msg->inject_scroll_event.vscroll,
                     device_width, device_height);
        }
        break;
    }
    default:
        // Skip unsupported message types
        free(json_str);
        return NULL;
    }

    return json_str;
}

bool la_websocket_deserialize_event(const char *json_str, struct sc_control_msg *msg)
{
    if (!json_str || !msg)
    {
        return false;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root)
    {
        LOGE("Failed to parse JSON: %s", json_str);
        return false;
    }

    bool success = false;
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *data_item = cJSON_GetObjectItemCaseSensitive(root, "data");

    if (!cJSON_IsString(type_item) || !cJSON_IsObject(data_item))
    {
        LOGE("Invalid JSON format: missing type or data");
        goto end;
    }

    const char *type = type_item->valuestring;

    if (strcmp(type, "key") == 0)
    {
        msg->type = SC_CONTROL_MSG_TYPE_INJECT_KEYCODE;

        cJSON *action_item = cJSON_GetObjectItemCaseSensitive(data_item, "action");
        cJSON *keycode_item = cJSON_GetObjectItemCaseSensitive(data_item, "keycode");
        cJSON *repeat_item = cJSON_GetObjectItemCaseSensitive(data_item, "repeat");
        cJSON *metastate_item = cJSON_GetObjectItemCaseSensitive(data_item, "metastate");

        if (cJSON_IsString(action_item) && cJSON_IsNumber(keycode_item))
        {
            if (strcmp(action_item->valuestring, "down") == 0)
                msg->inject_keycode.action = AKEY_EVENT_ACTION_DOWN;
            else if (strcmp(action_item->valuestring, "up") == 0)
                msg->inject_keycode.action = AKEY_EVENT_ACTION_UP;
            else
            {
                LOGE("Invalid key action: %s", action_item->valuestring);
                goto end;
            }

            msg->inject_keycode.keycode = (enum android_keycode)keycode_item->valueint;
            msg->inject_keycode.repeat = repeat_item ? repeat_item->valueint : 0;
            msg->inject_keycode.metastate = metastate_item ? (enum android_metastate)metastate_item->valueint : 0;
            success = true;
        }
    }
    else if (strcmp(type, "text") == 0)
    {
        msg->type = SC_CONTROL_MSG_TYPE_INJECT_TEXT;

        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(data_item, "text");
        if (cJSON_IsString(text_item) && text_item->valuestring)
        {
            msg->inject_text.text = strdup(text_item->valuestring);
            success = (msg->inject_text.text != NULL);
        }
    }
    else if (strcmp(type, "touch_down") == 0 ||
             strcmp(type, "touch_up") == 0 ||
             strcmp(type, "touch_move") == 0)
    {
        msg->type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;

        if (strcmp(type, "touch_down") == 0)
            msg->inject_touch_event.action = AMOTION_EVENT_ACTION_DOWN;
        else if (strcmp(type, "touch_up") == 0)
            msg->inject_touch_event.action = AMOTION_EVENT_ACTION_UP;
        else
            msg->inject_touch_event.action = AMOTION_EVENT_ACTION_MOVE;

        cJSON *pointer_id_item = cJSON_GetObjectItemCaseSensitive(data_item, "pointer_id");
        cJSON *x_item = cJSON_GetObjectItemCaseSensitive(data_item, "x");
        cJSON *y_item = cJSON_GetObjectItemCaseSensitive(data_item, "y");
        cJSON *pressure_item = cJSON_GetObjectItemCaseSensitive(data_item, "pressure");
        cJSON *width_item = cJSON_GetObjectItemCaseSensitive(data_item, "width");
        cJSON *height_item = cJSON_GetObjectItemCaseSensitive(data_item, "height");

        if (cJSON_IsString(pointer_id_item) && cJSON_IsNumber(x_item) && cJSON_IsNumber(y_item) &&
            cJSON_IsNumber(width_item) && cJSON_IsNumber(height_item))
        {
            msg->inject_touch_event.pointer_id = strtoull(pointer_id_item->valuestring, NULL, 10);
            msg->inject_touch_event.position.point.x = x_item->valueint;
            msg->inject_touch_event.position.point.y = y_item->valueint;
            msg->inject_touch_event.position.screen_size.width = width_item->valueint;
            msg->inject_touch_event.position.screen_size.height = height_item->valueint;
            msg->inject_touch_event.pressure = pressure_item ? (float)pressure_item->valuedouble : 1.0f;

            // Defaults
            msg->inject_touch_event.action_button = 0;
            msg->inject_touch_event.buttons = 0;

            success = true;
        }
    }
    else if (strcmp(type, "scroll_h") == 0 || strcmp(type, "scroll_v") == 0)
    {
        msg->type = SC_CONTROL_MSG_TYPE_INJECT_SCROLL_EVENT;

        cJSON *x_item = cJSON_GetObjectItemCaseSensitive(data_item, "x");
        cJSON *y_item = cJSON_GetObjectItemCaseSensitive(data_item, "y");
        cJSON *width_item = cJSON_GetObjectItemCaseSensitive(data_item, "width");
        cJSON *height_item = cJSON_GetObjectItemCaseSensitive(data_item, "height");

        if (cJSON_IsNumber(x_item) && cJSON_IsNumber(y_item) &&
            cJSON_IsNumber(width_item) && cJSON_IsNumber(height_item))
        {
            msg->inject_scroll_event.position.point.x = x_item->valueint;
            msg->inject_scroll_event.position.point.y = y_item->valueint;
            msg->inject_scroll_event.position.screen_size.width = width_item->valueint;
            msg->inject_scroll_event.position.screen_size.height = height_item->valueint;
            msg->inject_scroll_event.buttons = 0;

            if (strcmp(type, "scroll_h") == 0)
            {
                cJSON *hscroll_item = cJSON_GetObjectItemCaseSensitive(data_item, "hscroll");
                msg->inject_scroll_event.hscroll = hscroll_item ? (float)hscroll_item->valuedouble : 0.0f;
                msg->inject_scroll_event.vscroll = 0.0f;
            }
            else
            {
                cJSON *vscroll_item = cJSON_GetObjectItemCaseSensitive(data_item, "vscroll");
                msg->inject_scroll_event.vscroll = vscroll_item ? (float)vscroll_item->valuedouble : 0.0f;
                msg->inject_scroll_event.hscroll = 0.0f;
            }
            success = true;
        }
    }
    else
    {
        LOGE("Unknown message type: %s", type);
    }

end:
    cJSON_Delete(root);
    return success;
}

struct la_websocket_client *
la_websocket_client_init(const char *url, la_websocket_on_message_cb on_message,
                         void *userdata)
{
    struct la_websocket_client *client = malloc(sizeof(*client));
    if (!client)
    {
        LOGE("Failed to allocate WebSocket client");
        return NULL;
    }

    memset(client, 0, sizeof(*client));

    client->url = strdup(url);
    if (!client->url)
    {
        LOGE("Failed to duplicate URL");
        free(client);
        return NULL;
    }

    // Parse URL
    if (!parse_websocket_url(url, &client->protocol, &client->address,
                             &client->port, &client->path))
    {
        LOGE("Failed to parse WebSocket URL: %s", url);
        free(client->url);
        free(client);
        return NULL;
    }

    client->on_message = on_message;
    client->userdata = userdata;
    client->connected = false;
    client->running = true;
    client->thread_started = false;
    client->context = NULL;
    client->wsi = NULL;
    client->send_len = 0;

    pthread_mutex_init(&client->lock, NULL);

    LOGI("LinkAndroid WebSocket client initialized for: %s", url);
    LOGI("Parsed: %s://%s:%d%s", client->protocol, client->address,
         client->port, client->path);

    // Start connection thread
    if (pthread_create(&client->thread, NULL, websocket_thread, client) != 0)
    {
        LOGE("Failed to create WebSocket thread");
        free(client->protocol);
        free(client->address);
        free(client->path);
        free(client->url);
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NULL;
    }

    client->thread_started = true;
    LOGI("WebSocket connection thread started");

    return client;
}

bool la_websocket_client_send(struct la_websocket_client *client, const char *json)
{
    if (!client)
    {
        LOGD("WebSocket client is NULL");
        return false;
    }

    pthread_mutex_lock(&client->lock);

    if (!client->connected || !client->wsi)
    {
        // Not connected, print to stdout as fallback
        pthread_mutex_unlock(&client->lock);
        printf("[WebSocket Event] %s\n", json);
        fflush(stdout);
        return false;
    }

    size_t json_len = strlen(json);
    if (json_len > MAX_PAYLOAD_SIZE)
    {
        LOGE("JSON payload too large: %zu bytes", json_len);
        pthread_mutex_unlock(&client->lock);
        return false;
    }

    // Allocate new node with padding for LWS
    struct message_node *node = malloc(sizeof(struct message_node));
    if (!node)
    {
        LOGE("Failed to allocate message node");
        pthread_mutex_unlock(&client->lock);
        return false;
    }

    // Allocate payload with LWS_PRE padding
    node->payload = malloc(LWS_PRE + json_len + 1);
    if (!node->payload)
    {
        LOGE("Failed to allocate message payload");
        free(node);
        pthread_mutex_unlock(&client->lock);
        return false;
    }

    node->len = json_len;
    node->next = NULL;

    // Copy data after padding
    memcpy(node->payload + LWS_PRE, json, json_len);
    node->payload[LWS_PRE + json_len] = '\0'; // Null terminate for debugging

    // Add to queue
    if (client->queue_tail)
    {
        client->queue_tail->next = node;
        client->queue_tail = node;
    }
    else
    {
        client->queue_head = node;
        client->queue_tail = node;
    }

    // LOGD("Sending WebSocket message: %s", json);

    // Request write callback - REMOVED unsafe call from this thread
    // lws_callback_on_writable(client->wsi);

    // Wake up the service thread to handle the write request immediately
    if (client->context)
    {
        lws_cancel_service(client->context);
    }

    pthread_mutex_unlock(&client->lock);

    return true;
}

void la_websocket_client_send_event(struct la_websocket_client *client,
                                    const struct sc_control_msg *msg,
                                    uint16_t device_width,
                                    uint16_t device_height)
{
    char *json = serialize_event_to_json(msg, device_width, device_height);
    if (!json)
    {
        // Message type not supported or serialization failed (e.g., hover events)
        // Silently ignore unsupported events
        // LOGD("WebSocket event serialization skipped for unsupported message type %d", msg->type);
        return;
    }

    bool sent = la_websocket_client_send(client, json);
    if (!sent)
    {
        // Fallback: message was printed to stdout by la_websocket_client_send
        LOGD("WebSocket send failed, event printed to stdout");
    }

    free(json);
}

bool la_websocket_client_is_connected(struct la_websocket_client *client)
{
    if (!client)
    {
        return false;
    }

    pthread_mutex_lock(&client->lock);
    bool connected = client->connected;
    pthread_mutex_unlock(&client->lock);

    return connected;
}

void la_websocket_client_destroy(struct la_websocket_client *client)
{
    if (!client)
    {
        return;
    }

    // Signal thread to stop
    client->running = false;

    // Cancel libwebsockets service to exit immediately
    if (client->context)
    {
        lws_cancel_service(client->context);
    }

    if (client->thread_started)
    {
        pthread_join(client->thread, NULL);
    }

    // Clear output queue
    pthread_mutex_lock(&client->lock);
    struct message_node *node = client->queue_head;
    while (node)
    {
        struct message_node *next = node->next;
        free(node->payload);
        free(node);
        node = next;
    }
    client->queue_head = NULL;
    client->queue_tail = NULL;
    pthread_mutex_unlock(&client->lock);

    pthread_mutex_destroy(&client->lock);

    free(client->protocol);
    free(client->address);
    free(client->path);
    free(client->url);
    free(client);

    LOGI("LinkAndroid WebSocket client destroyed");
}
