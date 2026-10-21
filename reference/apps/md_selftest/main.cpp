// 无网络的端到端自检：直接驱动 md::net::Room，验证协议与隐藏信息裁剪。
//
// 之所以不连 socket，是因为这台机器上任何链接了 ws2_32 的可执行文件都会被
// Application Control 拦下。把 Room 与 socket 解耦之后，真正会出错的部分
// （座位、回合、视角裁剪、掉线接管）仍然可以被完整验证。
#include <cstdio>
#include <string>
#include <vector>

#include "md/net/room.hpp"
#include "md/net/wire.hpp"

namespace {

int g_fail = 0;
int g_pass = 0;

void check(bool ok, const char* what) {
  if (ok) {
    ++g_pass;
    std::printf("  [ok]   %s\n", what);
  } else {
    ++g_fail;
    std::printf("  [FAIL] %s\n", what);
  }
}

int count_sub(const std::string& s, const std::string& sub) {
  int n = 0;
  for (std::size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size())) ++n;
  return n;
}

long find_int(const std::string& s, const std::string& key) {
  const std::string pat = "\"" + key + "\":";
  std::size_t p = s.find(pat);
  if (p == std::string::npos) return -1;
  p += pat.size();
  bool neg = false;
  if (p < s.size() && s[p] == '-') { neg = true; ++p; }
  long acc = 0;
  bool any = false;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
    acc = acc * 10 + (s[p] - '0');
    ++p;
    any = true;
  }
  if (!any) return -1;
  return neg ? -acc : acc;
}

std::vector<int> find_int_array(const std::string& s, const std::string& key) {
  std::vector<int> out;
  const std::string pat = "\"" + key + "\":[";
  std::size_t p = s.find(pat);
  if (p == std::string::npos) return out;
  p += pat.size();
  while (p < s.size() && s[p] != ']') {
    if (s[p] == '-' || (s[p] >= '0' && s[p] <= '9')) {
      bool neg = false;
      if (s[p] == '-') { neg = true; ++p; }
      long acc = 0;
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') { acc = acc * 10 + (s[p] - '0'); ++p; }
      out.push_back(static_cast<int>(neg ? -acc : acc));
    } else {
      ++p;
    }
  }
  return out;
}

bool contains(const std::string& s, const std::string& sub) {
  return s.find(sub) != std::string::npos;
}

struct Harness {
  md::net::Room room;
  std::vector<std::vector<std::string>> inbox;
  std::vector<std::string> all_views;  // 每一份推送出去的 view，用来做泄露检查

  explicit Harness(uint64_t seed, int rounds = 8) : room(seed, rounds), inbox(16) {}

  void route(const std::vector<md::net::Delivery>& out) {
    for (const auto& d : out) {
      if (d.conn >= 0 && d.conn < static_cast<int>(inbox.size())) inbox[d.conn].push_back(d.line);
      if (contains(d.line, "\"t\":\"view\"")) all_views.push_back(d.line);
    }
  }

  void send(int conn, const std::string& line) {
    std::vector<md::net::Delivery> out;
    room.on_message(conn, line, out);
    route(out);
  }

  void disconnect(int conn) {
    std::vector<md::net::Delivery> out;
    room.on_disconnect(conn, out);
    route(out);
  }

  std::string last(int conn) const {
    if (inbox[conn].empty()) return {};
    return inbox[conn].back();
  }
};

// 从 Room 的 turn() 造一条合法的 JSON 命令
std::string pick_cmd(const md::net::Room::TurnInfo& t) {
  std::string c = "{\"t\":\"cmd\",\"k\":\"" + t.kind + "\"";
  if (t.kind == "gather") {
    c += ",\"a\":0";  // 一律抢枪，够用了
  } else if (t.kind == "gun") {
    c += ",\"a\":" + std::to_string(t.targets.empty() ? -1 : t.targets.front());
  } else if (t.kind == "play") {
    if (t.cards.empty()) {
      c += ",\"a\":-1";
    } else {
      c += ",\"a\":" + std::to_string(t.cards.back());
      if (!t.targets.empty()) c += ",\"b\":" + std::to_string(t.targets.front());
    }
  } else {
    c += ",\"a\":-1";
  }
  c += "}";
  return c;
}

// ------------------------------------------------------------------ 用例

void test_ai_only() {
  std::printf("\n[1] 四个 AI 直接开局，跑完整一局\n");
  Harness h(7);
  std::vector<md::net::Delivery> out;
  h.room.start_ai_only(out);
  h.route(out);
  check(h.room.over(), "对局在 AI 驱动下自行结束");
  const md::MatchResult& r = h.room.result();
  check(r.winner != md::Winner::None, "产生了胜者");
  check(r.rounds >= 1 && r.rounds <= 8, "轮数在 1..8 之间");
  check(h.all_views.empty(), "没有真人座位时不产生任何投递");
}

void test_single_human_full_game() {
  std::printf("\n[2] 一个真人 + 三个 AI，走完整局\n");
  Harness h(11);
  h.send(0, "{\"t\":\"join\"}");
  check(h.room.seat_of_conn(0) == 0, "join 拿到座位 0");
  check(contains(h.last(0), "\"t\":\"joined\""), "收到 joined");
  check(!h.room.started(), "满座前不开局");

  h.send(0, "{\"t\":\"start\"}");
  check(h.room.started(), "start 之后开局");
  check(contains(h.last(0), "\"t\":\"view\""), "开局后推送了 view");

  int steps = 0;
  while (!h.room.over() && steps++ < 400) {
    const auto t = h.room.turn();
    if (t.seat != 0) break;
    h.send(0, pick_cmd(t));
  }
  check(h.room.over(), "对局结束");
  check(steps > 0 && steps <= 400, "没有死循环");
  check(contains(h.last(0), "\"t\":\"over\""), "收到 over 消息");
}

void test_hidden_information() {
  std::printf("\n[3] 隐藏信息：每个人只能看到自己的手牌\n");
  Harness h(23);
  h.send(0, "{\"t\":\"join\"}");   // seat 0
  h.send(1, "{\"t\":\"join\"}");   // seat 1
  check(h.room.seat_of_conn(1) == 1, "第二个连接拿到座位 1");
  h.send(0, "{\"t\":\"start\"}");
  check(h.room.started(), "两人就座、其余补 AI 后开局");

  int steps = 0;
  while (!h.room.over() && steps++ < 400) {
    const auto t = h.room.turn();
    if (t.seat == 0) h.send(0, pick_cmd(t));
    else if (t.seat == 1) h.send(1, pick_cmd(t));
    else break;
  }

  check(!h.all_views.empty(), "确实产生了 view 推送");
  bool leak = false;
  int checked = 0;
  for (const std::string& v : h.all_views) {
    const long seat = find_int(v, "seat");
    if (seat < 0) continue;
    const auto pub_hand = find_int_array(v, "pub_hand");
    if (static_cast<long>(pub_hand.size()) != md::kSeats) continue;
    const int imprints = count_sub(v, "\"imp\":");
    if (imprints != pub_hand[static_cast<std::size_t>(seat)]) leak = true;
    ++checked;
  }
  check(checked > 0, "校验了若干份 view");
  check(!leak, "view 里的手牌张数与 pub_hand[自己] 一致（未泄露他人手牌）");
}

void test_rejects() {
  std::printf("\n[4] 非法输入被拒\n");
  Harness h(31);
  h.send(0, "{\"t\":\"join\"}");
  h.send(1, "{\"t\":\"join\"}");
  h.send(0, "{\"t\":\"start\"}");

  // 轮到座位 0 时，座位 1 的连接抢着行动
  const auto t = h.room.turn();
  if (t.seat == 0) {
    const std::size_t before = h.inbox[1].size();
    h.send(1, "{\"t\":\"cmd\",\"k\":\"" + t.kind + "\",\"a\":0}");
    check(h.inbox[1].size() > before && contains(h.last(1), "not your turn"),
          "他人回合行动被拒：not your turn");
  } else {
    check(false, "预期轮到座位 0");
  }

  // 伪造 seat 字段，试图替座位 0 行动
  if (t.seat == 0) {
    h.send(1, "{\"t\":\"cmd\",\"k\":\"" + t.kind + "\",\"a\":0,\"seat\":0}");
    check(contains(h.last(1), "not your turn"), "伪造 seat 字段无效（以连接座位为准）");
  }

  // 乱发命令
  h.send(0, "{\"t\":\"cmd\",\"k\":\"bogus\",\"a\":0}");
  check(contains(h.last(0), "bad command"), "未知 k 被拒：bad command");

  h.send(0, "not json at all");
  check(contains(h.last(0), "bad json"), "非法 JSON 被拒：bad json");

  h.send(0, "{\"t\":\"nonsense\"}");
  check(contains(h.last(0), "unknown message"), "未知消息类型被拒");
}

void test_illegal_move() {
  std::printf("\n[5] 越界的手牌下标被拒\n");
  Harness h(41);
  h.send(0, "{\"t\":\"join\"}");
  h.send(0, "{\"t\":\"start\"}");

  // 一直推进到自己的出牌阶段
  int steps = 0;
  while (!h.room.over() && steps++ < 50) {
    const auto t = h.room.turn();
    if (t.seat != 0) break;
    if (t.kind == "play" && !t.cards.empty()) {
      h.send(0, "{\"t\":\"cmd\",\"k\":\"play\",\"a\":999,\"b\":-1}");
      check(contains(h.last(0), "illegal move"), "越界下标被拒：illegal move");
      return;
    }
    h.send(0, pick_cmd(t));
  }
  check(false, "没能走到出牌阶段");
}

void test_disconnect_takeover() {
  std::printf("\n[6] 掉线座位由 AI 接管，整局不卡死\n");
  Harness h(53);
  h.send(0, "{\"t\":\"join\"}");
  h.send(1, "{\"t\":\"join\"}");
  h.send(0, "{\"t\":\"start\"}");
  h.disconnect(1);
  check(h.room.seat_of_conn(1) == -1, "掉线后座位解绑");
  check(h.room.seat_is_ai(1), "该座位转为 AI");

  int steps = 0;
  while (!h.room.over() && steps++ < 400) {
    const auto t = h.room.turn();
    if (t.seat != 0) break;
    h.send(0, pick_cmd(t));
  }
  check(h.room.over(), "剩下的人仍能打完");
}

}  // namespace

int main() {
  std::printf("Missing_Dogs 端到端自检（无网络）\n");
  std::printf("================================\n");

  test_ai_only();
  test_single_human_full_game();
  test_hidden_information();
  test_rejects();
  test_illegal_move();
  test_disconnect_takeover();

  std::printf("\n================================\n");
  std::printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
