# 5b 菜单移植规格（从 Go rime.go 移植到 MoqiDaemon）

> 状态源：2026-11-02 下午开工。本文是实现的权威规格；Go 侧原文 =
> moqi-ime/input_methods/rime/rime.go（buildMenu:3436, onCommand:969,
> 常量:25-118）、appearance_config.go、appearance_themes.go、cloud_clipboard.go、
> scheme_set.go。黄金菜单树 = F:\work\IME\probe5.py 输出（26 项）。

## 分发原则

- daemon 自建全树（onMenu 本地作答），命令按 id：本地执行 or 代理 Go（5b v1）。
- 代理集（v1）：10 部署 / 11 同步 / 17 更新配置 / 23 下载方案集 / 215 导入皮肤 /
  5001-5004 WebDAV·云剪贴板 / 3000+n 方案集切换（redeploy+launcher 协作）/ 
  打开置顶短语·超级简拼·成对符号设置（文件模板逻辑）。
- 本地集：2000+n 开关 / 1000+n 方案 / 12,13,14,16 文件夹 / 21,22 帮助讨论 /
  30,32,33 维护回执（launcher 执行）/ 210 输入状态共享 / 220,221 输入设置 /
  外观全表（100-225,4000+n 主题）。

## 树结构（= probe5 黄金）

1. 开关区：menuSwitches() 顺序 = backend.SaveOptions() 顺序（librime 保存的
   开关表，来自 default.yaml/patch），每项 id=2000+i，text=switchMenuText
   （enabled 时 "states[1] → states[0]"，否则 "states[0] → states[1]"），
   checked=GetOption。
   - **v1 开关表来源**：rime 引擎侧无 C API 直接给 schema switches；v1 采用
     解析当前 schema yaml 的 switches 块（含 states）+ default.yaml 的
     save_options 顺序；与 probe5 黄金逐项 diff 验证（白霜 8 项：
     ascii_mode/full_shape/traditionalization/翻译/emoji/拆分/煋/ascii_punct/固顶）。
2. 分隔线（text:""）
3. 切换方案集（submenu）：availableSchemeSets()=%APPDATA%\Moqi 下非点开头目录
   （首项恒 default "Rime"），checked=currentSchemeSetName()（读
   scheme_set 配置文件），尾加分隔线+"下载方案集(&D)" id=23。
4. 输入方案(&I)：schemaMenuItems——rime RimeGetSchemaList，text=Name||ID，
   id=1000+i，checked=CurrentSchemaID。
5. 打开置顶短语 / 打开超级简拼 / 更新配置(&P) id=17 / 刷新配置(&R) id=10 / 分隔线
6. 输入状态共享 id=210 checked=input_state_shared（appearance_config.json）
7. 外观(&A)：切换主题（listThemes：内置表+用户 appearance_themes.json，
   id=4000+i，text=Label，checked=cfg.candidate_theme==id）/ 行内预编辑 100 /
   候选排列 170,171 / 每行候选数 180-183（横排才 enabled）/ 候选间距 184-189 /
   总候选数量 190-193 / 字体大小 113-127 / 候选文字字体 194-197 /
   注释文字大小 213-225 / 注释文字字体 198-201 / 候选框背景 120-122 /
   高亮颜色 130-132 / 字体颜色 140-142 / 高亮文字颜色 145-147 / 分隔线 /
   导入皮肤 215
8. 输入设置：自动插入成对符号 220 / 打开成对符号设置 / 分号键次选 221
9. 同步设置：WebDAV 配置 5004 / 测试 WebDAV 5003 / 启用云剪贴板 5002(checked=
   cloud cfg.enabled) / 云剪贴板设置 5001 / 同步用户词库 11
10. 维护：开启调试 30(checked=launcher 配置 logLevel==debug|trace) /
    重启引擎 32 / 退出引擎 33
11. 打开文件夹(&O)：用户 14 / 共享 13 / 同步 12 / 日志 16
12. 分隔线 / 帮助文档(&H) 21 / 参加讨论(&J) 22

URL：helpDocsURL=https://moqiyinxing.chunqiujinjing.com/index；
discussionURL=https://github.com/gaboolic/moqi-im-windows/discussions

## 命令语义（本地集）

- toggleOption(name)：rimeSetOption(session, name, !get)。shared 输入态
  （ascii_mode/traditionalization/ascii_punct/full_shape ∩ save_options）且
  input_state_shared → 写 appearance_config.json shared_options；
  其余 save_options 内的 → synced_options。
- 方案：rimeSelectSchema(session, schema_id) + 持久化 current_schema_id。
- 外观命令：改 appearance_config.json 对应字段（id↔字段映射同上表 checked 逻辑）
  + resp.customizeUi（键名=customizeUIMap：candFontName/candFontSize/
  candPerRow/candSpacing/candUseCursor/inlinePreedit/autoPairQuotes/
  candBackgroundColor/candHighlightColor/candTextColor/candHighlightTextColor/
  candCommentFontSize/candCommentFontName/...）+ changeButton（updateLangStatus
  图标不必需：开关键盘状态没变）。**跨进程生效**：appearance_config.json 是
  DLL/daemon 引擎会话建立时读的桥；RPC 模式客户端由 resp.customizeUi 即时生效。
- 文件夹/URL：ShellExecuteW("open")；日志目录先 MkdirAll。
- 30/32/33：回执即可（launcher 在代理路径上拦截执行——注意：**onCommand 现在
  由 daemon 本地答，launcher 依然能看到该请求并执行**，因为它代理时对
  METHOD_ON_COMMAND 一律调用 handleLauncherCommand ✓ 行为不变）。

## 响应形状

- onMenu resp：success/seqNum/returnValue=1 + menuItems（JsonCompat 契约：
  叶子 submenu:[] 可省略——DLL 零值守卫已兼容，但**保持与 Go 一致发空数组**，
  黄金 diff 才逐字节对齐）。
- onCommand resp：success/seqNum/returnValue=1/0 + customizeUi(可选)。

## 验证

1. 黄金树 diff：daemon onMenu 输出 vs probe5 抓的 Go 输出（结构化递归对比，
   忽略 checked 动态项、逐项列出差异）。
2. 命令冒烟：开关切换后 rimeGetOption 反读；外观命令后 appearance_config.json
   断言；文件夹命令 explorer 进程；5004 等 id 确认走代理（Go 日志有痕）。
3. 用户实机：右键菜单逐区点击。

## 5c（本文件只记范围）

AI 热键（filterKeyDown 里的 AI 分支——daemon 目前本地答导致 AI 断路，需先补
"AI 热键转发 Go"再移植 WinHTTP 客户端）；云剪贴板 WebDAV；更新配置/下载方案集
（网络+launcher 协作）；然后 Go 子进程、MoqiLaunch 退休（daemon 绑管道+自启）。
