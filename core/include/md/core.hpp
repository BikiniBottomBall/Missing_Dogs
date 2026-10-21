// Missing_Dogs 规则核心 —— 零依赖，不引用 Godot，不引用 socket。
//
// 设计约束（与 docs/architecture.md 对应）：
//   1. core 只接受 Command，只产出 PlayerView。它不知道网络，也不知道 UI。
//   2. 隐藏信息只存在于 Session 内部；PlayerView 是按座位裁剪后的投影。
//   3. AI 只吃 PlayerView，因此它物理上无法作弊——同一份 AI 可以直接拿去联机补位。
//   4. 所有随机数走可种子的 Rng，因此对局可复现、可用于 AI 搜索与回放。
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace md {

constexpr int kSeats = 4;
constexpr int kCats = 6;
constexpr int kIdents = 4;
constexpr int kMaxHand = 7;
constexpr int kMemoryCap = 10;
constexpr int kFactionUnlock = 7;   // 记忆达到 7 -> 知道自己是好是坏
constexpr int kIdentityUnlock = 9;  // 记忆达到 9 -> 知道自己是哪一个
constexpr int kDefaultRounds = 8;

// 牌类别。前三个是"子身份轴"（区分你是哪种好人），
// 后三个是"阵营轴"（区分好人 / 卧底）。见 docs/design-validation.md。
enum class Cat : uint8_t { Inv = 0, Cover, Rem, Emp, Harm, Dec };
constexpr std::array<Cat, kCats> kAllCats = {Cat::Inv, Cat::Cover, Cat::Rem,
                                             Cat::Emp, Cat::Harm, Cat::Dec};

// 身份。0/1/2 是三种好人，3 是卧底。
enum class Ident : uint8_t { Hunter = 0, Witness, Rescuer, Mole };

inline bool is_mole_ident(Ident i) { return i == Ident::Mole; }
inline int cat_index(Cat c) { return static_cast<int>(c); }
inline int ident_index(Ident i) { return static_cast<int>(i); }
const char* cat_name(Cat c);
const char* ident_name(Ident i);

struct Card {
  Cat cat = Cat::Inv;
  bool imprint = false;  // 印记牌：携带身份信号。对手看不出是不是印记。
};

// xoshiro256**，可种子化，用于保证对局可复现。
struct Rng {
  std::array<uint64_t, 4> s{};
  explicit Rng(uint64_t seed = 0x9E3779B97F4A7C15ull);
  void seed_with(uint64_t seed);
  uint64_t next();
  double unit();                                // [0,1)
  int below(int n);                             // [0,n)
  int pick(const std::array<double, kCats>& w);  // 按权重抽类别
};

struct Revealed {
  int seat = -1;
  int round = 0;
  Cat cat = Cat::Inv;
};

// 玩家视角。这是 AI 和客户端能看到的全部世界。
struct PlayerView {
  int seat = -1;
  int round = 0;
  int memory = 0;
  bool alive = true;
  std::vector<Card> hand;

  // 记忆解锁的知识
  bool faction_known = false;
  bool faction_is_mole = false;
  bool identity_known = false;
  Ident identity = Ident::Hunter;

  // 公共信息
  std::array<int, kSeats> pub_memory{};
  std::array<bool, kSeats> pub_alive{};
  std::array<int, kSeats> pub_hand{};
  int gun_holder = -1;
  int bullets = 0;
  std::vector<Revealed> history;  // 所有人打出的牌（只有类别，看不出是不是印记）

  // 私有信息：调查牌换来的偷看
  std::vector<Card> peeked;
};

enum class DecisionKind : uint8_t { Gather, Gun, Play, Over };

struct Pending {
  DecisionKind kind = DecisionKind::Over;
  int seat = -1;
  std::vector<int> cards;    // Play 阶段合法的手牌下标
  std::vector<int> targets;  // 合法目标座位
};

struct Command {
  DecisionKind kind = DecisionKind::Over;
  int seat = -1;
  // Gather: a = 0 抢 / 1 躲 / 2 盯
  // Gun:    a = 目标座位，或 -1 表示不开枪
  // Play:   a = 手牌下标，b = 目标座位（无目标牌填 -1）；a = -1 表示弃权
  int a = -1;
  int b = -1;
};

enum class Winner : uint8_t { None = 0, Good, Mole };

struct MatchResult {
  Winner winner = Winner::None;
  int rounds = 0;
  int mem_deaths = 0;
  int gun_deaths = 0;
  bool mole_alive = false;
  int goods_unlocked = 0;        // 好人中"完整知道身份"的人数
  int shots = 0;                 // 总开枪次数
  int shots_hit_mole = 0;        // 打死了卧底
  int shots_hit_good = 0;        // 打死了好人
  bool gun_holder_is_mole = false;
  int mole_self_knew_round = -1; // 卧底自己察觉到"我是卧底"的轮次
  int first_faction_round = -1;  // 全场第一次有人解锁阵营的轮次
};

class Session {
 public:
  explicit Session(uint64_t seed, int max_rounds = kDefaultRounds);

  void setup();

  Pending pending() const;
  bool submit(const Command& cmd);  // 校验通过返回 true

  PlayerView view_of(int seat) const;
  bool over() const { return over_; }
  const MatchResult& result() const { return result_; }

  // 供模拟器 / 服务器查询
  int seat_count() const { return kSeats; }
  int round() const { return round_; }
  int gun_holder() const { return gun_holder_; }
  int bullets() const { return bullets_; }
  int memory_of(int s) const { return players_[s].memory; }
  bool alive_of(int s) const { return players_[s].alive; }
  Ident ident_of(int s) const { return players_[s].id; }
  void note_mole_aware(int round);  // 由 AI/模拟器回报"卧底察觉了"
  // 解锁阈值可调——它们和开局记忆值强耦合，必须一起调参（见 docs/architecture.md）
  void set_unlocks(int faction, int identity);

 private:
  struct P {
    Ident id = Ident::Hunter;
    int memory = 4;
    bool alive = true;
    std::vector<Card> hand;
    int shield = 0;
    bool faction_known = false;
    bool identity_known = false;
    int gather = -1;
    bool shot_this_round = false;
    std::vector<Card> peeked;
  };

  void draw_to(int seat, int n, double imprint_chance);
  void advance();
  void enter_gather();
  void resolve_gather();
  void enter_play();
  void finish_round();
  void check_end();
  void kill(int seat, bool by_gun);
  void apply_play(int seat, int card_index, int target);
  void apply_shoot(int seat, int target);
  void refresh_unlocks();
  int next_play_seat() const;

  Rng rng_;
  std::array<P, kSeats> players_{};
  std::vector<Revealed> history_;
  int round_ = 0;
  int max_rounds_ = kDefaultRounds;
  int faction_unlock_ = kFactionUnlock;
  int identity_unlock_ = kIdentityUnlock;
  int gun_holder_ = -1;
  int bullets_ = 0;
  bool gun_asked_ = false;
  bool over_ = false;

  enum class Phase { Gather, Gun, Play, EndOfRound, Done };
  Phase phase_ = Phase::Gather;
  int gather_count_ = 0;
  int play_idx_ = 0;
  std::vector<int> play_order_;
  MatchResult result_{};
};

// ---------------------------------------------------------------- AI
//
// AI 只接收 PlayerView。这是硬约束：它看不到真实身份，也看不到别人的手牌，
// 所以它和人类玩家处在完全相同的信息条件下。

struct SelfBelief {
  double p_mole = 0.25;              // "我是卧底"的后验
  std::array<double, kSeats> p_other{};  // 每个座位是卧底的后验
};

SelfBelief ai_belief(const PlayerView& v);
Command ai_decide(const PlayerView& v, const Pending& p, Rng& rng);

// 调试 / 模拟用：全部牌池权重（未归一化）
std::array<std::array<double, kCats>, kIdents> identity_weights();
std::array<double, kCats> utility_weights();

}  // namespace md
