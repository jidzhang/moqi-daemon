// 5e WebDAV 守卫/命名纯函数回归（黄金 = Go guard.go 行为）。
#include <cstdio>
#include <string>

#include "DaemonWebDAV.h"

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* name) {
  if (ok) {
    ++g_pass;
    std::printf("PASS %s\n", name);
  } else {
    ++g_fail;
    std::printf("FAIL %s\n", name);
  }
}

int main() {
  using namespace Moqi;
  std::string body;

  // MD5（Go md5 十六进制；向量 = md5("hello") = 5d41402abc4b2a76b9719d911017c592）
  check(contentMD5Hex("hello") == "5d41402abc4b2a76b9719d911017c592",
        "md5 hello");
  check(buildClipFilename("hello") ==
            contentMD5Hex("hello") + ".txt", "filename = md5.txt");
  // 中文 UTF-8 稳定性（只断言长度与后缀；具体值跨实现一致由 hello 锚定）
  check(buildClipFilename("\xe4\xbd\xa0\xe5\xa5\xbd").size() == 36,
        "md5 cjk filename length");

  // IsClipFileName
  check(isClipFileName(std::string(31, 'a') + ".txt") == false,
        "31-hex rejected (len 31)");
  check(isClipFileName(std::string(32, 'a') + ".txt"), "32 hex lower ok");
  check(isClipFileName(std::string(32, 'A') + ".txt") == false,
        "32 hex upper rejected");
  check(isClipFileName("clip_2024.txt"), "clip_ prefix ok");
  check(isClipFileName("CLIP_2024.txt"), "clip_ case-insensitive ok");
  check(isClipFileName("a/b.txt") == false, "slash rejected");
  check(isClipFileName("readme.md") == false, "non-txt rejected");

  // uploadable
  check(isUploadableText("ab"), "len2 ok");
  check(isUploadableText("a") == false, "len1 rejected");
  check(isUploadableText("   \t ") == false, "blank rejected");
  check(isUploadableText("\xe2\x97\x8f" "\xe2\x97\x8f" "\xe2\x97\x8f" "\xe2\x97\x8f") == false,
        "masked bullets rejected");
  check(isUploadableText("\xe2\x97\x8f" "a" "\xe2\x97\x8f" "b") == true,
        "mixed mask+text ok");

  // preview
  check(formatPreview("", 60) == "\xEF\xBC\x88\xE7\xA9\xBA\xEF\xBC\x89",
        "empty preview = (empty)");
  check(formatPreview("a\n\nb  c", 60) == "a b c", "whitespace folded");
  // 截断按字符数（3 个中文字符截到 2 → 2 汉字 + …）
  check(formatPreview("\xe4\xbd\xa0\xe5\xa5\xbd\xe5\x91\x80", 2) ==
            "\xe4\xbd\xa0\xe5\xa5\xbd\xe2\x80\xa6",
        "truncate by chars with ellipsis");

  // settings root 归一
  check(normalizeWebDavDir("") == "/moqi-input-method/", "empty -> default");
  check(normalizeWebDavDir("moqi-input-method") == "/moqi-input-method/",
        "slashes added");
  check(normalizeWebDavDir("/a/b/") == "/a/b/", "already normalized");

  // base url 门禁（Go 先 ToLower 再比前缀：HTTPS:// 也通过）
  check(isAllowedBaseUrl("http://1.2.3.4:5005/"), "http ok");
  check(isAllowedBaseUrl("HTTPS://x/"), "upper https ok (lowered)");
  check(isAllowedBaseUrl("ftp://x/") == false, "ftp rejected");

  // ---- 可选网络段：F:\work\IME\fake_webdav.py 起本地假服务后设
  // WEBDAV_FAKE_URL=http://127.0.0.1:18077/dav/ 运行（无则跳过）。
  if (const char* url = std::getenv("WEBDAV_FAKE_URL")) {
    WebDavConfig cfg;
    cfg.baseUrl = url;
    cfg.username = "u1";
    cfg.password = "p1";
    std::string err;
    WebDavClient cli(cfg);
    check(cli.testConnection(&err), "net: test connection (dirs created)");
    if (!err.empty()) std::printf("  testConnection err: %s\n", err.c_str());
    const std::string text = "\xe4\xbd\xa0\xe5\xa5\xbd cloud";
    check(cli.uploadClip(buildClipFilename(text), text, &err),
          "net: upload clip");
    check(cli.downloadClip(buildClipFilename(text), &body, &err) && body == text,
          "net: download roundtrip");
    std::vector<ClipEntry> clips;
    const bool listOk = cli.listClips(&clips, &err);
    std::printf("  listClips ok=%d n=%zu err=%s\n", listOk ? 1 : 0,
                clips.size(), err.c_str());
    for (const auto& c : clips) {
      std::printf("    clip: %s (mod %lld)\n", c.name.c_str(),
                  c.lastModifiedMs);
    }
    check(listOk && clips.size() == 1 &&
              clips[0].name == buildClipFilename(text),
          "net: list clips");
    check(cli.deleteClip(buildClipFilename(text), &err), "net: delete clip");
    std::vector<ClipEntry> after;
    check(cli.listClips(&after, &err) && after.empty(), "net: list empty");
    // 错误密码 → 401 文案
    WebDavConfig bad = cfg;
    bad.password = "wrong";
    WebDavClient badCli(bad);
    check(badCli.testConnection(&err) == false &&
              err.find("\xe8\xae\xa4\xe8\xaf\x81\xe5\xa4\xb1\xe8\xb4\xa5") == 0,
          "net: 401 message");
  }

  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
