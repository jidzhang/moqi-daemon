# 5e 云剪贴板/WebDAV 移植规格（Go → MoqiDaemon C++）

> 2026-11-02 夜整理。来源 = moqi-ime 实读（cloud_clipboard.go 617 行 /
> cloudclipboard 包 1484 行 / user_dict_sync.go / hotkey / password /
> cloud_clipboard_config.go），本文是移植契约，实现照此写，不必再读 Go。
> **架构前提：剪贴板监听原在 MoqiLaunch.PipeServer（AddClipboardFormatListener
> + 500ms 防抖 → BackendServer → METHOD_CLOUD_CLIPBOARD_UPLOAD=19 走 stdin 给
> Go）。MoqiLaunch 已退休 → 上传链路自 5c 起断裂，daemon 必须接管监听。**

## 1. WebDAV 客户端（cloudclipboard/client.go → C++ WinHTTP）

- Basic Auth（用户名:密码 base64 于 Authorization 头），30s 超时，http/https。
- 方法与判定：
  - `PROPFIND url Depth:0|1`，body 固定 XML（`d:propfind`→`getlastmodified`+`displayname`），
    Content-Type: text/xml; charset=utf-8。2xx=在（207 常态）；401→"认证失败：请检查
    用户名和密码。飞牛须使用「可见文件夹范围」所属账号登录"；其他→"无法访问 WebDAV
    根路径 HTTP %d。飞牛地址请填 http://192.168.x.x:5005/（注意末尾 /）"。
  - 解析 multistatus：每个 `d:response` 的 `d:displayname` + `d:getlastmodified`
    （HTTP 日期串→Unix ms）。
  - `MKCOL url`（创建目录；405=已存在也算 directoryExists 兜底通过）。
  - `PUT url text/plain; charset=utf-8` 2xx 即成，否则 "PUT failed: HTTP %d"。
  - `GET` 全量 body；`DELETE` 2xx 或 404 均成功。
- 目录布局（paths.go）：`SettingsRoot`（默认 `/moqi-input-method/`，归一：空→默认、
  补前导/尾随 `/`）→ `clip/`（剪贴板条目）与 `dict/<方案集>/<设备ID>/`（词库快照）。
  URL 拼法 = TrimRight(base,"/") + "/" + TrimPrefix(path,"/")。
- ensureRemoteDirectory：PROPFIND 根(401/非2xx 报上面文案) → 无 SettingsRoot 则
  MKCOL（失败两次检查后报"设置目录 %s 不存在且无法自动创建…"）→ 无 clip/ 则 MKCOL。
- ListClips：ensure + PROPFIND clip Depth1 → 过滤 IsClipFileName 去重 → 按
  LastModified 降序。ListUserDictSnapshots：dict/<set>/ Depth1 → 每设备目录再
  Depth1 → `.userdb.txt` 条目。

## 2. 守卫/命名（guard.go）

- 文件名 = MD5(hex 小写 32) + ".txt"；IsClipFileName = 32 位小写 hex.txt（无 `/`）
  或前缀 `clip_`（兼容旧）。
- IsUploadableText：长度 2..32768、Trim 非空、非掩码串（掩码字符 `*•●·` 占 ≥80% 不传）。
- ShouldUpload：Enabled && IsComplete && !applyingRemote && uploadable && md5 !=
  lastUploadedHash。
- NormalizePlainText = TrimSpace；FormatPreview：空白折叠单行、超长截断加 `…`
  （候选显示上限 60 字符，preview 全量上限 240）；空文本显示 `（空）`。
- IsUserDictSnapshotFileName：`.userdb.txt` 结尾、无 `/\:`、base 非空非点。
- IsSyncDeviceDirName：非空/非点/≤128、字符 ∈ [字母数字-_.]。IsSchemeSetDirName：
  非 0x20、无 `/\:`、≤128 runes。

## 3. 配置与凭据

- `%APPDATA%\Moqi\cloud_clipboard.json`：
  `{enabled, base_url, username, settings_root, min_interval_sec(默认10),
   list_hotkey(默认"Ctrl+0"；旧默认 Ctrl+Shift+0/Ctrl+Shift+V 读取时归一为新默认)}`。
  密码不入文件。
- 密码 = `%APPDATA%\Moqi\cloud_clipboard.pw`：CryptProtectData/UnprotectData（DPAPI，
  无 entropy），空密码=删文件。C++ 直接 crypt32 同名 API。
- IsComplete = base_url&&username&&password 非空；IsAllowedBaseURL = http(s):// 前缀
  （否则文案"WebDAV 地址须以 http:// 或 https:// 开头"）。
- 命令 id：5001 云剪贴板设置 5002 启用开关 5003 测试连接 5004 WebDAV 配置
  （+11 同步用户词库）。

## 4. 同步引擎（sync.go → daemon 常驻模块）

- 状态：cfg、client(enabled&&complete&&allowed 才建)、lastUploadAt、lastUploadedHash、
  isUploadInFlight、pendingText、debounce(500ms)、applyingRemote(粘贴远端后 300ms
  抑制上传)。
- UploadFromRequest(text)：normalize→空弃；applying/重复 hash/不可上传→弃；否则
  ScheduleUpload（500ms 防抖取最新）。performUpload：in-flight→挂 pending；间隔
  <minIntervalSec→延迟重排；hash 相同弃；PUT md5.txt。失败重试 pending。
- ListClipsForDisplay：ListClips→截 40 条→逐条 GET 全文→preview/fulltext。
- 词库同步（user_dict_sync.go）：本地根 = <rime用户目录>\sync；流程 =
  DownloadUserDictSnapshots(远端 dict/<方案集>/ 所有设备快照到 sync\<设备>\) →
  librime SyncUserData（合并）→ 读 installation.yaml 的 installation_id 为设备 ID →
  UploadUserDictSnapshots(sync\<本机ID>\ 全部 .userdb.txt 上传)。并发去重 running
  标志；通知文案"用户词库同步完成：下载 N，上传 M"。
- rime 用户目录 = %APPDATA%\Moqi\<当前方案集>（scheme_set.json current，默认 Rime）。

## 5. UI 集成（cloud_clipboard.go 编排层）

- **列表选择器**（候选窗承载）：热键 Ctrl+0（可配）或 preserved key
  GUID {9d9f5a64-1a18-47f0-9a4f-7cbf2f5a44d9} 唤起；未启用→消息窗"请先在设置中开启
  云剪贴板"2500ms；配置不全→"请先完成 WebDAV 配置"；列表空→"云剪贴板为空"。
  加载失败→"加载失败: …"。展示：组字串=`云剪贴板 页/总页`，每页 9 条（preview+
  时间注释 mm-dd HH:mm），候选数字/空格/回车上屏全文（applyingRemote 置 300ms）、
  `-`/`=` 翻页、↑↓ 移动、Delete 删远端条目（删空提示"云剪贴板已清空"）、Esc/其他键
  退出。选中项高亮同步（candidateIndex RPC）。
- **上传触发**（daemon 接管 MoqiLaunch 角色）：隐藏窗口
  AddClipboardFormatListener + 500ms 防抖（WM_APP+3 自定义 flush 消息模式）→ 读
  剪贴板 CF_UNICODETEXT → UploadFromRequest。配置未启用时监听可挂空。
- **托盘通知**（Go TrayNotification）：现架构无托盘 → 统一改为 ShowMessage
  （消息窗，时长 2500-3000ms）或异步推送帧。设置保存/连接测试/同步完成等文案照抄。
- **两个设置对话框**（cloud_clipboard_dialog_windows.go，296 行 Win32）：
  WebDAV 配置 = base_url/username/password(留空=保留)/settings_root + KeepPassword
  语义（勾"保留密码"且新密码空→不覆盖）；云剪贴板设置 = enabled/min_interval_sec/
  list_hotkey。保存→saveCloudClipboardConfig + 通知。**C++ 移植可用 CreateDialog
  内存模板或直接读配置文件的"打开文件"过渡（与配置AI助手同款），终态做对话框。**
- 测试连接（5003）：强制 Enabled=true 建 client → ensureRemoteDirectory →
  通知"WebDAV 连接成功"/"连接失败: …"。

## 6. 64 位进程内引擎的接入点

- 热键：TSF preserved key（激活响应 addPreservedKey 注册 Ctrl+0+GUID）→
  onPreservedKey RPC → daemon 弹列表；DLL 的 chord 转发路径当前仅"有组字时"发
  RPC——无组字按 Ctrl+0 需靠 preserved key 机制（Go 同款双保险）。
- 词库同步对 64 位：daemon 执行 SyncUserData 合并的是 daemon 会话引擎的 userdb
  （%APPDATA%\Moqi\<方案集>）；进程内引擎用 RimeInProc（userdb 关闭）→ 不受影响、
  也无收益（与既有已知缺口一致）。

## 7. 实施顺序（建议）

1. DaemonWebDAV.cpp（WinHTTP 客户端+守卫/命名+MD5）+ 单测：本地起极简 WebDAV
   假服务（python http.server 扩展 PROPFIND/MKCOL/PUT/GET/DELETE）验证五方法。
2. DaemonCloudConfig.cpp（json+DPAPI 密码）+ 命令 5002/5003 接线（通知用消息窗）。
3. 剪贴板监听（隐藏窗+防抖）+ Sync 引擎（上传链）。
4. 列表选择器状态机（DaemonEngine，参照 AI 覆盖层但带分页/删除）+ 热键/preserved key。
5. 词库同步命令 11。6. 设置对话框（或先"打开配置文件"过渡）。7. 全链真机验证
   （真 NAS 由用户验，本地假服务过自动化）。
