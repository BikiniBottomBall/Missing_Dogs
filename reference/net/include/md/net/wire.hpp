// 线协议：Command 的解析 与 PlayerView 的编码。
//
// 这一层被两个宿主共用，是"单机与联机走同一套协议"的落点：
//   · apps/md_server    —— 走 TCP
//   · godot/addons/md_core（GDExtension） —— 走进程内调用
//
// 两者的客户端代码因此完全一致，只是 transport 不同。
//
// 设计取舍：
//   · 只绑 3 个方法 + 1 个信号，其余走 JSON。回合制卡牌每回合几 KB，
//     JSON 不是瓶颈，而手写 GDExtension 绑定样板很贵。
//   · 牌类别用整数下标（0..5）而不是中文串，避免任何编码问题，也让包更小。
//   · 客户端发来的 seat 一律丢弃，服务器以连接自身的座位为准。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "md/core.hpp"

namespace md::net {

// ---------------------------------------------------------------- 极简 JSON
//
// 只解析扁平对象：{"key": 数字 | 布尔 | 字符串, ...}
// 够用即可——客户端发来的只有命令，没有嵌套结构。

struct FlatValue {
  enum class Kind { Number, Boolean, String } kind = Kind::Number;
  long number = 0;
  bool boolean = false;
  std::string text;
};

class FlatJson {
 public:
  bool parse(std::string_view text);
  const FlatValue* find(std::string_view key) const;
  long num(std::string_view key, long def = 0) const;
  bool boolean(std::string_view key, bool def = false) const;
  std::string str(std::string_view key, std::string_view def = "") const;

 private:
  std::vector<std::pair<std::string, FlatValue>> kv_;
};

std::string json_quote(std::string_view s);

// ---------------------------------------------------------------- 编解码

// pending 为 nullptr 时表示"轮不到你行动"。
std::string encode_view(const PlayerView& v, const Pending* pending);

// 解析客户端命令。out.seat 会被置为客户端自报的座位，
// **调用方必须用连接的真实座位覆盖它**。
bool decode_command(std::string_view json, Command& out);

std::string encode_joined(int seat);
std::string encode_error(std::string_view msg);
std::string encode_result(md::Winner winner, int rounds, bool mole_alive);

}  // namespace md::net
