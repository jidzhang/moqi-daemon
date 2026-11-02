// 阶段5c：AI 移植实现（语义见 DaemonAI.h 与 Go 侧 ai_client.go/ai_config.go）。
// include 纪律：RimeCore.h 链开篇（jsoncpp/裸 STL 开篇会让 MSVC STL 在
// <cmath> 崩，见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

#include "DaemonAI.h"
#include "ProtoFraming.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace Moqi {
namespace {

// ---------------------------------------------------------------- aikey ---

const char kAIKeyPrefix[] = "MQ-";
const size_t kAIKeyTagLen = 4;
const unsigned char kAIKeyMask[] = {'M', 'o', 'q', 'i', 'A', 'I', '2', '0', '2', '6'};

void xorMask(std::string *data) {
  for (size_t i = 0; i < data->size(); ++i) {
    (*data)[i] = static_cast<char>((*data)[i] ^ kAIKeyMask[i % sizeof(kAIKeyMask)]);
  }
}

// SHA-256 前 4 字节（CryptoAPI；避免引入 openssl）。
bool sha256Tag4(const std::string &data, unsigned char out[4]) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  bool ok = false;
  if (::CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
    if (::CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
      if (::CryptHashData(hash, reinterpret_cast<const BYTE *>(data.data()),
                          static_cast<DWORD>(data.size()), 0)) {
        BYTE digest[32] = {};
        DWORD len = sizeof(digest);
        if (::CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0) &&
            len >= 4) {
          std::copy(digest, digest + 4, out);
          ok = true;
        }
      }
      ::CryptDestroyHash(hash);
    }
    ::CryptReleaseContext(prov, 0);
  }
  return ok;
}

bool base64Decode(const std::string &in, std::string *out) {
  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int rev[256];
  std::fill(std::begin(rev), std::end(rev), -1);
  for (int i = 0; i < 64; ++i) {
    rev[static_cast<unsigned char>(kTable[i])] = i;
  }
  out->clear();
  int buf = 0;
  int bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = rev[static_cast<unsigned char>(c)];
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<char>((buf >> bits) & 0xFF));
    }
  }
  return true;
}

std::string trimCopy(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string toLowerCopy(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::wstring utf8ToWs(const std::string &s) {
  if (s.empty()) return std::wstring();
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0);
  std::wstring out(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        &out[0], n);
  return out;
}

std::string wsToUtf8(const std::wstring &ws) {
  if (ws.empty()) return std::string();
  int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                                nullptr, 0, nullptr, nullptr);
  std::string out(n, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()),
                        &out[0], n, nullptr, nullptr);
  return out;
}

// ------------------------------------------------------- hotkey parsing ---

// "Ctrl+Shift+G" -> keyCode/flags。主键只接受单字母或 F1-F12。
bool parseHotkey(const std::string &hotkey, AIAction *action) {
  std::vector<std::string> parts;
  std::string cur;
  for (char c : hotkey) {
    if (c == '+') {
      parts.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  parts.push_back(cur);
  if (parts.empty()) return false;

  bool ctrl = false, alt = false, shift = false;
  std::string mainKey;
  for (const auto &raw : parts) {
    const std::string p = toLowerCopy(trimCopy(raw));
    if (p.empty()) continue;
    if (p == "ctrl" || p == "control") {
      ctrl = true;
    } else if (p == "alt") {
      alt = true;
    } else if (p == "shift") {
      shift = true;
    } else {
      mainKey = p;
    }
  }
  if (mainKey.empty()) return false;
  unsigned keyCode = 0;
  if (mainKey.size() == 1) {
    const char c = mainKey[0];
    if (c >= 'a' && c <= 'z') {
      keyCode = static_cast<unsigned>(c - 'a' + 'A'); // VK 大写字母
    } else if (c >= '0' && c <= '9') {
      keyCode = static_cast<unsigned>(c);
    } else {
      return false;
    }
  } else if (mainKey.size() <= 3 && mainKey[0] == 'f') {
    const int n = std::atoi(mainKey.c_str() + 1);
    if (n < 1 || n > 12) return false;
    keyCode = static_cast<unsigned>(VK_F1 + n - 1);
  } else {
    return false;
  }
  action->keyCode = keyCode;
  action->ctrl = ctrl;
  action->alt = alt;
  action->shift = shift;
  return true;
}

// --------------------------------------------------------------- config ---

std::string aiConfigPathUser() {
  const wchar_t *appData = _wgetenv(L"APPDATA");
  if (!appData || !*appData) return "";
  std::wstring path(appData);
  path += L"\\Moqi\\ai_config.json";
  return wsToUtf8(path);
}

std::string aiConfigPathBundled(const std::wstring &programDir) {
  if (programDir.empty()) return "";
  return wsToUtf8(programDir + L"\\moqi-ime\\input_methods\\rime\\ai_config.json");
}

bool readJsonFile(const std::string &utf8Path, Json::Value *out) {
  if (utf8Path.empty()) return false;
  std::ifstream in(utf8ToWs(utf8Path), std::ios::binary);
  if (!in) return false;
  Json::CharReaderBuilder builder;
  std::string errs;
  return Json::parseFromStream(builder, in, out, &errs);
}

// ---------------------------------------------------------- prompt 填充 ---

void replaceAll(std::string *s, const std::string &from, const std::string &to) {
  if (from.empty()) return;
  size_t pos = 0;
  while ((pos = s->find(from, pos)) != std::string::npos) {
    s->replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string candidatesTop3Text(const std::vector<std::string> &cands) {
  std::string out;
  for (size_t i = 0; i < cands.size() && i < 3; ++i) {
    if (!out.empty()) out += "\n";
    out += std::to_string(i + 1) + ". " + cands[i];
  }
  return out;
}

std::string buildUserPrompt(const std::string &promptTemplate,
                            const std::string &composition,
                            const std::vector<std::string> &candidates,
                            const std::string &previousCommit) {
  std::string prompt = promptTemplate;
  replaceAll(&prompt, "{{composition}}", composition);
  replaceAll(&prompt, "{{previous_commit}}", previousCommit);
  replaceAll(&prompt, "{{candidate_1}}", candidates.empty() ? "" : candidates[0]);
  replaceAll(&prompt, "{{candidates_top3}}", candidatesTop3Text(candidates));
  std::string context = composition;
  if (!candidates.empty()) {
    if (!context.empty()) context += "  ";
    context += candidatesTop3Text(candidates);
  }
  replaceAll(&prompt, "{{context}}", context);
  return prompt;
}

// 结果解析：按行切、去编号/引号/空行（对齐 Go parseReviewCandidates）。
std::vector<std::string> parseReviewCandidates(const std::string &content) {
  std::vector<std::string> out;
  std::istringstream ss(content);
  std::string line;
  while (std::getline(ss, line)) {
    std::string s = trimCopy(line);
    if (s.empty()) continue;
    // 去 "1. " / "1) " / "- " / "* " 前缀
    size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i > 0 && i < s.size() && (s[i] == '.' || s[i] == ')' || s[i] == '、')) {
      s = trimCopy(s.substr(i + 1));
    } else if (!s.empty() && (s[0] == '-' || s[0] == '*')) {
      s = trimCopy(s.substr(1));
    }
    // 去引号
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                          (s.front() == '\xE2' && s.back() == '"'))) {
      s = s.substr(1, s.size() - 2);
    }
    if (!s.empty()) out.push_back(s);
    if (out.size() >= 9) break; // 与候选窗上限一致
  }
  return out;
}

// ------------------------------------------------------------- WinHTTP ---

// 极简 POST（同步；调用方在后台线程）。返回 body；失败置 err。
bool httpPostJson(const std::string &url, const std::string &apiKey,
                  const std::string &body, std::string *responseBody,
                  std::string *err) {
  const std::wstring wurl = utf8ToWs(url);
  URL_COMPONENTS comps = {};
  comps.dwStructSize = sizeof(comps);
  comps.dwHostNameLength = static_cast<DWORD>(-1);
  comps.dwUrlPathLength = static_cast<DWORD>(-1);
  comps.dwExtraInfoLength = static_cast<DWORD>(-1);
  comps.dwSchemeLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0,
                         &comps)) {
    *err = "invalid url";
    return false;
  }
  const std::wstring host(comps.lpszHostName, comps.dwHostNameLength);
  std::wstring path(comps.lpszUrlPath, comps.dwUrlPathLength);
  if (comps.dwExtraInfoLength > 0) {
    path.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);
  }
  const bool https = comps.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = ::WinHttpOpen(
      L"MoqiIME/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (session == nullptr) {
    *err = "WinHttpOpen failed";
    return false;
  }
  ::WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);
  HINTERNET connect =
      ::WinHttpConnect(session, host.c_str(), comps.nPort, 0);
  if (connect == nullptr) {
    ::WinHttpCloseHandle(session);
    *err = "WinHttpConnect failed";
    return false;
  }
  HINTERNET request = ::WinHttpOpenRequest(
      connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
  if (request == nullptr) {
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    *err = "WinHttpOpenRequest failed";
    return false;
  }
  const std::wstring headers =
      L"Content-Type: application/json\r\nAuthorization: Bearer " +
      utf8ToWs(apiKey) + L"\r\n";
  BOOL ok = ::WinHttpSendRequest(
      request, headers.c_str(), static_cast<DWORD>(-1),
      const_cast<char *>(body.data()), static_cast<DWORD>(body.size()),
      static_cast<DWORD>(body.size()), 0);
  if (ok) {
    ok = ::WinHttpReceiveResponse(request, nullptr);
  }
  if (ok) {
    DWORD status = 0;
    DWORD len = sizeof(status);
    ::WinHttpQueryHeaders(request,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                          WINHTTP_NO_HEADER_INDEX);
    std::string acc;
    DWORD avail = 0;
    do {
      avail = 0;
      if (!::WinHttpQueryDataAvailable(request, &avail)) break;
      if (avail == 0) break;
      std::string chunk(avail, 0);
      DWORD read = 0;
      if (!::WinHttpReadData(request, &chunk[0], avail, &read)) break;
      acc.append(chunk.data(), read);
    } while (avail > 0);
    if (status >= 200 && status < 300) {
      *responseBody = acc;
      ok = true;
    } else {
      *err = "http " + std::to_string(status) + ": " + acc.substr(0, 200);
      ok = false;
    }
  } else {
    *err = "request failed err=" + std::to_string(::GetLastError());
  }
  ::WinHttpCloseHandle(request);
  ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return ok != 0;
}

} // namespace

std::string ResolveAIKey(const std::string &raw) {
  const size_t prefixLen = sizeof(kAIKeyPrefix) - 1;
  if (raw.compare(0, prefixLen, kAIKeyPrefix) != 0) {
    return raw; // 用户明文 key 原样返回
  }
  std::string blob;
  if (!base64Decode(raw.substr(prefixLen), &blob)) return raw;
  if (blob.size() <= kAIKeyTagLen) return raw;
  std::string plain = blob.substr(0, blob.size() - kAIKeyTagLen);
  xorMask(&plain);
  unsigned char tag[4] = {};
  if (!sha256Tag4(plain, tag)) return raw;
  if (memcmp(tag, blob.data() + plain.size(), 4) != 0) return raw;
  return plain;
}

bool DaemonAI::load() {
  cfg_ = AIConfig();
  Json::Value root;
  // 用户配置优先，缺失回落到随包配置。
  if (!readJsonFile(aiConfigPathUser(), &root)) {
    const wchar_t *program = _wgetenv(L"MOQI_PROGRAM_DIR");
    std::wstring programDir;
    if (program && *program) {
      programDir = program;
    } else {
      wchar_t exePath[MAX_PATH] = {};
      if (::GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
        std::wstring full(exePath);
        const size_t slash = full.find_last_of(L'\\');
        if (slash != std::wstring::npos) programDir = full.substr(0, slash);
      }
    }
    if (!readJsonFile(aiConfigPathBundled(programDir), &root)) {
      return false;
    }
  }
  const Json::Value &api = root["api"];
  if (!api.isObject()) return false;
  // 哨兵：只有键名恰好是 base_url 才启用（内置的"AI 默认关"机制）。
  const Json::Value &baseVal = api["base_url"];
  if (!baseVal.isString()) return false;
  std::string baseUrl = trimCopy(baseVal.asString());
  if (baseUrl.empty()) return false;
  cfg_.baseUrl = baseUrl;
  cfg_.apiKey = ResolveAIKey(trimCopy(api.get("api_key", "").asString()));
  cfg_.model = trimCopy(api.get("model", "").asString());
  if (cfg_.apiKey.empty() || cfg_.model.empty()) return false;

  const Json::Value &actions = root["actions"];
  if (actions.isArray()) {
    for (const auto &item : actions) {
      AIAction action;
      action.name = item.get("name", "").asString();
      action.hotkey = item.get("hotkey", "").asString();
      action.prompt = item.get("prompt", "").asString();
      if (action.prompt.empty() || action.hotkey.empty()) continue;
      if (!parseHotkey(action.hotkey, &action)) continue;
      cfg_.actions.push_back(action);
    }
  }
  cfg_.enabled = !cfg_.actions.empty();
  return cfg_.enabled;
}

const AIAction *DaemonAI::match(unsigned keyCode, bool ctrl, bool alt,
                               bool shift) const {
  for (const auto &action : cfg_.actions) {
    if (action.keyCode == keyCode && action.ctrl == ctrl &&
        action.alt == alt && action.shift == shift) {
      return &action;
    }
  }
  return nullptr;
}

bool DaemonAI::generate(const AIAction &action, const std::string &composition,
                        const std::vector<std::string> &candidates,
                        const std::string &previousCommit,
                        std::vector<std::string> *out, std::string *err) {
  if (!cfg_.enabled) {
    *err = "ai disabled";
    return false;
  }
  Json::Value body(Json::objectValue);
  body["model"] = cfg_.model;
  Json::Value messages(Json::arrayValue);
  Json::Value systemMsg(Json::objectValue);
  systemMsg["role"] = "system";
  systemMsg["content"] =
      "你是中文输入法的文字助手。只输出结果本身，不要解释、不要引号、不要编号。";
  messages.append(systemMsg);
  Json::Value userMsg(Json::objectValue);
  userMsg["role"] = "user";
  userMsg["content"] =
      buildUserPrompt(action.prompt, composition, candidates, previousCommit);
  messages.append(userMsg);
  body["messages"] = messages;
  body["temperature"] = 0.7;

  Json::StreamWriterBuilder writer;
  writer["commentStyle"] = "None";
  writer["indentation"] = "";
  const std::string payload = Json::writeString(writer, body);

  std::string url = cfg_.baseUrl;
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/chat/completions";

  std::string responseBody;
  if (!httpPostJson(url, cfg_.apiKey, payload, &responseBody, err)) {
    return false;
  }
  Json::Value resp;
  {
    Json::CharReaderBuilder builder;
    std::string errs;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(responseBody.data(),
                       responseBody.data() + responseBody.size(), &resp,
                       &errs)) {
      *err = "bad response json";
      return false;
    }
  }
  const Json::Value &choices = resp["choices"];
  if (!choices.isArray() || choices.empty()) {
    *err = "no choices";
    return false;
  }
  const std::string content =
      choices[0]["message"].get("content", "").asString();
  *out = parseReviewCandidates(content);
  if (out->empty()) {
    *err = "empty ai result";
    return false;
  }
  return true;
}

} // namespace Moqi