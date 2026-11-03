// 5e 云剪贴板/WebDAV 客户端实现（WinHTTP）。语义/文案对齐 Go
// cloudclipboard/client.go，规格见 docs/5e-cloud-clipboard-port-spec.md。
// include 纪律：RimeCore.h 链开篇（见 ChildProxy.h 注释）。
#include "RimeCore.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>

#include "DaemonWebDAV.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace Moqi {
namespace {

// --------------------------------------------------------------- 小工具 --

std::string toLower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

std::string trim(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  return s.substr(b, e - b);
}

std::wstring utf8ToWs(const std::string& s) {
  if (s.empty()) return L"";
  int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring ws(n, 0);
  ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &ws[0], n);
  return ws;
}

std::string wsToUtf8(const std::wstring& ws) {
  if (ws.empty()) return "";
  int n = ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr,
                                0, nullptr, nullptr);
  std::string s(n, 0);
  ::WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), &s[0], n,
                        nullptr, nullptr);
  return s;
}

std::string trimRight(const std::string& s, char c) {
  std::string r = s;
  while (!r.empty() && r.back() == c) r.pop_back();
  return r;
}

// 去掉 URL 路径部分的前导 "/"（Go TrimPrefix(path, "/")），仅用于拼相对段。
std::string stripLeadingSlash(const std::string& s) {
  std::string r = s;
  while (!r.empty() && r.front() == '/') r.erase(r.begin());
  return r;
}

std::string urlPathEscape(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

std::string urlPathUnescape(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(s[i + 1]), lo = nib(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    out += s[i];
  }
  return out;
}

// Basic 凭据头（base64 走 crypt32，勿手搛）。
std::string basicAuthHeader(const std::string& user, const std::string& pass) {
  const std::string raw = user + ":" + pass;
  DWORD n = 0;
  ::CryptBinaryToStringA((const BYTE*)raw.data(), (DWORD)raw.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr,
                         &n);
  std::string b64(n > 0 ? n - 1 : 0, 0);
  if (n > 0) {
    ::CryptBinaryToStringA((const BYTE*)raw.data(), (DWORD)raw.size(),
                           CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &b64[0],
                           &n);
  }
  while (!b64.empty() && (b64.back() == '\r' || b64.back() == '\n')) {
    b64.pop_back();
  }
  return "Authorization: Basic " + b64;
}

// RFC1123 HTTP 日期 → Unix 毫秒（WinHttpTimeToSystemTime）。
long long httpDateToUnixMs(const std::string& httpDate) {
  SYSTEMTIME st = {};
  if (::WinHttpTimeToSystemTime(utf8ToWs(httpDate).c_str(), &st)) {
    FILETIME ft = {};
    SYSTEMTIME stUtc = st;  // WinHttpTimeToSystemTime 已按 GMT 解释
    if (::SystemTimeToFileTime(&stUtc, &ft)) {
      ULARGE_INTEGER u{};
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      // FILETIME(1601 起 100ns) → Unix ms
      return (long long)((u.QuadPart - 116444736000000000ULL) / 10000ULL);
    }
  }
  return 0;
}

// 轻量 XML 实体解码（displayname 里 &amp; &lt; &gt; &quot; &apos; &#xN;）。
std::string decodeXmlEntities(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '&') {
      size_t sc = s.find(';', i);
      if (sc != std::string::npos && sc - i <= 10) {
        const std::string ent = s.substr(i + 1, sc - i - 1);
        if (ent == "amp") { out += '&'; i = sc + 1; continue; }
        if (ent == "lt") { out += '<'; i = sc + 1; continue; }
        if (ent == "gt") { out += '>'; i = sc + 1; continue; }
        if (ent == "quot") { out += '"'; i = sc + 1; continue; }
        if (ent == "apos") { out += '\''; i = sc + 1; continue; }
        if (ent.size() > 1 && (ent[0] == 'x' || ent[0] == 'X') &&
            ent.find_first_not_of("0123456789abcdefABCDEF", 1) ==
                std::string::npos) {
          unsigned long cp = std::stoul(ent.substr(1), nullptr, 16);
          if (cp < 0x80) {
            out += (char)cp;
          } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
          } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
          }
          i = sc + 1;
          continue;
        }
      }
    }
    out += s[i++];
  }
  return out;
}

// 前缀无关的开标签查找：匹配 <[prefix:]localName ...>，返回 '>' 的位置，
// contentStart = 标签内容起点。找不到返回 npos。
size_t findOpenTag(const std::string& xml, size_t from, const char* localName,
                   size_t* contentStart) {
  const std::string ln = localName;
  size_t pos = from;
  while (pos < xml.size()) {
    size_t lt = xml.find('<', pos);
    if (lt == std::string::npos) return std::string::npos;
    size_t p = lt + 1;
    size_t nameStart = p;
    while (p < xml.size() &&
           (std::isalnum((unsigned char)xml[p]) || xml[p] == '_' ||
            xml[p] == '-' || xml[p] == '.')) {
      ++p;
    }
    if (p < xml.size() && xml[p] == ':') nameStart = p + 1;
    if (xml.compare(nameStart, ln.size(), ln) == 0) {
      size_t after = nameStart + ln.size();
      if (after < xml.size() &&
          (xml[after] == '>' || xml[after] == '/' ||
           std::isspace((unsigned char)xml[after]))) {
        size_t gt = xml.find('>', after);
        if (gt != std::string::npos) {
          if (contentStart) *contentStart = gt + 1;
          return gt;
        }
      }
    }
    pos = lt + 1;
  }
  return std::string::npos;
}

// 前缀无关的标签内容提取：<[prefix:]localName>…</[prefix:]localName>。
bool extractTagContent(const std::string& xml, size_t from,
                       const char* localName, std::string* out,
                       size_t* endOfClose) {
  size_t contentStart = 0;
  size_t gt = findOpenTag(xml, from, localName, &contentStart);
  if (gt == std::string::npos) return false;
  // 闭合标签（允许任意前缀）
  size_t close = xml.find("</", gt + 1);
  while (close != std::string::npos) {
    size_t closeEnd = xml.find('>', close);
    if (closeEnd == std::string::npos) return false;
    const std::string closing = xml.substr(close + 2, closeEnd - close - 2);
    const size_t colon = closing.find(':');
    const std::string closeLocal =
        colon == std::string::npos ? closing : closing.substr(colon + 1);
    if (closeLocal == localName) {
      if (out) *out = decodeXmlEntities(xml.substr(contentStart, close - contentStart));
      if (endOfClose) *endOfClose = closeEnd + 1;
      return true;
    }
    close = xml.find("</", closeEnd + 1);
  }
  return false;
}

// 多状态解析：逐 <...response> 块取 displayname（缺失回退 href 末段）
// + getlastmodified + 是否目录。
struct PropfindEntry {
  std::string name;
  long long lastModifiedMs = 0;
  bool isDir = false;
};

void parsePropfind(const std::string& xml, std::vector<PropfindEntry>* out) {
  size_t pos = 0;
  for (;;) {
    size_t contentStart = 0;
    size_t gt = findOpenTag(xml, pos, "response", &contentStart);
    if (gt == std::string::npos) break;
    // 本块闭合 </…response>（response 不嵌套，配对从 gt 起最近的同名闭合）。
    size_t close = gt + 1;
    size_t blockEnd = xml.size();
    for (;;) {
      close = xml.find("</", close);
      if (close == std::string::npos) break;
      const size_t closeEnd = xml.find('>', close);
      if (closeEnd == std::string::npos) break;
      const std::string closing =
          xml.substr(close + 2, closeEnd - close - 2);
      const size_t colon = closing.find(':');
      const std::string closeLocal =
          colon == std::string::npos ? closing : closing.substr(colon + 1);
      if (closeLocal == "response") {
        blockEnd = close;
        break;
      }
      close = closeEnd + 1;
    }
    const std::string block =
        xml.substr(contentStart, blockEnd - contentStart);
    std::string name, modified, href;
    extractTagContent(block, 0, "displayname", &name, nullptr);
    if (name.empty() && extractTagContent(block, 0, "href", &href, nullptr)) {
      std::string h = trim(href);
      while (!h.empty() && h.back() == '/') h.pop_back();
      const size_t slash = h.find_last_of('/');
      name = urlPathUnescape(slash == std::string::npos ? h
                                                       : h.substr(slash + 1));
    }
    if (name.empty()) {
      if (blockEnd >= xml.size()) break;
      pos = blockEnd + 1;
      continue;
    }
    PropfindEntry e;
    e.name = name;
    e.isDir = block.find("collection") != std::string::npos;
    if (extractTagContent(block, 0, "getlastmodified", &modified, nullptr)) {
      e.lastModifiedMs = httpDateToUnixMs(trim(modified));
    }
    out->push_back(e);
    if (blockEnd >= xml.size()) break;
    pos = blockEnd + 1;
  }
}

}  // namespace

// --------------------------------------------------------------- 守卫 --

std::string contentMD5Hex(const std::string& text) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  std::string hex;
  if (::CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES,
                             CRYPT_VERIFYCONTEXT)) {
    if (::CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
      if (::CryptHashData(hash, (const BYTE*)text.data(), (DWORD)text.size(),
                          0)) {
        DWORD len = 16;
        BYTE digest[16] = {};
        if (::CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0)) {
          static const char* d = "0123456789abcdef";
          for (int i = 0; i < 16; ++i) {
            hex += d[digest[i] >> 4];
            hex += d[digest[i] & 0xF];
          }
        }
      }
      ::CryptDestroyHash(hash);
    }
    ::CryptReleaseContext(prov, 0);
  }
  return hex;
}

std::string buildClipFilename(const std::string& text) {
  return contentMD5Hex(text) + ".txt";
}

bool isClipFileName(const std::string& name) {
  const std::string lower = toLower(name);
  if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".txt") != 0) {
    return false;
  }
  if (name.find('/') != std::string::npos) return false;
  // Go isHexLower：只认小写 hex 的原文 base。
  const std::string base = name.substr(0, name.size() - 4);
  if (base.size() == 32 &&
      base.find_first_not_of("0123456789abcdef") == std::string::npos) {
    return true;
  }
  return lower.compare(0, 5, "clip_") == 0;
}

bool isUserDictSnapshotFileName(const std::string& name) {
  const std::string lower = toLower(name);
  const std::string suffix = ".userdb.txt";
  if (lower.size() <= suffix.size() ||
      lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) !=
          0) {
    return false;
  }
  if (name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos) {
    return false;
  }
  const std::string base = name.substr(0, name.size() - suffix.size());
  return !base.empty() && base != "." && base != "..";
}

bool isSyncDeviceDirName(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 128) {
    return false;
  }
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c >= 0x80) {
      continue;
    }
    return false;
  }
  return true;
}

bool isSchemeSetDirName(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return false;
  if (utf8ToWs(name).size() > 128) return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\' || c == ':') return false;
  }
  return true;
}

bool isUploadableText(const std::string& utf8) {
  if (utf8.size() < 2 || utf8.size() > 32768) return false;
  if (trim(utf8).empty()) return false;
  // 掩码串（*•●· 占 ≥80% 字符）不传（Go looksLikeMaskedSecret，按码点计）。
  std::wstring ws = utf8ToWs(utf8);
  if (ws.size() >= 4) {
    size_t masked = 0;
    for (wchar_t c : ws) {
      if (c == L'*' || c == L'\u2022' || c == L'\u25CF' || c == L'\u00B7') ++masked;
    }
    if (masked >= ws.size() * 8 / 10) return false;
  }
  return true;
}

std::string normalizePlainText(const std::string& utf8) { return trim(utf8); }

std::string formatPreview(const std::string& utf8, size_t maxChars) {
  if (utf8.empty()) return "\xEF\xBC\x88\xE7\xA9\xBA\xEF\xBC\x89";  // （空）
  // 空白折叠单行（按 UTF-8 字符遍历）。
  std::wstring ws = utf8ToWs(utf8);
  std::wstring single;
  bool pendingSpace = false;
  for (wchar_t c : ws) {
    if (iswspace(c)) {
      pendingSpace = !single.empty();
    } else {
      if (pendingSpace) single += L' ';
      pendingSpace = false;
      single += c;
    }
  }
  if (single.size() <= maxChars) return wsToUtf8(single);
  single = single.substr(0, maxChars) + L"\u2026";
  return wsToUtf8(single);
}

std::string normalizeWebDavDir(const std::string& raw) {
  std::string path = trim(raw);
  if (path.empty()) path = "/moqi-input-method/";
  if (path.front() != '/') path = "/" + path;
  if (path.back() != '/') path += "/";
  return path;
}

bool isAllowedBaseUrl(const std::string& url) {
  const std::string u = toLower(trim(url));
  return u.compare(0, 7, "http://") == 0 || u.compare(0, 8, "https://") == 0;
}

const char* webDavRejectionMessage() {
  return "WebDAV 地址须以 http:// 或 https:// 开头";
}

// --------------------------------------------------------------- 客户端 --

namespace {
const char kPropfindBody[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n"
    "<d:propfind xmlns:d=\"DAV:\">\n"
    "  <d:prop>\n"
    "    <d:getlastmodified/>\n"
    "    <d:displayname/>\n"
    "  </d:prop>\n"
    "</d:propfind>";
}  // namespace

WebDavClient::WebDavClient(const WebDavConfig& cfg) : cfg_(cfg) {}

int WebDavClient::request(const wchar_t* verb, const std::string& url,
                          const std::string* body, const char* contentType,
                          int depth, std::string* respBody, std::string* err) {
  URL_COMPONENTSW parts = {};
  parts.dwStructSize = sizeof(parts);
  wchar_t host[256] = {};
  wchar_t path[1024] = {};
  parts.lpszHostName = host;
  parts.dwHostNameLength = 255;
  parts.lpszUrlPath = path;
  parts.dwUrlPathLength = 1023;
  const std::wstring wurl = utf8ToWs(url);
  if (!::WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) {
    if (err) *err = "WebDAV 地址无效";
    return -1;
  }
  const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

  HINTERNET session = ::WinHttpOpen(L"MoqiIME/2.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    if (err) *err = "网络初始化失败";
    return -1;
  }
  ::WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

  HINTERNET connect =
      ::WinHttpConnect(session, host, parts.nPort, 0);
  if (!connect) {
    ::WinHttpCloseHandle(session);
    if (err) *err = "无法连接 WebDAV 服务器";
    return -1;
  }
  HINTERNET req = ::WinHttpOpenRequest(
      connect, verb, path, nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!req) {
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    if (err) *err = "请求创建失败";
    return -1;
  }
  const std::string auth = basicAuthHeader(cfg_.username, cfg_.password);
  ::WinHttpAddRequestHeaders(req, utf8ToWs(auth).c_str(), (DWORD)-1,
                            WINHTTP_ADDREQ_FLAG_ADD);
  if (contentType) {
    const std::string ct = std::string("Content-Type: ") + contentType;
    ::WinHttpAddRequestHeaders(req, utf8ToWs(ct).c_str(), (DWORD)-1,
                            WINHTTP_ADDREQ_FLAG_ADD);
  }
  if (depth >= 0) {
    const std::string d = "Depth: " + std::to_string(depth);
    ::WinHttpAddRequestHeaders(req, utf8ToWs(d).c_str(), (DWORD)-1,
                            WINHTTP_ADDREQ_FLAG_ADD);
  }

  const BOOL sent = ::WinHttpSendRequest(
      req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
      body ? (LPVOID)body->data() : WINHTTP_NO_REQUEST_DATA,
      body ? (DWORD)body->size() : 0,
      body ? (DWORD)body->size() : 0, 0);
  int status = -1;
  if (sent && ::WinHttpReceiveResponse(req, nullptr)) {
    DWORD code = 0, size = sizeof(code);
    ::WinHttpQueryHeaders(req,
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                          WINHTTP_NO_HEADER_INDEX);
    status = (int)code;
    if (respBody) {
      for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        std::string chunk(avail, 0);
        DWORD readBytes = 0;
        if (!::WinHttpReadData(req, &chunk[0], avail, &readBytes)) break;
        chunk.resize(readBytes);
        *respBody += chunk;
        if (readBytes == 0) break;
      }
    }
  } else {
    if (err) *err = "无法连接 WebDAV 服务器";
  }
  ::WinHttpCloseHandle(req);
  ::WinHttpCloseHandle(connect);
  ::WinHttpCloseHandle(session);
  return status;
}

bool WebDavClient::propfindOk(const std::string& url, int depth,
                              std::string* body, int* status,
                              std::string* err) {
  const std::string pf = kPropfindBody;
  int st = request(L"PROPFIND", url, &pf, "text/xml; charset=utf-8", depth,
                   body, err);
  if (status) *status = st;
  return st >= 200 && st <= 299;
}

bool WebDavClient::directoryExists(const std::string& url) {
  std::string err;
  int status = 0;
  return propfindOk(url, 0, nullptr, &status, &err);
}

bool WebDavClient::mkcol(const std::string& url) {
  std::string err;
  const int st = request(L"MKCOL", url, nullptr, nullptr, -1, nullptr, &err);
  return st >= 200 && st <= 299;
}

bool WebDavClient::ensureRemoteDirectory(std::string* err) {
  const std::string base = trimRight(cfg_.baseUrl, '/') + "/";
  int status = 0;
  if (!propfindOk(base, 0, nullptr, &status, nullptr)) {
    if (err) {
      if (status == 401) {
        *err = "认证失败：请检查用户名和密码。飞牛须使用「可见文件夹范围」所属账号登录";
      } else if (status > 0) {
        *err = "无法访问 WebDAV 根路径 HTTP " + std::to_string(status) +
               "。飞牛地址请填 http://192.168.x.x:5005/（注意末尾 /）";
      }
    }
    return false;
  }
  // SettingsRoot
  const std::string settingsPath =
      stripLeadingSlash(normalizeWebDavDir(cfg_.settingsRoot));
  const std::string settingsUrl =
      trimRight(cfg_.baseUrl, '/') + "/" + trimRight(settingsPath, '/') + "/";
  if (!directoryExists(settingsUrl)) {
    if (!mkcol(settingsUrl) && !directoryExists(settingsUrl)) {
      if (err) *err = "设置目录 " + cfg_.settingsRoot + " 不存在且无法自动创建。请在 NAS 文件管理中手动新建，或把设置目录改为 /";
      return false;
    }
  }
  // clip/（Go ClipDirectoryURL 带尾随斜杠；MKCOL/PROPFIND 必须同形）
  const std::string clipUrl = trimRight(cfg_.baseUrl, '/') + "/" +
                              stripLeadingSlash(settingsPath) + "clip/";
  if (directoryExists(clipUrl)) return true;
  if (mkcol(clipUrl) && directoryExists(clipUrl)) return true;
  if (err) *err = "剪贴板目录 clip 无法创建。请在 " + cfg_.settingsRoot +
                  " 下手动新建 clip 文件夹";
  return false;
}

bool WebDavClient::testConnection(std::string* err) {
  return ensureRemoteDirectory(err);
}

bool WebDavClient::listClips(std::vector<ClipEntry>* out, std::string* err) {
  if (!ensureRemoteDirectory(err)) return false;
  const std::string settingsPath =
      stripLeadingSlash(normalizeWebDavDir(cfg_.settingsRoot));
  const std::string clipUrl =
      trimRight(cfg_.baseUrl, '/') + "/" + stripLeadingSlash(settingsPath) + "clip/";
  std::string body;
  int status = 0;
  if (!propfindOk(clipUrl, 1, &body, &status, err)) {
    if (err && status > 0) *err = "PROPFIND failed: HTTP " + std::to_string(status);
    return false;
  }
  std::vector<PropfindEntry> entries;
  parsePropfind(body, &entries);
  std::map<std::string, bool> seen;
  for (const auto& e : entries) {
    if (!isClipFileName(e.name) || seen.count(e.name)) continue;
    seen[e.name] = true;
    ClipEntry c;
    c.name = e.name;
    c.lastModifiedMs = e.lastModifiedMs;
    out->push_back(c);
  }
  std::stable_sort(out->begin(), out->end(),
                   [](const ClipEntry& a, const ClipEntry& b) {
                     return a.lastModifiedMs > b.lastModifiedMs;
                   });
  return true;
}

bool WebDavClient::uploadClip(const std::string& filename,
                              const std::string& text, std::string* err) {
  if (!ensureRemoteDirectory(err)) return false;
  const std::string settingsPath =
      stripLeadingSlash(normalizeWebDavDir(cfg_.settingsRoot));
  const std::string url = trimRight(cfg_.baseUrl, '/') + "/" +
                          stripLeadingSlash(settingsPath) + "clip/" + filename;
  const int st = request(L"PUT", url, &text, "text/plain; charset=utf-8", -1,
                         nullptr, err);
  if (st < 200 || st > 299) {
    if (err) *err = "PUT failed: HTTP " + std::to_string(st);
    return false;
  }
  return true;
}

bool WebDavClient::downloadClip(const std::string& name, std::string* body,
                                std::string* err) {
  const std::string settingsPath =
      stripLeadingSlash(normalizeWebDavDir(cfg_.settingsRoot));
  const std::string url = trimRight(cfg_.baseUrl, '/') + "/" +
                          stripLeadingSlash(settingsPath) + "clip/" + name;
  const int st = request(L"GET", url, nullptr, nullptr, -1, body, err);
  if (st < 200 || st > 299) {
    if (err) *err = "GET failed: HTTP " + std::to_string(st);
    return false;
  }
  return true;
}

bool WebDavClient::deleteClip(const std::string& name, std::string* err) {
  if (!isClipFileName(name)) {
    if (err) *err = "invalid clip filename";
    return false;
  }
  const std::string settingsPath =
      stripLeadingSlash(normalizeWebDavDir(cfg_.settingsRoot));
  const std::string url = trimRight(cfg_.baseUrl, '/') + "/" +
                          stripLeadingSlash(settingsPath) + "clip/" + name;
  const int st = request(L"DELETE", url, nullptr, nullptr, -1, nullptr, err);
  if ((st < 200 || st > 299) && st != 404) {
    if (err) *err = "DELETE failed: HTTP " + std::to_string(st);
    return false;
  }
  return true;
}

namespace {
std::string settingsPathOf(const WebDavConfig& cfg) {
  return stripLeadingSlash(normalizeWebDavDir(cfg.settingsRoot));
}
std::string dictRootUrlOf(const WebDavConfig& cfg) {
  return trimRight(cfg.baseUrl, '/') + "/" + settingsPathOf(cfg) + "dict/";
}
std::string schemeDirUrlOf(const WebDavConfig& cfg,
                           const std::string& schemeSet) {
  return dictRootUrlOf(cfg) + urlPathEscape(schemeSet) + "/";
}
std::string deviceDirUrlOf(const WebDavConfig& cfg, const std::string& schemeSet,
                           const std::string& deviceId) {
  return schemeDirUrlOf(cfg, schemeSet) + urlPathEscape(deviceId) + "/";
}
std::string snapshotUrlOf(const WebDavConfig& cfg, const std::string& schemeSet,
                          const std::string& deviceId,
                          const std::string& name) {
  return deviceDirUrlOf(cfg, schemeSet, deviceId) + urlPathEscape(name);
}
}  // namespace

bool WebDavClient::listUserDictSnapshots(
    const std::string& schemeSet, std::vector<UserDictSnapshotEntry>* out,
    std::string* err) {
  if (!isSchemeSetDirName(schemeSet)) {
    if (err) *err = "方案集名称无效";
    return false;
  }
  if (!ensureRemoteDirectory(err)) return false;
  // dict/ 与 dict/<方案集>/ 按需创建；部分 NAS 不允许 MKCOL 中间层时
  // 给出与 Go 同义的可见提示。
  const std::string dictUrl = dictRootUrlOf(cfg_);
  if (!directoryExists(dictUrl) && !(mkcol(dictUrl) && directoryExists(dictUrl))) {
    if (err) *err = "用户词库同步目录 dict 无法创建";
    return false;
  }
  const std::string schemeUrl = schemeDirUrlOf(cfg_, schemeSet);
  if (!directoryExists(schemeUrl) &&
      !(mkcol(schemeUrl) && directoryExists(schemeUrl))) {
    if (err) {
      *err = "词库同步子目录 " + schemeSet +
             " 无法创建，请手动新建 " + cfg_.settingsRoot + "dict/" + schemeSet;
    }
    return false;
  }
  std::string body;
  int status = 0;
  if (!propfindOk(schemeUrl, 1, &body, &status, err)) {
    if (err && status > 0) {
      *err = "PROPFIND dict failed: HTTP " + std::to_string(status);
    }
    return false;
  }
  std::vector<PropfindEntry> devices;
  parsePropfind(body, &devices);
  std::map<std::string, bool> seenDevices;
  for (const auto& d : devices) {
    if (!d.isDir || !isSyncDeviceDirName(d.name) || seenDevices.count(d.name)) {
      continue;
    }
    seenDevices[d.name] = true;
    std::string deviceBody;
    int deviceStatus = 0;
    if (!propfindOk(deviceDirUrlOf(cfg_, schemeSet, d.name), 1, &deviceBody,
                    &deviceStatus, err)) {
      if (err && deviceStatus > 0) {
        *err = "PROPFIND device failed: HTTP " +
               std::to_string(deviceStatus);
      }
      return false;
    }
    std::vector<PropfindEntry> files;
    parsePropfind(deviceBody, &files);
    std::map<std::string, bool> seenFiles;
    for (const auto& f : files) {
      if (f.isDir || !isUserDictSnapshotFileName(f.name) ||
          seenFiles.count(f.name)) {
        continue;
      }
      seenFiles[f.name] = true;
      UserDictSnapshotEntry e;
      e.schemeSet = schemeSet;
      e.deviceId = d.name;
      e.name = f.name;
      out->push_back(e);
    }
  }
  return true;
}

bool WebDavClient::downloadUserDictSnapshot(
    const UserDictSnapshotEntry& entry, std::string* data, std::string* err) {
  if (!isSchemeSetDirName(entry.schemeSet) ||
      !isSyncDeviceDirName(entry.deviceId) ||
      !isUserDictSnapshotFileName(entry.name)) {
    if (err) *err = "invalid user dict snapshot path";
    return false;
  }
  const int st = request(
      L"GET", snapshotUrlOf(cfg_, entry.schemeSet, entry.deviceId, entry.name),
      nullptr, nullptr, -1, data, err);
  if (st < 200 || st > 299) {
    if (err) *err = "GET user dict snapshot failed: HTTP " + std::to_string(st);
    return false;
  }
  return true;
}

bool WebDavClient::uploadUserDictSnapshot(const UserDictSnapshotEntry& entry,
                                          const std::string& data,
                                          std::string* err) {
  if (!isSchemeSetDirName(entry.schemeSet) ||
      !isSyncDeviceDirName(entry.deviceId) ||
      !isUserDictSnapshotFileName(entry.name)) {
    if (err) *err = "invalid user dict snapshot path";
    return false;
  }
  const std::string schemeUrl = schemeDirUrlOf(cfg_, entry.schemeSet);
  if (!directoryExists(schemeUrl) &&
      !(mkcol(schemeUrl) && directoryExists(schemeUrl))) {
    if (err) *err = "词库同步子目录 " + entry.schemeSet + " 无法创建";
    return false;
  }
  const std::string deviceUrl =
      deviceDirUrlOf(cfg_, entry.schemeSet, entry.deviceId);
  if (!directoryExists(deviceUrl) &&
      !(mkcol(deviceUrl) && directoryExists(deviceUrl))) {
    if (err) *err = "词库同步设备目录无法创建";
    return false;
  }
  const int st = request(L"PUT",
                         snapshotUrlOf(cfg_, entry.schemeSet, entry.deviceId,
                                       entry.name),
                         &data, "text/plain; charset=utf-8", -1, nullptr,
                         err);
  if (st < 200 || st > 299) {
    if (err) *err = "PUT user dict snapshot failed: HTTP " + std::to_string(st);
    return false;
  }
  return true;
}

}  // namespace Moqi
