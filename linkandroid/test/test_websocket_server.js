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

// Check if panel should be shown

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


const panelConfig = {
  type: 'panel',
  data: {
    buttons: [
      { id: 'home', icon: 'home' },
      { id: 'back', icon: 'back' },
      { id: 'recent', icon: 'task' },
      { id: 'volume_up', icon: 'v-plus' },
      { id: 'volume_down', icon: 'v-minus' },
      { id: 'screenshot', icon: 'screenshot' },
      { id: 'quit', icon: 'quit' },
      { id: 'follow', icon: 'follow_active' },  // 文字和图标二选一，这里演示使用图标
      { id: 'active', text: '激活窗口' },  // 文字和图标二选一，这里演示使用文字
      { id: 'toggle_top', icon: 'top', text: '□置顶' },  // 文字和图标二选一，这里演示使用图标
    ]
  }
};

wss.on('connection', (ws, req) => {
  const clientIp = req.socket.remoteAddress;
  console.log(`[${new Date().toISOString()}] Client connected from ${clientIp}`);
  console.log(`Waiting for messages...`);

  const delay = (ms) => new Promise(resolve => setTimeout(resolve, ms));

  async function playbackEvents(logArray) {
    if (logArray.length === 0) return;

    // 解析第一条的时间作为基准
    const getTs = (log) => new Date(log.match(/\[(.*?)\]/)[1]).getTime();
    const getPayload = (log) => JSON.parse(log.split('Received Event:')[1]);

    const startTime = getTs(logArray[0]);

    for (let i = 0; i < logArray.length; i++) {
      const currentTs = getTs(logArray[i]);
      const payload = getPayload(logArray[i]);

      // 计算当前事件与上一个事件的差值 (如果是第一条则为 0)
      const waitTime = i === 0 ? 0 : currentTs - getTs(logArray[i - 1]);

      if (waitTime > 0) {
        await delay(waitTime);
      }

      // 发送数据
      ws.send(JSON.stringify(payload));

      console.log(`[Playback] Sent ${payload.type} at +${currentTs - startTime}ms`);
    }
  }

  // Send panel configuration with test buttons (including Emoji and Chinese)
  // Only send if --linkandroid-panel-show flag is present


  // Optional: Send periodic key events for testing (disabled by default)
  // Uncomment the code below to enable automated key event testing
  setInterval(() => {
    // ws.send(JSON.stringify({
    //   type: 'key',
    //   data: {
    //     action: "down",
    //     keycode: 3,
    //   }
    // }));
    // setTimeout(() => {
    //   ws.send(JSON.stringify({
    //     type: 'key',
    //     data: {
    //       action: "up",
    //       keycode: 3,
    //     }
    //   }));
    // }, 100);
    //     const rawLogs = `
    // [2026-02-06T02:06:54.248Z] Received Event:{"type":"touch_down","data":{"pointer_id":"18446744073709551615","x":457,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.430Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":461,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.450Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":465,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.462Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":473,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.490Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":489,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.498Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":512,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.517Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":540,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.529Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":567,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.548Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":590,"y":654,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.565Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":614,"y":657,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.591Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":633,"y":661,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.601Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":649,"y":665,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.615Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":661,"y":665,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.631Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":665,"y":665,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.647Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":669,"y":669,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:54.665Z] Received Event:{"type":"touch_move","data":{"pointer_id":"18446744073709551615","x":673,"y":669,"pressure":1,"width":1080,"height":2336}}
    // [2026-02-06T02:06:55.414Z] Received Event:{"type":"touch_up","data":{"pointer_id":"18446744073709551615","x":673,"y":669,"pressure":0,"width":1080,"height":2336}}
    // `
    //     playbackEvents(rawLogs.split('\n').filter(line => line.trim() !== ''));

    ws.send(JSON.stringify({ type: 'active' }));

  }, 3000);

  ws.on('message', (data) => {
    try {
      const message = data.toString();
      const event = JSON.parse(message);
      // Log with color coding based on event type
      console.log(`\n[${new Date().toISOString()}] Received Event:${JSON.stringify(event)}`);
      if (event.type === 'ready') {
        console.log('\n[INFO] Sending panel configuration with buttons...');
        ws.send(JSON.stringify(panelConfig));
        console.log('[INFO] Panel configuration sent with', panelConfig.data.buttons.length, 'buttons');
      } else if (event.type === 'panel_button_click') {
        if ('follow' === event.data.id) {
          // 切换跟随按钮状态（如果使用文字模式）
          const followBtn = panelConfig.data.buttons.find(b => b.id === 'follow');
          if (followBtn.icon === 'follow') {
            followBtn.icon = 'follow_active';
          } else {
            followBtn.icon = 'follow';
          }
          // 图标模式下状态切换由客户端处理
          ws.send(JSON.stringify(panelConfig));
        } else if ('quit' === event.data.id) {
          console.log('\n[INFO] Quit button clicked, sending quit command to scrcpy...');
          ws.send(JSON.stringify({ type: 'quit' }));
          console.log('[INFO] Quit command sent, scrcpy should exit gracefully');
        } else if ('active' === event.data.id) {
          console.log('\n[INFO] Active button clicked, sending active command to scrcpy...');
          ws.send(JSON.stringify({ type: 'active' }));
          console.log('[INFO] Active command sent, scrcpy window should be brought to front');
        } else if ('toggle_top' === event.data.id) {
          // 切换置顶按钮状态
          const topBtn = panelConfig.data.buttons.find(b => b.id === 'toggle_top');
          let isEnabled = false;
          if (topBtn.text) {
            // 文字模式
            isEnabled = topBtn.text === '✓置顶';
            if (isEnabled) {
              topBtn.text = '□置顶';
            } else {
              topBtn.text = '✓置顶';
            }
          } else {
            // 图标模式 - 使用数据字段标记状态
            isEnabled = topBtn._enabled || false;
            topBtn._enabled = !isEnabled;
          }
          ws.send(JSON.stringify(panelConfig));
          // 发送置顶命令
          console.log('\n[INFO] Top button clicked, sending top command to scrcpy...');
          ws.send(JSON.stringify({
            type: 'top',
            data: {
              enable: !isEnabled
            }
          }));
          console.log(`[INFO] Top command sent, window always-on-top: ${!isEnabled}`);
        }
      }
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
