#!/usr/bin/env node

/**
 * Test WebSocket Server for linkandroid-scrcpy
 * 
 * This server receives and logs all events forwarded by scrcpy
 * when using the --linkandroid-server option.
 * 
 * Usage:
 *   npm install
 *   npm start
 * 
 * Then run scrcpy with:
 *   ./run x --linkandroid-server ws://127.0.0.1:6000/scrcpy
 */

const { WebSocketServer } = require('ws');

const PORT = 63006;
const PATH = '/scrcpy';

const wss = new WebSocketServer({
  port: PORT,
  path: PATH
});

console.log(`\n==============================================`);
console.log(`LinkAndroid Test WebSocket Server`);
console.log(`==============================================`);
console.log(`Listening on: ws://127.0.0.1:${PORT}${PATH}`);
console.log(`Waiting for scrcpy connection...`);
console.log(`==============================================\n`);

wss.on('connection', (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  console.log(`[${new Date().toISOString()}] Client connected from ${clientIp}`);
  console.log(`Waiting for messages...`);

  // Send panel configuration with test buttons (including Emoji and Chinese)
  // setTimeout(() => {
  console.log('\n[INFO] Sending panel configuration with buttons...');
  const panelConfig = {
    type: 'panel',
    data: {
      buttons: [
        { id: 'home', text: '首页' },
        { id: 'back', text: '返回' },
        { id: 'recent', text: '任务' },
        { id: 'volume_up', text: '声音+' },
        { id: 'volume_down', text: '声音-' },
        { id: 'screenshot', text: '截图' },
      ]
    }
  };
  ws.send(JSON.stringify(panelConfig));
  console.log('[INFO] Panel configuration sent with', panelConfig.data.buttons.length, 'buttons');
  // }, 1000);

  // Optional: Send periodic key events for testing (disabled by default)
  // Uncomment the code below to enable automated key event testing
  /*
  setInterval(() => {
    ws.send(JSON.stringify({
      type: 'key',
      data: {
        action: "down",
        keycode: 3,
      }
    }));
    setTimeout(() => {
      ws.send(JSON.stringify({
        type: 'key',
        data: {
          action: "up",
          keycode: 3,
        }
      }));
    }, 100);
  }, 3000);
  */

  ws.on('message', (data) => {
    try {
      const message = data.toString();
      const event = JSON.parse(message);
      // Log with color coding based on event type
      console.log(`\n[${new Date().toISOString()}] Received Event:${JSON.stringify(event)}`);
    } catch (err) {
      console.error(`[${new Date().toISOString()}] Error parsing message:`, err.message);
      console.error('Raw message:', data.toString());
    }
  });

  ws.on('close', () => {
    console.log(`\n[${new Date().toISOString()}] Client disconnected`);
  });

  ws.on('error', (err) => {
    console.error(`[${new Date().toISOString()}] WebSocket error:`, err.message);
  });
});

wss.on('error', (err) => {
  console.error('Server error:', err.message);
  process.exit(1);
});

// Handle graceful shutdown
process.on('SIGINT', () => {
  console.log('\n\nShutting down server...');
  wss.close(() => {
    console.log('Server closed');
    process.exit(0);
  });
});

process.on('SIGTERM', () => {
  console.log('\n\nShutting down server...');
  wss.close(() => {
    console.log('Server closed');
    process.exit(0);
  });
});
