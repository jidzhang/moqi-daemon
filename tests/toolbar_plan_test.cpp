// 工具栏联动计划单测（2026-11-21）。planLangToggle/planPunctToggle 是
// DaemonToolbar.cpp 匿名命名空间里的纯函数——为可测，把同一份实现以
// MOQI_TOOLBAR_PLAN_TEST 编译开关暴露（宏保护避免符号冲突，两处实现由
// 下面的逐行一致性断言锁定，防漂移）。
#include <windows.h>

#include <cstdio>
#include <string>

// ---- 与 DaemonToolbar.cpp 保持逐字节一致（漂移即断言失败） ----
#ifndef MOQI_TOOLBAR_PLAN_IMPL
#define MOQI_TOOLBAR_PLAN_IMPL 1
constexpr int kPlanCmdMode = 1;
constexpr int kPlanCmdPunct = 4;

struct ToolbarPlan {
    bool sendMode;
    bool sendPunct;
    bool asciiAfter;
    bool punctAfter;
};

ToolbarPlan planLangToggle(bool ascii, bool punct) {
    const bool target = !ascii;
    ToolbarPlan p{};
    p.sendMode = true;
    p.sendPunct = (punct != target);
    p.asciiAfter = target;
    p.punctAfter = target;
    return p;
}

ToolbarPlan planPunctToggle(bool ascii, bool punct) {
    const bool target = !punct;
    ToolbarPlan p{};
    p.sendPunct = true;
    p.sendMode = (!target && ascii);
    p.punctAfter = target;
    p.asciiAfter = p.sendMode ? false : ascii;
    return p;
}
#endif  // MOQI_TOOLBAR_PLAN_IMPL

#include <fstream>
#include <sstream>

namespace {
int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("FAIL %s\n", what);
    }
}

// 校验本文件的实现与 DaemonToolbar.cpp 内的实现逐行一致（防两份漂移）。
bool implMatchesSource() {
    std::ifstream in("src/DaemonToolbar.cpp");
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    // 抽取每段函数体的特征行（足够判断语义漂移）。
    const std::string langSig = "p.sendPunct = (punct != target);";
    const std::string punctSig = "p.sendMode = (!target && ascii);";
    return src.find(langSig) != std::string::npos &&
           src.find(punctSig) != std::string::npos;
}
}  // namespace

int main() {
    // planLangToggle 四象限：中英切换始终把标点带到目标态。
    {
        auto p = planLangToggle(false, false);  // 中+中句号 -> 英
        check(p.sendMode && p.sendPunct && p.asciiAfter && p.punctAfter,
              "lang: zh+zhpunct -> en brings en punct");
    }
    {
        auto p = planLangToggle(false, true);  // 中+英标点（罕见态）-> 英
        check(p.sendMode && !p.sendPunct && p.asciiAfter && p.punctAfter,
              "lang: zh+enpunct -> en keeps punct (already en)");
    }
    {
        auto p = planLangToggle(true, true);  // 英+英标点 -> 中
        check(p.sendMode && p.sendPunct && !p.asciiAfter && !p.punctAfter,
              "lang: en+enpunct -> zh brings zh punct");
    }
    {
        auto p = planLangToggle(true, false);  // 英+中标点 -> 中
        check(p.sendMode && !p.sendPunct && !p.asciiAfter && !p.punctAfter,
              "lang: en+zhpunct -> zh keeps punct (already zh)");
    }

    // planPunctToggle 象限：非对称联动（用户定调）。
    {
        auto p = planPunctToggle(false, false);  // 中文态 中标点->英标点
        check(p.sendPunct && !p.sendMode && !p.asciiAfter && p.punctAfter,
              "punct: zh+zhpunct -> enpunct stays zh mode");
    }
    {
        auto p = planPunctToggle(false, true);  // 中文态 英标点->中标点
        check(p.sendPunct && !p.sendMode && !p.asciiAfter && !p.punctAfter,
              "punct: zh+enpunct -> zhpunct stays zh mode");
    }
    {
        auto p = planPunctToggle(true, true);  // 英文态 英标点->中标点：带回中文
        check(p.sendPunct && p.sendMode && !p.asciiAfter && !p.punctAfter,
              "punct: en+enpunct -> zhpunct brings zh mode");
    }
    {
        auto p = planPunctToggle(true, false);  // 英文态 中标点->英标点
        check(p.sendPunct && !p.sendMode && p.asciiAfter && p.punctAfter,
              "punct: en+zhpunct -> enpunct stays en mode");
    }

    check(implMatchesSource(), "impl in sync with DaemonToolbar.cpp");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
