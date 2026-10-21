// Room：一局对战的会话管理，**不依赖 socket**。
//
// 这样拆的理由不只是为了可测试：真正的逻辑（座位、回合、隐藏信息裁剪、
// 掉线转 AI）必须能在没有网络的情况下验证。apps/md_server 只负责
// 把 Delivery 投递到 socket 上。
//
// 连接用整数 conn 标识（由调用方给出，服务器里就是 peers 的下标）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "md/core.hpp"

namespace md::net {

// 一条要投递出去的消息。
struct Delivery {
  int conn = -1;
  std::string line;
};

class Room {
 public:
  explicit Room(uint64_t seed, int max_rounds = md::kDefaultRounds);

  // 一条来自 conn 的消息。产生的消息追加到 out。
  void on_message(int conn, std::string_view line, std::vector<Delivery>& out);

  // 四个座位全用 AI，直接开局（观战 / 冒烟测试用）
  void start_ai_only(std::vector<Delivery>& out);

  // conn 掉线。对局进行中时该座位转交给 AI，避免整局卡死。
  void on_disconnect(int conn, std::vector<Delivery>& out);

  int seat_of_conn(int conn) const;
  bool started() const { return session_ != nullptr; }
  bool over() const;
  bool seat_is_ai(int seat) const { return seats_[seat].occupied && seats_[seat].ai; }

  // 当前该谁行动、有哪些合法选项（供测试与观战使用）
  struct TurnInfo {
    int seat = -1;
    std::string kind;  // "gather" / "gun" / "play" / ""（无人行动）
    std::vector<int> cards;
    std::vector<int> targets;
  };
  TurnInfo turn() const;

  const md::MatchResult& result() const;

 private:
  void start_game(std::vector<Delivery>& out);
  void fill_with_ai();
  void pump_ai();
  void broadcast(std::vector<Delivery>& out);
  void dispatch_cmd(int conn, std::string_view line, std::vector<Delivery>& out);
  void send(int seat, const std::string& line, std::vector<Delivery>& out);

  uint64_t seed_;
  int max_rounds_;
  md::Rng ai_rng_;

  bool announced_over_ = false;
  std::unique_ptr<md::Session> session_;

  struct Seat {
    int conn = -1;  // -1 表示没有真人
    bool ai = false;
    bool occupied = false;
  };
  Seat seats_[md::kSeats];
};

}  // namespace md::net
