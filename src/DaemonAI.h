#pragma once
// 阶段5c：AI 评审移植（Go rime.go 的 AI 分支 + ai_client.go + ai_config.go
// + imecore/aikey 的 C++ 版）。语义对齐 Go：热键触发 → 后台 HTTP →
// 候选覆盖到候选窗顶部 → 数字/空格/回车选中即上屏。
//
// 配置：%APPDATA%\Moqi\ai_config.json（缺省回落到安装目录 input_methods\
// rime\ai_config.json）。「base_url<后缀>」这种畸变键名是内置的"AI 默认关"
// 哨兵：只有键名恰好是 "base_url" 时才启用。
#include <string>
#include <vector>

namespace Moqi {

struct AIAction {
  std::string name;
  std::string hotkey;
  std::string prompt;
  unsigned keyCode = 0;
  bool ctrl = false;
  bool alt = false;
  bool shift = false;
};

struct AIConfig {
  bool enabled = false;
  std::string baseUrl;
  std::string apiKey;
  std::string model;
  std::vector<AIAction> actions;
};

class DaemonAI {
public:
  // 读配置（每次热键前刷新，用户改文件即生效）。返回是否启用。
  bool load();
  bool enabled() const { return cfg_.enabled; }
  const std::vector<AIAction> &actions() const { return cfg_.actions; }

  // 热键精确匹配（Ctrl/Alt/Shift 组合 + 主键）。
  const AIAction *match(unsigned keyCode, bool ctrl, bool alt, bool shift) const;

  // 同步 HTTP 生成（调用方放后台线程）。成功时 out 为候选列表。
  bool generate(const AIAction &action, const std::string &composition,
                const std::vector<std::string> &candidates,
                const std::string &previousCommit,
                std::vector<std::string> *out, std::string *err);

private:
  AIConfig cfg_;
};

// aikey：MQ- 前缀混淆 key 的还原（base64 + XOR + sha256 前 4 字节校验）。
// 非混淆格式（用户自己的明文 key）原样返回。
std::string ResolveAIKey(const std::string &raw);

} // namespace Moqi