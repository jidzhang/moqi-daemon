// proto_framing_test —— 管道帧协议回归（历史事故：>4096B 帧被静默丢块）。
// 同时锁定健壮性边界：损坏/恶意长度前缀不得让接收方永久挂起或无限分配。
// include 纪律：jsoncpp 必须先于裸 STL（MSVC STL 在 <cmath> 崩，见
// ChildProxy.h 注释与 memory windows-build-discipline）。
#include <json/json.h>

#include "ProtoFraming.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <algorithm>

using Moqi::Proto::FrameBuffer;
using Moqi::Proto::framePayload;

static int g_failed = 0;

static void check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failed;
}

int main() {
  // --- 基本往返 ---
  {
    FrameBuffer fb;
    fb.append(framePayload("hello").data(), framePayload("hello").size());
    std::string out;
    check(fb.nextFrame(out) && out == "hello", "roundtrip single frame");
    check(!fb.nextFrame(out), "no second frame");
  }

  // --- 粘包：两帧一次喂入 ---
  {
    FrameBuffer fb;
    const std::string a = framePayload("aaa");
    const std::string b = framePayload("bb");
    const std::string both = a + b;
    fb.append(both.data(), both.size());
    std::string out;
    check(fb.nextFrame(out) && out == "aaa", "sticky: first frame");
    check(fb.nextFrame(out) && out == "bb", "sticky: second frame");
    check(!fb.nextFrame(out), "sticky: exhausted");
  }

  // --- 半包：分片喂入（历史事故核心：>4096B 帧被拆多次读） ---
  {
    FrameBuffer fb;
    const std::string big(10000, 'x'); // 远大于 4096 读缓冲
    const std::string framed = framePayload(big);
    std::string out;
    bool got = false;
    for (size_t i = 0; i < framed.size(); i += 1000) {
      fb.append(framed.data() + i, std::min<size_t>(1000, framed.size() - i));
      if (fb.nextFrame(out)) {
        got = true;
        break;
      }
    }
    check(got && out == big, "split read: 10KB frame reassembled");
  }

  // --- 空 payload ---
  {
    FrameBuffer fb;
    const std::string framed = framePayload("");
    fb.append(framed.data(), framed.size());
    std::string out;
    check(fb.nextFrame(out) && out.empty(), "empty payload frame");
  }

  // --- 长度前缀字节序（小端，协议固定） ---
  {
    const std::string framed = framePayload("ab");
    check(framed.size() == 6, "frame size = 4 + payload");
    const auto* p = reinterpret_cast<const unsigned char*>(framed.data());
    check(p[0] == 2 && p[1] == 0 && p[2] == 0 && p[3] == 0,
          "length prefix is 4-byte little-endian");
  }

  // --- 健壮性：损坏的巨长前缀不得永久挂起（历史缺陷：无上限检查） ---
  {
    FrameBuffer fb;
    // 声明 4GB payload 但只给 4 字节：修复前会一直等下去（缓冲区无限增长）
    const std::uint32_t bogus = 0xFFFFFFF0u;
    std::string hdr(4, '\0');
    std::memcpy(&hdr[0], &bogus, 4);
    fb.append(hdr.data(), hdr.size());
    std::string out;
    const bool framed = fb.nextFrame(out);
    check(!framed, "absurd length prefix rejected (no frame yet)");
    check(fb.overflowed(), "absurd length prefix sets overflow flag");
  }

  // --- 正常大帧（1MB）不误判为超限 ---
  {
    FrameBuffer fb;
    const std::string big(1024 * 1024, 'y');
    const std::string framed = framePayload(big);
    fb.append(framed.data(), framed.size());
    std::string out;
    check(fb.nextFrame(out) && out == big, "1MB frame accepted");
    check(!fb.overflowed(), "1MB frame does not trip overflow");
  }

  // --- overflow 后 clear 可恢复（重连语义） ---
  {
    FrameBuffer fb;
    const std::uint32_t bogus = 0xFFFFFFF0u;
    std::string hdr(4, '\0');
    std::memcpy(&hdr[0], &bogus, 4);
    fb.append(hdr.data(), hdr.size());
    std::string out;
    fb.nextFrame(out);
    fb.clear();
    check(!fb.overflowed(), "clear() resets overflow flag");
    const std::string framed = framePayload("ok");
    fb.append(framed.data(), framed.size());
    check(fb.nextFrame(out) && out == "ok", "usable after clear");
  }

  if (g_failed == 0) {
    std::printf("11 passed, 0 failed\n");
    return 0;
  }
  std::printf("%d FAILED\n", g_failed);
  return 1;
}