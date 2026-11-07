// Lite 版菜单裁剪契约测试（仅 MOQI_LITE 构建参与编译）。
// 定稿清单见 57-交接-2026-11-06：只保留四个基础开关 + 输入方案 + 重新部署
// 方案 + 输入状态固定 + 外观 + 输入设置（无 AI 项）+ 打开文件夹 + 参与讨论。
// 开关裁剪保留原始索引（ascii_punct 是第 8 个开关 → id 仍 2007），故这里
// 同时锁 id 序与 handleCommand 的索引映射。
#include "DaemonMenuCore.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("  PASS %s\n", what);
  } else {
    ++g_fail;
    std::printf("> FAIL %s\n", what);
  }
}

}  // namespace

int main() {
  using namespace Moqi::Menu;

  MenuState st;
  // 与全量金样同序的九开关（扩展项的 option 名以真实引擎为准，这里用
  // 代表性名字——裁剪只看白名单，与扩展项名字无关）。
  const char* names[] = {"ascii_mode",   "full_shape",   "simplification",
                         "translation",  "emoji",        "chaizi",
                         "rime_ext7",    "ascii_punct",  "fix_top"};
  // 前四项用真实状态文案（断言用）；扩展项文案任意（裁剪后不出现）。
  const char* st0[] = {"中文", "半角", "简体", "翻译", "💀", "拆分不提示", "地", "。，", "固顶关"};
  const char* st1[] = {"英文", "全角", "繁体", "翻译", "😄", "拆分提示",   "煋", "．，", "固顶开"};
  for (size_t k = 0; k < 9; ++k) {
    MenuSwitchDef sw;
    sw.name = names[k];
    sw.states = {st0[k], st1[k]};
    st.switches.push_back(sw);
    st.options[names[k]] = false;
  }
  st.schemas.push_back({"rime_frost", "白霜拼音", true});
  st.schemas.push_back({"rime_frost_t9", "中文九键", false});
  st.schemas.push_back({"rime_frost_flypy", "白霜小鹤双拼", false});
  st.themes.push_back({"default", "默认主题"});

  Json::Value menu = buildMenu(st);

  // 1) 顶层形状：四开关、分隔线、输入方案组、重新部署、分隔线、
  // 输入状态固定、外观组、输入设置组、打开文件夹组、分隔线、参与讨论。
  const char* wantText[] = {"中文 → 英文",  "半角 → 全角", "简体 → 繁体",
                            "。， → ．，",  "",            "输入方案(&I)",
                            "重新部署方案(&R)", "",        "输入状态固定",
                            "外观(&A)",     "输入设置",    "打开文件夹(&O)",
                            "",             "参与讨论"};
  const int wantId[] = {2000, 2001, 2002, 2007, 0, 0, 10, 0, 210, 0, 0, 0, 0, 22};
  const bool wantSep[] = {false, false, false, false, true,  false, false, true,
                          false, false, false, false, true,  false};
  bool shapeOk = menu.size() == 14;
  if (shapeOk) {
    for (unsigned i = 0; i < menu.size(); ++i) {
      if (menu[i]["id"].asInt() != wantId[i] ||
          menu[i].get("text", "").asString() != wantText[i] ||
          menu[i].get("separator", false).asBool() != wantSep[i]) {
        shapeOk = false;
        std::printf("  top[%u] got id=%d text=%s\n", i,
                    menu[i]["id"].asInt(),
                    menu[i].get("text", "").asString().c_str());
      }
    }
  }
  check(shapeOk, "lite: top-level shape = 定稿清单（4 开关+方案+部署+状态+外观+设置+文件夹+讨论）");

  // 2) 被裁项全树零出现（2026-11-07 追加：导入皮肤/中文九键/同步文件夹）。
  const char* banned[] = {"打开置顶短语", "打开超级简拼", "在线更新方案",
                          "同步设置",    "WebDAV",       "云剪贴板",
                          "同步用户词库", "维护",         "开启调试",
                          "重启引擎",    "退出引擎",     "帮助文档",
                          "配置AI助手",  "导入皮肤",     "中文九键",
                          "同步文件夹",  "共享文件夹"};
  std::string flat = menu.toStyledString();
  bool noneBanned = true;
  for (const char* b : banned) {
    if (flat.find(b) != std::string::npos) {
      noneBanned = false;
      std::printf("  banned text present: %s\n", b);
    }
  }
  check(noneBanned, "lite: 裁剪项（AI/云/WebDAV/同步/维护/帮助/短语/简拼/在线更新）零出现");

  // 3) 输入设置组 = 三项（成对开关/设置入口/分号次选），无 AI。
  bool inputOk = false;
  for (const auto& top : menu) {
    if (top.get("text", "").asString() == "输入设置") {
      inputOk = top["submenu"].size() == 3;
    }
  }
  check(inputOk, "lite: 输入设置组恰好三项（无 AI 配置项）");

  // 4) 开关命令的原始索引映射：2007 → ascii_punct（未因裁剪重排）。
  MenuState s2 = st;
  CommandResult r = handleCommand(2007, s2);
  check(r.action == CmdAction::Local && r.optionToggled == "ascii_punct",
        "lite: handleCommand(2007) 翻转 ascii_punct（原始索引保持）");

  // 5) 方案过滤同法：T9 跳过后小鹤双拼仍为原始索引 id 1002。
  bool schemaOk = false;
  for (const auto& top : menu) {
    if (top.get("text", "").asString() == "输入方案(&I)") {
      schemaOk = top["submenu"].size() == 2 &&
                 top["submenu"][1]["id"].asInt() == 1002 &&
                 top["submenu"][1].get("text", "").asString() == "白霜小鹤双拼";
    }
  }
  check(schemaOk, "lite: 方案组=2 项，小鹤双拼 id=1002（T9 跳过不重排）");
  MenuState s3 = st;
  CommandResult r3 = handleCommand(1002, s3);
  check(r3.action == CmdAction::Local && r3.schemaSelected == "rime_frost_flypy",
        "lite: handleCommand(1002) 选中 rime_frost_flypy（原始索引保持）");

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
