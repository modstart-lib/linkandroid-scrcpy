# LinkAndroid Test WebSocket Server

This is a simple Node.js WebSocket server for testing the linkandroid-scrcpy event forwarding feature.

## Installation

```bash
cd linkandroid/test
npm install
```

## Usage

### 1. Start the test server

```bash
npm start
```

The server will listen on `ws://127.0.0.1:6000/scrcpy`

### 2. Run scrcpy with the --linkandroid-server option

In another terminal:

```bash
cd /Users/mz/data/project/linkandroid/linkandroid-scrcpy
./run x --linkandroid-server ws://127.0.0.1:6000/scrcpy
```

### 3. Interact with the device

- **Click/tap** on the scrcpy window
- **Type text** with your keyboard
- **Scroll** with mouse wheel or trackpad
- **Swipe** by clicking and dragging

All events will be forwarded to the WebSocket server and printed in real-time.

## Event Types

The server receives JSON events in the following format:

### Key Event
```json
{
  "type": "key",
  "data": {
    "action": "down",
    "keycode": 62,
    "repeat": 0,
    "metastate": 0,
    "width": 1080,
    "height": 2400
  }
}
```

### Text Event
```json
{
  "type": "text",
  "data": {
    "text": "Hello World",
    "width": 1080,
    "height": 2400
  }
}
```

### Touch Events (touch_down, touch_move, touch_up)
```json
{
  "type": "touch_down",
  "data": {
    "pointer_id": 0,
    "x": 0.5,
    "y": 0.5,
    "pressure": 1.0,
    "width": 1080,
    "height": 2400
  }
}
```

**Note**: `x` and `y` are normalized coordinates (0.0-1.0) relative to device dimensions.

### Scroll Events (scroll_h, scroll_v)
```json
{
  "type": "scroll_v",
  "data": {
    "x": 0.5,
    "y": 0.5,
    "vscroll": -1.0,
    "width": 1080,
    "height": 2400
  }
}
```

## Stopping the Server

Press `Ctrl+C` to gracefully shut down the server.

## Troubleshooting

### Port already in use
If port 6000 is already in use, edit `test_websocket_server.js` and change the `PORT` constant to another value (e.g., 6001), then update the scrcpy command accordingly.

### Connection refused
Make sure the server is running before starting scrcpy with the `--linkandroid-server` option.

### No events received
1. Check that scrcpy is connected to a device
2. Verify the WebSocket URL in the scrcpy command matches the server URL
3. Try interacting with the scrcpy window (click, type, scroll)
