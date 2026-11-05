// DigitPassthrough.h —— 数字/句点原生直通（bug#20）的纯逻辑判定。
// 单独成头（无 rime/TSF 依赖）是为了可单测：tests/digit_passthrough_test.cpp
// 锁定行为。引擎侧 schemaUsesDigitInput() 调用这里。
#ifndef MOQI_DAEMON_DIGIT_PASSTHROUGH_H_
#define MOQI_DAEMON_DIGIT_PASSTHROUGH_H_

#include <algorithm>
#include <cctype>
#include <string>

namespace Moqi {

// 方案 id 是否属于 T9 九宫格类（id 含 "t9"，大小写不敏感）。
// 与 moqi_plain_number.lua 的 sid:lower():find("t9") 同口径。
inline bool schemaIdLooksT9(const std::string& schemaId) {
  std::string lower = schemaId;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return lower.find("t9") != std::string::npos;
}

// 方案 YAML 原文里 speller/alphabet 是否含数字（数字即输入的方案，
// 如 T9 九宫格 alphabet: 23456789）。行首顶格的 key 不算（子节点才缩进）。
// 注意：alphabet 值后同行注释里的数字也会命中——方向保守（禁用直通），
// 宁可少直通不可误直通。
inline bool schemaYamlUsesDigitAlphabet(const std::string& yamlText) {
  bool inSpeller = false;
  size_t pos = 0;
  while (pos <= yamlText.size()) {
    const size_t eol = yamlText.find('\n', pos);
    const std::string line =
        yamlText.substr(pos, eol == std::string::npos ? std::string::npos
                                                      : eol - pos);
    if (line.rfind("speller:", 0) == 0) {
      inSpeller = true;
    } else if (!line.empty() && line[0] != ' ' && line[0] != '\t') {
      inSpeller = false;
    }
    if (inSpeller) {
      const size_t key = line.find("alphabet:");
      if (key != std::string::npos) {
        for (size_t i = key + 9; i < line.size(); ++i) {
          if (line[i] >= '0' && line[i] <= '9') {
            return true;
          }
        }
      }
    }
    if (eol == std::string::npos) break;
    pos = eol + 1;
  }
  return false;
}

}  // namespace Moqi

#endif  // MOQI_DAEMON_DIGIT_PASSTHROUGH_H_