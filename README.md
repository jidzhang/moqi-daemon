# MoqiHelper（阶段 5：Go 后端 C++ 重写）

用 C++ 重写 `MoqiIME.exe`（Go 后端，11.5MB，运行时占底），目标产物 ~1-2MB。
新建独立仓（用户 2026-11-01 拍板"现在就干"），通过相对路径复用
`../moqi-im-windows` 的已验证源码，不复制不 fork。

## 背景与架构定位

架构重构（阶段 1-4，见 docs/24-交接-2026-11-01）后，热路径已进程内化
（TSF DLL 内 LocalEngine 直调 rime.dll），Go 后端只剩：

1. **RPC 回退路径**：x86 进程 / 引擎加载失败场景的按键处理
   （启动器把客户端命名管道代理到后端的 stdin/stdout，4 字节长度前缀 +
   protojson 兼容 JSON 帧）
2. 冷路径：语言栏菜单/方案切换/外观下发、AI 热键、云剪贴板、usage 日志

Go 侧这些逻辑（~24k 行）中，**按键路径语义已在阶段 2 以 C++ 落地**
（LocalRerank：三段重排 + displayPerm + typo memory；LocalEngine：键码
转换/去重/分号二选/响应构建）——本仓直接复用，不重写第二遍。

## 复用关系（关键决策）

- `../moqi-im-windows/MoqiTextService/RimeCore.h/.cpp`（新提取）：rime.dll
  扁平 API 加载器 + traits/部署 + 键码转换（KeyEvent→rime keysym）
- `../moqi-im-windows/MoqiTextService/LocalRerank.h/.cpp`：重排/typo memory
  （无 TextService 耦合）
- `../moqi-im-windows/proto/ProtoFraming.h`：帧格式
- jsoncpp：`../moqi-im-windows/jsoncpp` 子目录构建
- 耦合点在 CMake 以相对路径声明；将来可抽公共静态库解耦。

## 协议（与现行 JSON 管道逐字节兼容）

- 帧：`uint32 LE 长度 + JSON 载荷`（stdin 读请求 / stdout 写响应）
- 请求字段：`method`（符号名）、`seqNum`、`clientId`（启动器注入）、
  `guid`、`keyEvent{charCode,keyCode,repeatCount,scanCode,isExtended,keyStates[256]}`
  等；响应字段：`success/returnValue/seqNum/clientId/compositionString/
  candidateEntries/candidateCursor/commitString/...`（protojson 命名）
- 启动器按响应 `clientId` 路由回各客户端

## 奇偶矩阵（Parity Matrix，逐项补齐后才能替换 Go）

| 功能 | Go 现状 | Helper 阶段 | 状态 |
|---|---|---|---|
| init/activate/deactivate | ✓ | A | 本次 |
| 按键四方法（filter/on Key down/up） | ✓ | A | 本次 |
| highlight/select/changePage | ✓ | A | 本次 |
| compositionTerminated | ✓ | A | 本次 |
| 引擎会话/部署/RimeInProc | ✓（Go 版） | A | 本次 |
| onPreservedKey/onKeyboardStatus | 空实现 | A（同样空） | 本次 |
| **语言栏按钮/菜单**（addButtons/onMenu/onCommand：方案切换/维护菜单/主题） | ✓ | B | 待 |
| **customizeUI 外观下发** | ✓ | B | 待 |
| **AI 热键**（网络，默认关） | ✓ | C（或砍） | 待 |
| **云剪贴板**（网络+剪贴板） | ✓ | C（或砍） | 待 |
| custom_phrase / super_abbrev | ✓ | B/C | 待 |
| usage 日志 | ✓ | C | 待 |
| scheme_set 方案集管理 | ✓ | B | 待 |

替换策略：奇偶矩阵全绿前，Go 后端继续随包发布；矩阵绿后把
backends.json 指向 helper。

## 目录

```
moqi-helper/
  CMakeLists.txt       # 复用 ../moqi-im-windows 源码的构建
  src/main.cpp         # stdin/stdout 帧循环 + 分发
  src/HelperEngine.cpp # 每客户端会话 + 方法实现（移植自 LocalEngine）
  src/HelperEngine.h
```

## 验证

帧级手测：`echo <framed json> | MoqiHelper.exe`（见 docs 验证脚本）；
端到端：backends.json 切后端 → x86 应用 / MOQI_LOCAL_ENGINE=0 打字冒烟。
