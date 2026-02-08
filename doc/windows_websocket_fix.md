# Windows WebSocket 连接修复

## 问题描述

在 Windows 上使用 `--linkandroid-server` 参数运行投屏时，程序直接显示无响应并退出，错误代码为 `3489660927` (0xD0000FFF)。

## 错误原因

1. **错误的 libwebsockets 初始化选项**: 代码中使用了 `LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT` 选项，但构建配置中明确禁用了 SSL（`-DLWS_WITH_SSL=OFF`），导致在 Windows 上初始化时崩溃。

2. **缺少详细的错误日志**: 在 Windows 上 libwebsockets 初始化失败时，没有足够的诊断信息帮助定位问题。

3. **libwebsockets 构建配置不完整**: 没有明确禁用不需要的依赖项（libuv、libevent、glib），可能导致不必要的依赖冲突。

## 修复方案

### 1. 移除不兼容的 SSL 初始化选项

在 `linkandroid/src/websocket_client.c` 中:

```c
// 修复前
info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

// 修复后
info.options = 0;  // Minimal options for client-only usage
```

### 2. 添加 Windows 特定的错误诊断

```c
#ifdef _WIN32
#include <windows.h>
#endif

// 在错误处理中添加
#ifdef _WIN32
LOGE("Windows error code: %lu", GetLastError());
LOGE("Make sure ws2_32.dll and required dependencies are available");
#endif
```

### 3. 优化 libwebsockets 构建配置

在 `app/deps/libwebsockets.sh` 中添加:

```cmake
-DLWS_WITH_LIBUV=OFF
-DLWS_WITH_LIBEVENT=OFF
-DLWS_WITH_GLIB=OFF
```

### 4. 改进日志和错误提示

- 添加了详细的连接日志
- WebSocket 连接失败时不会导致程序崩溃，而是降级为无 WebSocket 模式继续运行
- 提供清晰的警告信息指示 WebSocket 功能已禁用

## 测试建议

1. **基本连接测试**:
   ```bash
   scrcpy.exe --serial <device> --linkandroid-server ws://localhost:10667/server
   ```

2. **连接失败场景**: 确保在 WebSocket 服务器未运行时，程序能正常降级运行

3. **完整功能测试**: 确认在正常连接时，事件转发和预览功能正常工作

## 技术细节

### Windows 上的 libwebsockets

- libwebsockets 需要 `ws2_32.dll` (Winsock2)
- 在程序启动时通过 `net_init()` 调用 `WSAStartup()` 初始化 Winsock
- libwebsockets 上下文创建在独立线程中进行，确保不阻塞主线程

### 错误代码解析

- `0xD0000FFF` (3489660927): 通常表示访问违规或非法操作
- 在这个场景下，由于尝试初始化未编译进库的 SSL 功能导致

## 预期效果

修复后，在 Windows 上:

1. ✅ 程序不会因为 WebSocket 初始化失败而崩溃
2. ✅ 提供清晰的错误日志帮助诊断问题
3. ✅ 在 WebSocket 服务器可用时能正常建立连接
4. ✅ 在 WebSocket 服务器不可用时能降级运行（无 WebSocket 功能）

## 相关文件

- `linkandroid/src/websocket_client.c` - WebSocket 客户端实现
- `app/deps/libwebsockets.sh` - libwebsockets 构建脚本
- `app/src/input_manager.c` - WebSocket 初始化调用
- `app/meson.build` - Windows 链接配置
