#!/usr/bin/env node

/**
 * LinkAndroid Automated WebSocket Protocol Test Runner
 *
 * Tests all --linkandroid related WebSocket features:
 *   - Panel create / update / clear / re-add (menu lifecycle)
 *   - Panel button types: icon, text, mixed
 *   - Panel edge cases: max capacity (32), over-capacity (40), special chars
 *   - Commands: active, top enable/disable, key injection
 *   - Quit: graceful disconnect
 *
 * Usage (from project root):
 *   node linkandroid/test/run_tests.js
 *
 * Or via make:
 *   make dev-test
 * scrcpy is launched the same as `make debug-preview` (video enabled, UI shown),
 * so the `ready` event fires after the first frame is rendered.
 *

 */

'use strict';

const { WebSocketServer } = require('ws');
const { spawn } = require('child_process');
const path = require('path');

// ─── Config ──────────────────────────────────────────────────────────────────

const PORT             = 63006;
const WS_PATH          = '/scrcpy';
const PROJECT_ROOT     = path.join(__dirname, '../..');
const CONNECT_TIMEOUT  = 30000;   // ms to wait for scrcpy to connect
const READY_TIMEOUT    = 15000;   // ms to wait for 'ready' event
const CMD_SETTLE       = 350;     // ms to wait after each command send
const STEP_PAUSE       = 5000;    // ms to pause after each test for visual observation
const QUIT_CLOSE_WAIT  = 5000;    // ms to wait for connection to close after quit

// ─── State ───────────────────────────────────────────────────────────────────

let passed  = 0;
let failed  = 0;
let skipped = 0;

// ─── Helpers ─────────────────────────────────────────────────────────────────

function log(msg) { process.stdout.write(msg + '\n'); }

function pass(id, name) {
    passed++;
    log(`  \x1b[32m✓\x1b[0m ${id}: ${name}`);
}

function fail(id, name, reason) {
    failed++;
    log(`  \x1b[31m✗\x1b[0m ${id}: ${name} — ${reason}`);
}

function skip(id, name, reason) {
    skipped++;
    log(`  \x1b[33m~\x1b[0m ${id}: ${name} — SKIPPED (${reason})`);
}

function send(ws, obj) {
    ws.send(JSON.stringify(obj));
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function isOpen(ws) {
    return ws.readyState === ws.OPEN;
}

function waitForClose(ws, timeout) {
    if (ws.readyState === ws.CLOSED) return Promise.resolve(true);
    return new Promise((resolve) => {
        const t = setTimeout(() => resolve(false), timeout);
        ws.once('close', () => { clearTimeout(t); resolve(true); });
    });
}

// ─── Test scenarios ───────────────────────────────────────────────────────────

async function runTests(ws, receivedMessages) {

    // ── T02: Ready event ──────────────────────────────────────────────────────
    if (receivedMessages.some((m) => m.type === 'ready')) {
        pass('T02', 'Ready event received');
    } else {
        fail('T02', 'Ready event received', 'ready event not received');
    }
    await sleep(STEP_PAUSE);

    // Helper: send a panel message, verify connection survives, then pause
    async function testPanel(id, name, buttons) {
        send(ws, { type: 'panel', data: { buttons } });
        await sleep(CMD_SETTLE);
        if (isOpen(ws)) pass(id, name);
        else fail(id, name, 'connection closed after send');
        await sleep(STEP_PAUSE);
    }

    // ── T03: Panel create — icon buttons ─────────────────────────────────────
    await testPanel('T03', 'Panel: create with icon buttons', [
        { id: 'home',   icon: 'home'  },
        { id: 'back',   icon: 'back'  },
        { id: 'recent', icon: 'task'  },
    ]);

    // ── T04: Panel update — text buttons ─────────────────────────────────────
    await testPanel('T04', 'Panel: update with text buttons', [
        { id: 'btn1', text: 'Home'  },
        { id: 'btn2', text: 'Back'  },
        { id: 'btn3', text: '返回'   },
    ]);

    // ── T05: Panel update — mixed icon + text ─────────────────────────────────
    await testPanel('T05', 'Panel: mixed icon+text buttons', [
        { id: 'home',  icon: 'home'     },
        { id: 'label', text: '首页'      },
        { id: 'back',  icon: 'back'     },
        { id: 'quit',  text: 'Quit 退出' },
    ]);

    // ── T06: Panel update — replace all buttons ───────────────────────────────
    await testPanel('T06', 'Panel: replace all buttons', [
        { id: 'screenshot',  icon: 'screenshot' },
        { id: 'volume_up',   icon: 'v-plus'     },
        { id: 'volume_down', icon: 'v-minus'    },
        { id: 'quit',        icon: 'quit'       },
    ]);

    // ── T07: Panel clear — empty buttons (panel hides, full-screen video) ─────
    await testPanel('T07', 'Panel: clear with empty buttons (panel hides, full-screen video)', []);

    // ── T08: Panel re-add after clear ─────────────────────────────────────────
    await testPanel('T08', 'Panel: re-add buttons after clear', [
        { id: 'home', icon: 'home' },
        { id: 'back', icon: 'back' },
    ]);

    // ── T09: Panel delete again ───────────────────────────────────────────────
    await testPanel('T09', 'Panel: delete again (empty buttons → panel hides again)', []);

    // ── T10: Panel max capacity — exactly 32 buttons ──────────────────────────
    const maxButtons = Array.from({ length: 32 }, (_, i) => ({ id: `b${i}`, text: `B${i}` }));
    await testPanel('T10', 'Panel: 32 buttons (SC_MAX_PANEL_BUTTONS, exact max)', maxButtons);

    // ── T11: Panel over capacity — 40 buttons (graceful truncation to 32) ─────
    const overButtons = Array.from({ length: 40 }, (_, i) => ({ id: `x${i}`, text: `X${i}` }));
    await testPanel('T11', 'Panel: 40 buttons (over-capacity, truncated gracefully to 32)', overButtons);

    // ── T12: Panel special characters — emoji + Chinese ───────────────────────
    await testPanel('T12', 'Panel: special chars (emoji + Chinese text)', [
        { id: 'emoji1',   text: '🏠 Home'  },
        { id: 'emoji2',   text: '⬅️ Back'  },
        { id: 'chinese1', text: '主页'      },
        { id: 'chinese2', text: '返回'      },
        { id: 'mixed',    text: '📷截图'    },
    ]);

    // ── T13: Panel clear after special chars ──────────────────────────────────
    await testPanel('T13', 'Panel: clear special-char buttons', []);

    // ── T14: Command active ───────────────────────────────────────────────────
    send(ws, { type: 'active' });
    await sleep(CMD_SETTLE);
    if (isOpen(ws)) pass('T14', 'Command: active (raise window to foreground)');
    else fail('T14', 'Command: active', 'connection closed');
    await sleep(STEP_PAUSE);

    // ── T15: Command top enable ───────────────────────────────────────────────
    send(ws, { type: 'top', data: { enable: true } });
    await sleep(CMD_SETTLE);
    if (isOpen(ws)) pass('T15', 'Command: top enable=true (always-on-top)');
    else fail('T15', 'Command: top enable=true', 'connection closed');
    await sleep(STEP_PAUSE);

    // ── T16: Command top disable ──────────────────────────────────────────────
    send(ws, { type: 'top', data: { enable: false } });
    await sleep(CMD_SETTLE);
    if (isOpen(ws)) pass('T16', 'Command: top enable=false (disable always-on-top)');
    else fail('T16', 'Command: top enable=false', 'connection closed');
    await sleep(STEP_PAUSE);

    // ── T17: Key injection — KEYCODE_HOME (3) ────────────────────────────────
    send(ws, { type: 'key', data: { action: 'down', keycode: 3 } });
    await sleep(100);
    send(ws, { type: 'key', data: { action: 'up',   keycode: 3 } });
    await sleep(CMD_SETTLE);
    if (isOpen(ws)) pass('T17', 'Command: key injection (KEYCODE_HOME down+up)');
    else fail('T17', 'Command: key injection', 'connection closed');
    await sleep(STEP_PAUSE);

    // ── T18: Panel add → active → clear cycle (full interaction flow) ─────────
    await testPanel('T18', 'Panel: full lifecycle — add buttons', [
        { id: 'home',       icon: 'home'        },
        { id: 'back',       icon: 'back'        },
        { id: 'recent',     icon: 'task'        },
        { id: 'volume_up',  icon: 'v-plus'      },
        { id: 'volume_down',icon: 'v-minus'     },
        { id: 'screenshot', icon: 'screenshot'  },
        { id: 'follow',     icon: 'follow_active'},
        { id: 'active',     text: '激活窗口'     },
        { id: 'toggle_top', icon: 'top', text: '□置顶' },
        { id: 'quit',       icon: 'quit'        },
    ]);
    send(ws, { type: 'active' });
    await sleep(CMD_SETTLE);
    await testPanel('T18b', 'Panel: full lifecycle — clear (panel disappears)', []);

    // ── T19: Quit — graceful disconnect ───────────────────────────────────────
    send(ws, { type: 'quit' });
    const closed = await waitForClose(ws, QUIT_CLOSE_WAIT);
    if (closed) pass('T19', 'Command: quit (scrcpy disconnects gracefully)');
    else fail('T19', 'Command: quit', `connection did not close within ${QUIT_CLOSE_WAIT / 1000}s`);
}

// ─── Main ─────────────────────────────────────────────────────────────────────

async function main() {
    log('\n════════════════════════════════════════════════════════════');
    log('  LinkAndroid WebSocket Protocol — Automated Test Runner');
    log('════════════════════════════════════════════════════════════\n');

    // ── Start WS server ───────────────────────────────────────────────────────
    const wss = new WebSocketServer({ port: PORT, path: WS_PATH });
    log(`[setup] WebSocket server listening on ws://127.0.0.1:${PORT}${WS_PATH}`);

    // ── Start scrcpy ──────────────────────────────────────────────────────────
    log(`[setup] Spawning scrcpy from ${PROJECT_ROOT}...`);
    const scrcpy = spawn('./run', [
        'x',
        '-V', 'debug',
        '--linkandroid-server', `ws://127.0.0.1:${PORT}${WS_PATH}`,
        '--linkandroid-panel-show',
    ], {
        cwd: PROJECT_ROOT,
        stdio: ['ignore', 'pipe', 'pipe'],
    });

    let scrcpyDied = false;
    scrcpy.stderr.on('data', (d) => process.stderr.write(d));
    scrcpy.stdout.on('data', (d) => process.stdout.write(d));
    scrcpy.on('exit', (code, sig) => {
        scrcpyDied = true;
        if (code !== 0 && code !== null) {
            log(`\n[scrcpy] process exited (code=${code}, signal=${sig})`);
        }
    });

    // ── Wait for connection ───────────────────────────────────────────────────
    log(`[setup] Waiting for scrcpy WebSocket connection (up to ${CONNECT_TIMEOUT / 1000}s)...`);
    log(`[setup] Make sure an Android device is connected via ADB.\n`);

    let ws;
    const receivedMessages = [];

    try {
        ws = await new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                reject(new Error(`No connection within ${CONNECT_TIMEOUT / 1000}s — is a device connected?`));
            }, CONNECT_TIMEOUT);

            // Also fail fast if scrcpy exits before connecting
            const poll = setInterval(() => {
                if (scrcpyDied) {
                    clearTimeout(timer);
                    clearInterval(poll);
                    reject(new Error('scrcpy process exited before connecting'));
                }
            }, 200);

            wss.once('connection', (socket) => {
                clearTimeout(timer);
                clearInterval(poll);
                socket.on('message', (data) => {
                    try { receivedMessages.push(JSON.parse(data.toString())); } catch {}
                });
                resolve(socket);
            });
        });
    } catch (err) {
        fail('T01', 'Connection established', err.message);
        scrcpy.kill();
        wss.close();
        log('\n════════════════════════════════════════════════════════════');
        log(`  Results: 0 passed, 1 failed, 0 skipped`);
        log('════════════════════════════════════════════════════════════\n');
        process.exit(1);
    }

    pass('T01', 'Connection established');

    // ── Wait for 'ready' event ────────────────────────────────────────────────
    log(`[setup] Waiting for 'ready' event (up to ${READY_TIMEOUT / 1000}s)...`);
    await new Promise((resolve) => {
        if (receivedMessages.some((m) => m.type === 'ready')) return resolve();
        const deadline = Date.now() + READY_TIMEOUT;
        const poll = setInterval(() => {
            if (receivedMessages.some((m) => m.type === 'ready') || Date.now() >= deadline) {
                clearInterval(poll);
                resolve();
            }
        }, 100);
    });

    // ── Run tests ─────────────────────────────────────────────────────────────
    log('\n[tests] Running all test scenarios...\n');
    try {
        await runTests(ws, receivedMessages);
    } catch (err) {
        log(`\n[fatal] Unexpected error during tests: ${err.message}`);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    await sleep(500);
    if (!scrcpyDied) scrcpy.kill('SIGTERM');
    wss.close();

    // ── Summary ───────────────────────────────────────────────────────────────
    const total = passed + failed + skipped;
    log('\n════════════════════════════════════════════════════════════');
    log(`  Results: \x1b[32m${passed} passed\x1b[0m, \x1b[31m${failed} failed\x1b[0m, \x1b[33m${skipped} skipped\x1b[0m  (${total} total)`);
    log('════════════════════════════════════════════════════════════\n');

    process.exit(failed > 0 ? 1 : 0);
}

main().catch((err) => {
    console.error('[fatal]', err);
    process.exit(1);
});
