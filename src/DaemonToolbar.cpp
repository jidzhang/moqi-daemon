// 输入工具栏实现（2026-11-19）。设计说明见 DaemonToolbar.h。
//
// 关键实现点：
//  1. 常驻窗口，不用定时器轮询状态——状态变化由 main.cpp 的命令回调路径
//     主动触发重绘（InvalidateRect），避免空转 CPU。
//  2. WS_EX_NOACTIVATE + WM_MOUSEACTIVATE 返回 MA_NOACTIVATE：点工具栏不能
//     把焦点从用户的输入框抢走，否则组字会中断、光标会跳。
//  3. 定位用 SPI_GETWORKAREA（工作区）而非整屏，避开任务栏。
//  4. 中文标点/全角等状态用底色高亮表示，不切换图标——这样图标只需要一套。
#include "DaemonToolbar.h"

#include <windows.h>
#include <windowsx.h>

#include "OsVersion.h"
#include "RimeCore.h"  // moqiAppDataDir
#include <fstream>

#include <atomic>
#include <mutex>
#include <thread>

namespace Moqi {

// main.cpp 提供（同命名空间内的轻量日志）。
void daemonLogExtern(const std::wstring& msg);

namespace {

constexpr wchar_t kClassName[] = L"MoqiDaemonToolbar";
constexpr UINT_PTR kRefreshTimerId = 0x7b;
// 状态刷新节拍：daemon 拿不到"引擎选项变了"的事件通知（那是 DLL 侧的事），
// 只能用低频轮询兜底。250ms 足够跟手，CPU 代价可忽略（一次 gather 只读
// 三个 option）。用户按键触发的路径会有更快的主动刷新。
constexpr UINT kRefreshIntervalMs = 250;

// 命令 id（与 DaemonMenuCore::CommandIds 对齐）。
constexpr int kCmdMenu = 0;  // 0 = 打开菜单（本地约定，不发给 daemon）
constexpr int kCmdModeIcon = 1;
constexpr int kCmdFullShape = 3;
constexpr int kCmdAsciiPunct = 4;

// 尺寸：用户 2026-11-19 两轮反馈定稿——先嫌留白大（按钮从 36 收到 22），
// 又嫌整体太小（放大 150%）。22*1.5≈33、24*1.5=36，同时内边距/间距跟着
// 放大，保持比例。文字用粗体（见 onPaint 的 lfWeight），小字号下更实。
// 2026-11-19 定稿：33x36 基础上再缩 5%（用户"再变小一点点"）。
// 2026-11-20：上下留白与字间距再收 10%（用户："上下留白和字与字中间的间距
// 缩小10%"）。字是独立字号，不动它——只把按钮盒子压小，视觉上留白自然变窄。
constexpr int kBtnWidth = 28;
constexpr int kBtnHeight = 31;
constexpr int kBtnGap = 2;
constexpr int kPad = 2;
constexpr int kEdgeGap = 12;
constexpr int kBtnCount = 4;
// 窗口圆角半径（像素）。窗口外形 rgn 与绘制用同一个值。
constexpr int kCornerRadius = 8;

// 配色（2026-11-19 用户定稿第二轮）：
//   字 = 谷歌蓝 #4285F4（用户："用谷歌输入法的蓝"）
//   底 = 候选高亮色再调浅（用户："背景再浅一些"）
//   边框 = 去掉（用户："边框去掉"，靠底色和投影边界已足够）
//   字色 = 黑（2026-11-20 用户："字还是改成黑字吧，蓝色看着不清楚"；
//          谷歌蓝在浅蓝底上对比度不足，黑字最清楚）
constexpr COLORREF kBg = RGB(232, 240, 254);        // 浅蓝底
constexpr COLORREF kHoverBg = RGB(214, 228, 252);   // 悬停加深一档
constexpr COLORREF kText = RGB(0, 0, 0);            // 黑字

std::atomic<bool> g_running{false};
std::atomic<HWND> g_hwnd{nullptr};
// 活跃客户端数（onActivate 加、onDeactivate 减）。只有 > 0 才显示工具栏：
// 用户切到墨奇输入法 → 所有宿主里的 TSF 都会 onActivate，计数 > 0；
// 切到英文键盘/别的输入法 → 全部 onDeactivate，计数归零，工具栏隐藏。
std::atomic<int> g_activeClients{0};
std::atomic<bool> g_visibleWanted{false};
ToolbarCallbacks g_callbacks;
ToolbarState g_state;
// g_state 由后台轮询线程写、UI 线程读（绘制 / 点击逻辑），加锁保护。
std::mutex g_stateMutex;

// 后台轮询线程 → UI 线程的"该刷新了"信号。
// 2026-11-20 用户实报两连：鼠标滑上去变等待光标、"过了好多秒之后才可以点击"、
// 点一下弹"程序无响应"。根因是 WM_TIMER 在**界面线程**上调 readState /
// 前台判定，而 readState 要走 rime 的内部锁（另一端可能正在部署/加载词典），
// Engine 侧还有冷启动。UI 线程一被拖住，窗口就不泵消息 → 系统判定无响应。
//
// 现在所有跨进程/加锁的操作都搬进轮询线程，界面线程只做绘制和窗口操作。
constexpr UINT kMsgRefresh = WM_APP + 0x51;
// 轮询线程算出的"前台是我们服务的宿主"，界面线程据此显隐。
std::atomic<bool> g_fgOurs{false};
// 状态变了（轮询线程置位，界面线程 exchange 后重绘）。
std::atomic<bool> g_stateDirty{false};
int g_hovered = -1;
// 拖动状态：g_dragging 期间窗口跟手，松手把位置记进 g_userPos。
// 有用户位置之后，refreshToolbarVisibility 不再把窗口拉回默认右下角。
bool g_dragging = false;
// g_dragArmed：按下了但还没确定是点击还是拖动（移动超过阈值才转拖动）。
bool g_dragArmed = false;
POINT g_pressPos = {};
int g_pressedButton = -1;
POINT g_dragOffset = {};
POINT g_userPos = {};
bool g_hasUserPos = false;

// 只画悬停底色，不画状态高亮（2026-11-19 用户定）：中/英、全/半 这些字
// 本身就是状态，"中"就是中文态、"英"就是英文态，再加蓝底是重复信息，
// 反而让 4 个按钮看起来像一堆色块。悬停底色保留——那是鼠标反馈不是状态。
void paintButton(HDC hdc, HFONT font, const RECT& rc, const wchar_t* label,
                 bool hovered) {
    if (hovered) {
        HBRUSH b = ::CreateSolidBrush(kHoverBg);
        ::FillRect(hdc, &rc, b);
        ::DeleteObject(b);
    }
    HGDIOBJ old = ::SelectObject(hdc, font);
    ::SetTextColor(hdc, kText);
    ::SetBkMode(hdc, TRANSPARENT);
    RECT tr = rc;
    // 中文句号「。」在全角字框里天然偏左下（字形设计如此），DT_CENTER 居中
    // 的是字框不是墨迹，所以视觉上明显靠左。这里按字宽比例右移一点补偿
    //（2026-11-19 用户："句号尽量居中吧，太靠左了"；逗号本身位置正常不动）。
    if (label != nullptr && label[0] == L'。') {
        const int dx = (rc.right - rc.left) / 5;
        tr.left += dx;
        tr.right += dx;
    }
    ::DrawTextW(hdc, label, -1, &tr,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(hdc, old);
}

// 按钮标签。中/英、全/半用汉字（16px 下比符号清晰），标点用实际字符。
void buttonLabel(int index, wchar_t* out, size_t n) {
    const wchar_t* s = L"";
    // 状态由轮询线程写：读一份快照再用，避免撕裂（三个 bool 不保证原子打包）。
    ToolbarState st;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        st = g_state;
    }
    switch (index) {
        case 0:
            // 菜单按钮用"墨"（用户 2026-11-19 定，替代原来的 ≡）。
            // 汉字在 16-26px 下比几何符号清晰，也和后面三个按钮风格统一。
            s = L"\u58A8";
            break;
        case 1:
            s = st.ascii ? L"\u82F1" : L"\u4E2D";
            break;
        case 2:
            s = st.asciiPunct ? L"," : L"\u3002";
            break;
        case 3:
            s = st.fullShape ? L"\u5168" : L"\u534A";
            break;
        default:
            break;
    }
    wcsncpy_s(out, n, s, _TRUNCATE);
}

RECT buttonRect(int index) {
    RECT rc = {};
    rc.left = kPad + index * (kBtnWidth + kBtnGap);
    rc.top = kPad;
    rc.right = rc.left + kBtnWidth;
    rc.bottom = rc.top + kBtnHeight;
    return rc;
}

int hitTest(POINT pt) {
    for (int i = 0; i < kBtnCount; ++i) {
        RECT rc = buttonRect(i);
        if (pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top &&
            pt.y < rc.bottom) {
            return i;
        }
    }
    return -1;
}

// ---------- 联动纯逻辑（单测锁定；见 toolbar_plan_test.cpp） ----------

// 工具栏命令 id（与 main.cpp 的 executeCommand 对齐）。
constexpr int kPlanCmdMode = 1;    // ascii_mode 开关
constexpr int kPlanCmdPunct = 4;   // ascii_punct 开关

struct ToolbarPlan {
    bool sendMode;   // 是否发中英开关
    bool sendPunct;  // 是否发标点开关
    bool asciiAfter;  // 执行后的 ascii 期望值（本地即时回显）
    bool punctAfter;  // 执行后的标点期望值
};

// 点「中/英」按钮的联动计划：切英文必配英文标点、切中文必配中文标点
//（用户定调"切到英文后不得英文字母加英文标点"；中文态下允许单独切标点，
// 但中英切换始终把标点带过去）。
ToolbarPlan planLangToggle(bool ascii, bool punct) {
    const bool target = !ascii;
    ToolbarPlan p{};
    p.sendMode = true;
    p.sendPunct = (punct != target);
    p.asciiAfter = target;
    p.punctAfter = target;
    return p;
}

// 点「。/,」按钮的联动计划：切到中文标点时，若当前是英文输入则一并切回
// 中文（用户定调"从英文标点切换到中文标点需要同步切换到中文输入"）；
// 英文输入态下切英文标点不动中英；中文态切英文标点也不动中英。
ToolbarPlan planPunctToggle(bool ascii, bool punct) {
    const bool target = !punct;
    ToolbarPlan p{};
    p.sendPunct = true;
    p.sendMode = (!target && ascii);  // 回中文标点 && 当前英文 -> 带回中文
    p.punctAfter = target;
    p.asciiAfter = p.sendMode ? false : ascii;
    return p;
}

// ---------- 执行（消费上面的计划） ----------

void executeLang() {
    bool ascii = false;
    bool punct = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ascii = g_state.ascii;
        punct = g_state.asciiPunct;
    }
    const ToolbarPlan plan = planLangToggle(ascii, punct);
    if (g_callbacks.executeCommand) {
        if (plan.sendMode) g_callbacks.executeCommand(kPlanCmdMode);
        if (plan.sendPunct) g_callbacks.executeCommand(kPlanCmdPunct);
    }
    // 本地先落状态：视觉立刻响应，真实引擎状态由轮询线程随后校准
    //（executeCommand 现在丢给后台线程，不再同步等引擎）。
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.ascii = plan.asciiAfter;
    g_state.asciiPunct = plan.punctAfter;
}

// 标点按钮：切到中文标点时，若当前是英文输入则一并切回中文。
void executePunct() {
    bool ascii = false;
    bool punct = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        ascii = g_state.ascii;
        punct = g_state.asciiPunct;
    }
    const ToolbarPlan plan = planPunctToggle(ascii, punct);
    if (g_callbacks.executeCommand) {
        if (plan.sendPunct) g_callbacks.executeCommand(kPlanCmdPunct);
        if (plan.sendMode) g_callbacks.executeCommand(kPlanCmdMode);
    }
    std::lock_guard<std::mutex> lock(g_stateMutex);
    g_state.asciiPunct = plan.punctAfter;
    g_state.ascii = plan.asciiAfter;
}

void showFunctionMenu(HWND owner) {
    if (!g_callbacks.buildMenuTree) return;
    const Json::Value items = g_callbacks.buildMenuTree();
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;

    // JSON 树 -> HMENU（与 DLL 侧 menuFromJson 同一套字段语义）。
    struct Builder {
        static void append(HMENU m, const Json::Value& arr) {
            for (const auto& it : arr) {
                if (!it.isObject()) continue;
                if (it.get("separator", false).asBool()) {
                    ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                    continue;
                }
                const std::string textUtf8 = it.get("text", "").asString();
                // UTF-8 -> 宽字符。菜单文本全是中文，按字节塞进 wstring 会乱码
                //（2026-11-19 用户实报"墨字弹出来的菜单是乱码"）。
                const std::wstring text = utf8ToWide(textUtf8.c_str());
                const int id = it.get("id", 0).asInt();
                const bool checked = it.get("checked", false).asBool();
                const bool enabled = it.get("enabled", true).asBool();
                const Json::Value& sub = it["submenu"];
                if (sub.isArray() && !sub.empty()) {
                    HMENU child = ::CreatePopupMenu();
                    if (child == nullptr) continue;
                    append(child, sub);
                    UINT f = MF_POPUP;
                    if (!enabled) f |= MF_GRAYED;
                    ::AppendMenuW(m, f, reinterpret_cast<UINT_PTR>(child),
                                  text.c_str());
                    continue;
                }
                UINT f = MF_STRING;
                if (checked) f |= MF_CHECKED;
                if (!enabled || id == 0) f |= MF_GRAYED;
                ::AppendMenuW(m, f, static_cast<UINT_PTR>(id), text.c_str());
            }
        }
    };
    Builder::append(menu, items);
    if (::GetMenuItemCount(menu) <= 0) {
        ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"\uFF08\u5F15\u64CE\u672A\u5C31\u7EEA\uFF09");
    }

    POINT pt = {};
    ::GetCursorPos(&pt);
    // 弹菜单前抢前台，事后 WM_NULL：否则菜单收不起来（同语言栏右键的坑）。
    ::SetForegroundWindow(owner);
    const UINT cmd = ::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
        pt.x, pt.y, 0, owner, nullptr);
    ::PostMessage(owner, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    if (cmd != 0 && g_callbacks.executeCommand) {
        g_callbacks.executeCommand(static_cast<int>(cmd));
    }
}

void onPaint(HWND hwnd) {
    PAINTSTRUCT ps = {};
    HDC hdc = ::BeginPaint(hwnd, &ps);
    RECT rc = {};
    ::GetClientRect(hwnd, &rc);

    // 底色**必须铺在内存位图上**，不能只铺屏幕 DC（2026-11-20 第二次修）。
    // 上一次只 FillRect 了屏幕 DC，紧接着 BitBlt 又把整块盖回内存位图的内容，
    // 而 GDI 的 RoundRect 在右/下边界各留 1px 没填——露出来的就是位图新建
    // 时的黑底。用户实报"边框还是有啊，黑色的"，逐像素量出来正是窗口最后
    // 一行/最后一列纯黑。
    HDC mem = ::CreateCompatibleDC(hdc);
    HBITMAP bmp = ::CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = ::SelectObject(mem, bmp);

    HBRUSH bg = ::CreateSolidBrush(kBg);
    ::FillRect(mem, &rc, bg);
    // 无边框（2026-11-19 用户："边框去掉"）。四角由窗口 rgn 裁成圆角，
    // 位图里直接铺满底色即可，不再描任何线。
    ::DeleteObject(bg);

    // 按钮字体：**固定微软雅黑 + 常规字重**（2026-11-20 用户 Win7 实报
    // "工具栏的字明显更粗"）。之前跟随系统 UI 字体并强制 FW_BOLD：Win10
    // 的 lfMessageFont 是 Segoe UI（笔画细，加粗后正好），Win7 中文系统是
    // 微软雅黑——雅黑本身笔画就粗，再叠 Bold 在小字号下笔画粘连发糊。
    // 雅黑常规体的视觉粗细已足够（用户要的"粗实"是相对 Segoe UI 常规），
    // 固定字族后两个系统渲染一致。字号仍按系统 DPI 换算（×1.4）。
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    HFONT font = nullptr;
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        ncm.lfMessageFont.lfWeight = FW_NORMAL;
        // 字号固定像素（2026-11-21 用户实报：Win7 比 Win10 大很多——旧算法
        // 按系统消息字体 ×1.4，随机器显示缩放 DPI 漂移）。工具栏窗口本身就是
        // 固定像素（122x35），字号固定与像素几何自洽，两系统/任何缩放一致；
        // DPI 不感知进程在高 DPI 屏由系统虚拟化整体拉伸，等比不失配。
        // -16（2026-11-23 用户"还是略大，再小10%"：-18×0.9 取整）。演进：
        // -20（固定像素起点）→ -18（11-22 缩10%）→ -16（本次）。
        ncm.lfMessageFont.lfHeight = -16;
        // 微软雅黑常规体（2026-11-21 用户定稿"都用雅黑，效果一致"）：
        // Win7 的 SimHei 老版字形有缺陷（"英"草字头横线断裂），且两系统
        // SimHei 版本不同导致字形不一致；雅黑两系统矢量渲染一致且为小字
        // 号 ClearType 优化。运行时调用系统字体渲染 UI 不涉分发，合规。
        wcsncpy_s(ncm.lfMessageFont.lfFaceName, L"Microsoft YaHei",
                  _TRUNCATE);
        font = ::CreateFontIndirectW(&ncm.lfMessageFont);
    }
    if (font == nullptr) {
        font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }

    for (int i = 0; i < kBtnCount; ++i) {
        wchar_t label[8] = {};
        buttonLabel(i, label, _countof(label));
        paintButton(mem, font, buttonRect(i), label, i == g_hovered);
    }

    ::BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);

    if (font != static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT))) {
        ::DeleteObject(font);
    }
    ::SelectObject(mem, oldBmp);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::EndPaint(hwnd, &ps);
}

// 配置文件热重载：toolbar_enabled 改动后无需重启 daemon。
// 只读 mtime（GetFileAttributesExW 一次 stat），变了才真解析。
void checkConfigReload() {
    static FILETIME lastWrite = {};
    static bool firstRun = true;

    const std::wstring root = moqiAppDataDir();
    if (root.empty()) return;
    const std::wstring path = root + L"\\appearance_config.json";

    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return;  // 文件不存在：保持当前设置
    }
    if (!firstRun &&
        ::CompareFileTime(&fad.ftLastWriteTime, &lastWrite) == 0) {
        return;  // 没动过
    }
    lastWrite = fad.ftLastWriteTime;
    firstRun = false;

    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    Json::Value cfg;
    std::string errs;
    Json::CharReaderBuilder rb;
    if (!Json::parseFromStream(rb, in, &cfg, &errs) || !cfg.isObject()) return;
    if (!cfg["toolbar_enabled"].isBool()) return;

    const bool enabled = cfg["toolbar_enabled"].asBool();
    if (enabled == g_visibleWanted.load()) return;
    g_visibleWanted.store(enabled);
    daemonLogExtern(L"[toolbar] config reload: toolbar_enabled=" +
                    std::to_wstring(enabled ? 1 : 0));
    refreshToolbarVisibility();
    // 开关两个方向都要回调（2026-11-20 用户实报"toolbar_enabled 改成
    // false 后，工具栏和托盘都没了"）：只有 true 才回调的话，Win7 关掉
    // 工具栏后 main.cpp 的 startTrayFallbackIfWin7 永远不会被触发，
    // 整个系统再没有任何入口。回调由 main.cpp 注入——这里不直接 include
    // DaemonTray.h，避免两个 UI 模块互相依赖。
    if (g_callbacks.onEnabledChanged) {
        g_callbacks.onEnabledChanged(enabled);
    }
}

// 把窗口外形裁成圆角矩形。SetWindowRgn 接管 rgn 所有权，不能自己删。
void applyRoundedRegion(HWND hwnd) {
    RECT rc = {};
    ::GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1,
                                    kCornerRadius * 2, kCornerRadius * 2);
    if (rgn != nullptr) {
        ::SetWindowRgn(hwnd, rgn, TRUE);
    }
}

// 工具栏窗口尺寸（复位与拖动钳制共用同一公式，防两处漂移）。
int toolbarWidth() {
    return kBtnWidth * kBtnCount + kBtnGap * (kBtnCount - 1) + kPad * 2;
}

int toolbarHeight() { return kBtnHeight + kPad * 2; }

// 把窗口左上角钳制到工作区内（全窗可见）：不越屏幕边缘、不压任务栏。
// 拖动跟手与复位校正共用（2026-11-22 用户实报"拖动能拖出屏幕视线外"——
// 旧拖动路径直接跟鼠标，没有任何边界）。
void clampToWorkArea(int& x, int& y, int w, int h) {
    RECT work = {};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.right = ::GetSystemMetrics(SM_CXSCREEN);
        work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    }
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
}

void positionWindow(HWND hwnd) {
    const int w = toolbarWidth();
    const int h = toolbarHeight();
    // 用户拖过就留在用户放的地方（只做一次越界校正，防止拖到屏幕外后
    // 分辨率变化导致窗口彻底看不见）。
    if (g_hasUserPos) {
        int x = g_userPos.x;
        int y = g_userPos.y;
        clampToWorkArea(x, y, w, h);
        ::SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        return;
    }
    RECT work = {};
    if (!::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.right = ::GetSystemMetrics(SM_CXSCREEN);
        work.bottom = ::GetSystemMetrics(SM_CYSCREEN);
    }
    const int x = work.right - w - kEdgeGap;
    const int y = work.bottom - h - kEdgeGap;
    ::SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT:
            onPaint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            // 拖动 = 四向移动光标；其余一律显式箭头（2026-11-20 用户实报
            // 悬停出现"忙等待"光标——不能交给 DefWindowProc，类光标为 NULL
            // 时光标状态不被设置，行为未定义）。
            ::SetCursor(::LoadCursorW(
                nullptr, g_dragging ? IDC_SIZEALL : IDC_ARROW));
            return TRUE;
        case WM_MOUSEACTIVATE:
            // 点工具栏不抢焦点（否则输入框失焦、组字中断）。
            return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: {
            // 拖动 vs 点击（2026-11-19 用户实报"工具栏没法移动"）：
            // 第一版要求按在"按钮以外的空白"才能拖，但工具栏本来就只有
            // 几个像素的内边距，用户根本抓不到空白。改成**任意位置按下都
            // 先记下起点**，之后移动超过阈值才判定为拖动；没超过阈值就
            // 当点击处理（在 LBUTTONUP 里按 hitTest 决定点哪个按钮）。
            POINT anchor = {};
            ::GetCursorPos(&anchor);
            RECT wr = {};
            ::GetWindowRect(hwnd, &wr);
            g_dragOffset.x = anchor.x - wr.left;
            g_dragOffset.y = anchor.y - wr.top;
            g_pressPos = anchor;
            g_pressedButton = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            g_dragArmed = true;
            ::SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_dragArmed) {
                g_dragArmed = false;
                ::ReleaseCapture();
                if (g_dragging) {
                    g_dragging = false;
                    // 记住用户拖到的位置：之后显隐循环不再把它拉回默认角落。
                    RECT wr = {};
                    ::GetWindowRect(hwnd, &wr);
                    g_userPos = POINT{wr.left, wr.top};
                    g_hasUserPos = true;
                    // 松手立刻变回箭头：不等下一次 WM_SETCURSOR（松手后鼠标
                    // 不动的话四向箭头会残留，2026-11-20 用户实报"划过去就是
                    // 拖动光标"的同族问题）。
                    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                    return 0;
                }
            }
            const int idx = g_pressedButton;
            daemonLogExtern(L"[toolbar] click idx=" + std::to_wstring(idx));
            switch (idx) {
                case 0:
                    showFunctionMenu(hwnd);
                    break;
                case 1:
                    executeLang();
                    break;
                case 2:
                    executePunct();
                    break;
                case 3:
                    if (g_callbacks.executeCommand) {
                        g_callbacks.executeCommand(kCmdFullShape);
                    }
                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        g_state.fullShape = !g_state.fullShape;
                    }
                    break;
                default:
                    break;
            }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_dragArmed && !g_dragging) {
                POINT cur = {};
                ::GetCursorPos(&cur);
                // 4px 阈值：低于这个算手抖，仍然当作点击。
                if (abs(cur.x - g_pressPos.x) > 4 ||
                    abs(cur.y - g_pressPos.y) > 4) {
                    g_dragging = true;
                    // 进入拖动：光标变"移动"光标。用 IDC_SIZEALL（四向箭头），
                    // 不是 IDC_CROSS —— 后者是精确选取用的细十字线，不做拖动。
                    // 用户说的"大概是个十字"指的就是这个四向箭头（2026-11-19）。
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                }
            }
            if (g_dragging) {
                POINT cur = {};
                ::GetCursorPos(&cur);
                int x = cur.x - g_dragOffset.x;
                int y = cur.y - g_dragOffset.y;
                // 拖动全程钳制在工作区内：鼠标可以出去，窗口始终全窗可见。
                clampToWorkArea(x, y, toolbarWidth(), toolbarHeight());
                ::SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                               SWP_NOSIZE | SWP_NOACTIVATE);
                return 0;
            }
            const int idx = hitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
            if (idx != g_hovered) {
                g_hovered = idx;
                ::InvalidateRect(hwnd, nullptr, FALSE);
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                ::TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g_hovered != -1) {
                g_hovered = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_TIMER:
            if (wp == kRefreshTimerId) {
                // 配置文件热重载（2026-11-19 用户实报"改了配置不管用"）：
                // toolbar_enabled 原来只在 daemon 启动时读一次，改完必须
                // 重启才生效。这里比对 mtime，变了就重读——切到工具栏/关掉
                // 工具栏都不用重启，Win10 上开 toolbar_enabled 也能立即出现。
                // （纯 stat + 偶尔一次小 JSON 解析，留在界面线程没问题。）
                checkConfigReload();
            }
            return 0;
        case kMsgRefresh:
            // 后台轮询线程算好的结果：这里只做窗口操作与重绘，绝不碰引擎。
            // 显隐的权威判定：前台窗口的进程是否由我们服务。
            // 切换输入法时 TSF 未必调 Deactivate（不同宿主行为不一），
            // 但前台窗口一定换人——这比等失活通知可靠得多
            //（2026-11-19 用户两轮实报"切走工具栏不消失"）。
            {
                const bool fgOurs = g_fgOurs.load();
                if (g_visibleWanted.load() && !fgOurs) {
                    ::ShowWindow(hwnd, SW_HIDE);
                } else if (g_visibleWanted.load() && fgOurs) {
                    if (!::IsWindowVisible(hwnd)) {
                        positionWindow(hwnd);
                        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                    }
                }
                if (g_stateDirty.exchange(false)) {
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case WM_SIZE:
            applyRoundedRegion(hwnd);
            return 0;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::KillTimer(hwnd, kRefreshTimerId);
            ::PostQuitMessage(0);
            return 0;
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void threadProc() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    // 类光标必须给：NULL 时悬停光标不归我们管，用户实报"鼠标划上去是
    // 忙等待光标"（窗口线程实测 1ms 响应、可点击——光标异常是类光标缺失，
    // 不是卡死）。
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // 类背景刷 = 工具栏底色：窗口擦除时也铺底色，而不是默认的黑。刷子随
    // 进程存活，不删（类还引用它）。
    static HBRUSH kClassBg = ::CreateSolidBrush(kBg);
    wc.hbrBackground = kClassBg;
    if (::RegisterClassExW(&wc) == 0) {
        daemonLogExtern(L"[toolbar] RegisterClassEx failed gle=" +
                        std::to_wstring(::GetLastError()));
        g_running.store(false);
        return;
    }

    // WS_EX_TOOLWINDOW + NOACTIVATE：不进 Alt-Tab、不抢焦点、始终置顶。
    HWND hwnd = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClassName,
        L"\u58A8\u5947\u5DE5\u5177\u680F", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0,
        0, nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        daemonLogExtern(L"[toolbar] CreateWindowEx failed gle=" +
                        std::to_wstring(::GetLastError()));
        g_running.store(false);
        return;
    }
    g_hwnd.store(hwnd);

    // 窗口外形裁成圆角：否则四角会露出未被绘制覆盖的直角区域
    //（WS_POPUP 默认矩形）。尺寸变化时在 WM_SIZE 里重设。
    applyRoundedRegion(hwnd);
    positionWindow(hwnd);
    // 初始状态不再从引擎同步读（2026-11-21 移除）：readState 要拿
    // g_dispatchLock，daemon 冷启动部署期间（可达数十秒）会把窗口线程冻在
    // 这里。默认态（中/。/半）够用——引擎就绪后第一个按键回包经
    // updateToolbarState 推送真实值。
    // 初始按"是否已有激活客户端"决定：daemon 启动时通常没有，先隐藏；
    // 一旦 DLL 发 onActivate 就会显示出来。
    ::ShowWindow(hwnd, SW_HIDE);
    ::SetTimer(hwnd, kRefreshTimerId, kRefreshIntervalMs, nullptr);

    // 后台轮询线程：只判前台归属，**不读引擎**（见 DaemonToolbar.h
// updateToolbarState 的说明）。250ms 一次；g_running 变 false 时退出。
    std::thread([]() {
        while (g_running.load()) {
            bool fgOurs = true;
            if (g_callbacks.isForegroundServed) {
                fgOurs = g_callbacks.isForegroundServed();
            }
            g_fgOurs.store(fgOurs, std::memory_order_relaxed);
            HWND hwnd = g_hwnd.load();
            if (hwnd != nullptr) ::PostMessageW(hwnd, kMsgRefresh, 0, 0);
            // 分片睡眠：退出时不必等满 250ms。
            for (int i = 0; i < 5 && g_running.load(); ++i) {
                ::Sleep(kRefreshIntervalMs / 5);
            }
        }
    }).detach();

    refreshToolbarVisibility();
    daemonLogExtern(L"[toolbar] created hwnd=" +
                    std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)) +
                    L" enabled=" + (g_visibleWanted.load() ? L"1" : L"0"));

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    g_hwnd.store(nullptr);
    g_running.store(false);
}

}  // namespace

void startToolbar(ToolbarCallbacks callbacks) {
    if (g_running.exchange(true)) return;
    g_callbacks = std::move(callbacks);
    std::thread([]() { threadProc(); }).detach();
}

void setToolbarEnabled(bool enabled) {
    g_visibleWanted.store(enabled);
    refreshToolbarVisibility();
}

void stopToolbar() {
    HWND hwnd = g_hwnd.load();
    if (hwnd != nullptr) {
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

void setToolbarActiveCount(int count) {
    if (count < 0) count = 0;
    g_activeClients.store(count);
    refreshToolbarVisibility();
}

void refreshToolbarVisibility() {
    HWND hwnd = g_hwnd.load();
    if (hwnd == nullptr) return;
    // 显示条件：配置允许 && 至少一个客户端处于激活态 && 前台是我们的宿主。
    // 最后一个条件是权威判定（见 WM_TIMER 里的说明）：TSF 不保证切走时发
    // Deactivate，前台窗口换人才是必然发生的。
    bool fgOurs = true;
    if (g_callbacks.isForegroundServed) {
        fgOurs = g_callbacks.isForegroundServed();
    }
    const bool show = g_visibleWanted.load() && g_activeClients.load() > 0 &&
                      fgOurs;
    if (show) {
        positionWindow(hwnd);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::InvalidateRect(hwnd, nullptr, FALSE);
    } else {
        ::ShowWindow(hwnd, SW_HIDE);
    }
}

void pokeToolbarState() {
    HWND hwnd = g_hwnd.load();
    if (hwnd == nullptr) return;
    // 只投个刷新消息：真正的引擎读取在后台轮询线程里做。调用方可能是
    // 分发线程（命令响应路径），这里再读一次 rime 就又把锁带进来了。
    ::PostMessageW(hwnd, kMsgRefresh, 0, 0);
}

void updateToolbarState(bool ascii, bool fullShape, bool asciiPunct) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (g_state.ascii != ascii || g_state.fullShape != fullShape ||
            g_state.asciiPunct != asciiPunct) {
            g_state.ascii = ascii;
            g_state.fullShape = fullShape;
            g_state.asciiPunct = asciiPunct;
            changed = true;
        }
    }
    if (!changed) return;
    g_stateDirty.store(true);
    HWND hwnd = g_hwnd.load();
    if (hwnd != nullptr) ::PostMessageW(hwnd, kMsgRefresh, 0, 0);
}

}  // namespace Moqi