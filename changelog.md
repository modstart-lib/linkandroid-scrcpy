# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Features

- Add automated WebSocket protocol test runner (`linkandroid/test/run_tests.js`) covering panel lifecycle (create/update/clear/re-add), button types (icon/text/mixed), edge cases (max 32, over-capacity, special chars/emoji), commands (active, top, key injection), and graceful quit.
- 新增 WebSocket 协议自动化测试脚本（`linkandroid/test/run_tests.js`），覆盖面板生命周期（创建/更新/清除/重新添加）、按钮类型（图标/文字/混合）、边界场景（最多 32 个、超容量、特殊字符/表情符号）、命令（激活窗口、置顶、按键注入）以及优雅断开连接等测试用例。

### Improvements

- Refactor `--linkandroid-panel-show` panel behavior: panel is now dynamic — hidden by default and shown/hidden based on WebSocket data rather than reserving space at startup.
- 重构 `--linkandroid-panel-show` 面板行为：面板现在是动态的，默认隐藏，根据 WebSocket 数据动态显示/隐藏，不再在启动时预留空间。

- Add `panel_enabled` field to decouple panel feature toggle from panel visibility state; panel hides (restoring full-screen video) when an empty button list is received.
- 新增 `panel_enabled` 字段，将面板功能开关与面板可见性状态解耦；收到空按钮列表时面板自动隐藏（恢复全屏视频布局）。

- Add `panel_layout_dirty` flag to force content rect recalculation on the next render cycle when panel layout changes, avoiding stale layout.
- 新增 `panel_layout_dirty` 标志，在面板布局变化时强制下一渲染周期重新计算内容矩形，避免布局陈旧。

- Guard `ready_event_sent` flag behind successful WebSocket send; flag is only set when the send actually succeeds.
- 将 `ready_event_sent` 标志的设置移至 WebSocket 发送成功后，仅在发送真正成功时才标记。
