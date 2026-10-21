// stdio 版服务器：从 stdin 读命令，往 stdout 写消息。
//
// 存在的意义：这台机器上 Smart App Control 会拦掉
//   · 链接 ws2_32 的 exe（md_server 跑不起来）
//   · 新编出来的 dll（GDExtension 加载不了）
// 但普通 exe 是放行的。所以这条 transport 让"Godot 客户端 ↔ C++ 服务器"
// 真的跨了一次进程边界，而且是同一份 Room、同一套 JSON 协议。
//
// 帧协议：每条一行 JSON，和 TCP 版完全一致，只多一个同步哨兵。
//   收到一条命令 -> 输出它产生的所有 Delivery -> 输出 {"t":"sync"}
// 客户端读到 sync 就知道"这一轮响应结束了"，可以在阻塞读上正常循环。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "md/core.hpp"
#include "md/net/room.hpp"

namespace {

void emit(const std::string& line) {
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seed = 1;
  int rounds = md::kDefaultRounds;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (std::strcmp(a, "--seed") == 0 && has_next) seed = static_cast<uint64_t>(std::atoll(argv[++i]));
    else if (std::strcmp(a, "--rounds") == 0 && has_next) rounds = std::atoi(argv[++i]);
  }

  md::net::Room room(seed, rounds);
  emit("{\"t\":\"ready\"}");

  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty()) {
      std::vector<md::net::Delivery> out;
      room.on_message(0, line, out);
      for (const auto& d : out) emit(d.line);
    }
    emit("{\"t\":\"sync\"}");
  }
  return 0;
}
