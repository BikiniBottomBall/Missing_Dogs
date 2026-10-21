#include "md/core.hpp"

#include <algorithm>
#include <cmath>

namespace md {

const char* cat_name(Cat c) {
  switch (c) {
    case Cat::Inv: return "调查";
    case Cat::Cover: return "掩护";
    case Cat::Rem: return "追忆";
    case Cat::Emp: return "共感";
    case Cat::Harm: return "杀伤";
    case Cat::Dec: return "欺骗";
  }
  return "?";
}

const char* ident_name(Ident i) {
  switch (i) {
    case Ident::Hunter: return "追捕者";
    case Ident::Witness: return "目击者";
    case Ident::Rescuer: return "救援者";
    case Ident::Mole: return "卧底";
  }
  return "?";
}

// ------------------------------------------------------------------ Rng
static uint64_t splitmix64(uint64_t& x) {
  uint64_t z = (x += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

Rng::Rng(uint64_t seed) { seed_with(seed); }

void Rng::seed_with(uint64_t seed) {
  uint64_t x = seed;
  for (auto& v : s) v = splitmix64(x);
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

uint64_t Rng::next() {
  const uint64_t result = rotl(s[1] * 5, 7) * 9;
  const uint64_t t = s[1] << 17;
  s[2] ^= s[0];
  s[3] ^= s[1];
  s[1] ^= s[2];
  s[0] ^= s[3];
  s[2] ^= t;
  s[3] = rotl(s[3], 45);
  return result;
}

double Rng::unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

int Rng::below(int n) {
  if (n <= 1) return 0;
  return static_cast<int>(unit() * static_cast<double>(n));
}

int Rng::pick(const std::array<double, kCats>& w) {
  double total = 0.0;
  for (double x : w) total += x;
  double r = unit() * total;
  double acc = 0.0;
  for (int i = 0; i < kCats; ++i) {
    acc += w[i];
    if (r < acc) return i;
  }
  return kCats - 1;
}

// ------------------------------------------------------------------ 牌池
// 与 docs/design-validation.md 的权重表一致（未归一化）。
// 前三位是三个好人子身份；第四位是卧底。
std::array<std::array<double, kCats>, kIdents> identity_weights() {
  return {{
      // Inv Cover Rem Emp Harm Dec
      {{36, 14, 16, 18, 8, 10}},  // 追捕者
      {{18, 32, 16, 18, 8, 10}},  // 目击者
      {{12, 16, 38, 18, 8, 10}},  // 救援者
      {{12, 12, 15, 6, 28, 22}},  // 卧底
  }};
}

// 通用牌的分布：与身份无关，因此不泄露任何信息。
std::array<double, kCats> utility_weights() { return {{20, 25, 20, 10, 10, 15}}; }

static std::array<double, kCats> normalized(const std::array<double, kCats>& w) {
  double t = 0.0;
  for (double x : w) t += x;
  std::array<double, kCats> out{};
  for (int i = 0; i < kCats; ++i) out[i] = w[i] / t;
  return out;
}

static const std::array<std::array<double, kCats>, kIdents>& pools_normalized() {
  static const auto v = [] {
    std::array<std::array<double, kCats>, kIdents> out{};
    auto raw = identity_weights();
    for (int i = 0; i < kIdents; ++i) out[i] = normalized(raw[i]);
    return out;
  }();
  return v;
}

static const std::array<double, kCats>& utility_normalized() {
  static const auto v = normalized(utility_weights());
  return v;
}

// ------------------------------------------------------------------ Session
Session::Session(uint64_t seed, int max_rounds) : rng_(seed), max_rounds_(max_rounds) {}

void Session::setup() {
  // 身份随机分配：1 个卧底 + 3 个不同子身份的好人
  std::array<Ident, kSeats> ids = {Ident::Hunter, Ident::Witness, Ident::Rescuer, Ident::Mole};
  for (int i = kSeats - 1; i > 0; --i) {
    int j = rng_.below(i + 1);
    std::swap(ids[i], ids[j]);
  }
  for (int s = 0; s < kSeats; ++s) {
    players_[s] = P{};
    players_[s].id = ids[s];
    players_[s].memory = 4;  // 开局 4 点（实验 C 的可用来）
    players_[s].alive = true;
  }

  // 开局 5 张 = 3 通用 + 2 印记
  for (int s = 0; s < kSeats; ++s) {
    draw_to(s, 3, 0.0);
    draw_to(s, 2, 1.0);
  }

  round_ = 0;
  enter_gather();
}

void Session::draw_to(int seat, int n, double imprint_chance) {
  auto& p = players_[seat];
  const auto& idp = pools_normalized()[ident_index(p.id)];
  const auto& util = utility_normalized();
  for (int k = 0; k < n; ++k) {
    if (static_cast<int>(p.hand.size()) >= kMaxHand) return;
    const bool imprint = rng_.unit() < imprint_chance;
    const int c = rng_.pick(imprint ? idp : util);
    p.hand.push_back(Card{static_cast<Cat>(c), imprint});
  }
}

void Session::enter_gather() {
  phase_ = Phase::Gather;
  gather_count_ = 0;
  for (auto& p : players_) p.gather = -1;
}

Pending Session::pending() const {
  Pending p;
  if (over_) return p;
  switch (phase_) {
    case Phase::Gather:
      p.kind = DecisionKind::Gather;
      p.seat = gather_count_;
      return p;
    case Phase::Gun: {
      if (gun_holder_ >= 0 && bullets_ > 0 && players_[gun_holder_].alive &&
          !players_[gun_holder_].shot_this_round) {
        p.kind = DecisionKind::Gun;
        p.seat = gun_holder_;
        for (int s = 0; s < kSeats; ++s)
          if (s != gun_holder_ && players_[s].alive) p.targets.push_back(s);
      }
      return p;
    }
    case Phase::Play: {
      if (play_idx_ < static_cast<int>(play_order_.size())) {
        const int s = play_order_[play_idx_];
        p.kind = DecisionKind::Play;
        p.seat = s;
        for (int i = 0; i < static_cast<int>(players_[s].hand.size()); ++i) p.cards.push_back(i);
        for (int t = 0; t < kSeats; ++t)
          if (t != s && players_[t].alive) p.targets.push_back(t);
      }
      return p;
    }
    default:
      return p;
  }
}

bool Session::submit(const Command& cmd) {
  if (over_) return false;
  if (cmd.kind != pending().kind) return false;

  switch (cmd.kind) {
    case DecisionKind::Gather: {
      if (cmd.seat != gather_count_) return false;
      if (cmd.a < 0 || cmd.a > 2) return false;
      players_[cmd.seat].gather = cmd.a;
      ++gather_count_;
      if (gather_count_ >= kSeats) resolve_gather();
      break;
    }
    case DecisionKind::Gun: {
      if (cmd.seat != gun_holder_) return false;
      if (cmd.a >= 0) {
        if (!players_[cmd.a].alive || cmd.a == cmd.seat) return false;
        apply_shoot(cmd.seat, cmd.a);
      }
      gun_asked_ = true;
      break;
    }
    case DecisionKind::Play: {
      const int s = play_order_[play_idx_];
      if (cmd.seat != s) return false;
      if (cmd.a < 0) {  // 弃权
        ++play_idx_;
        break;
      }
      if (cmd.a >= static_cast<int>(players_[s].hand.size())) return false;
      const Cat c = players_[s].hand[cmd.a].cat;
      const bool needs_target = (c == Cat::Inv || c == Cat::Emp || c == Cat::Harm || c == Cat::Dec);
      if (needs_target) {
        if (cmd.b < 0 || cmd.b >= kSeats || cmd.b == s || !players_[cmd.b].alive) return false;
      }
      apply_play(s, cmd.a, needs_target ? cmd.b : -1);
      ++play_idx_;
      break;
    }
    default:
      return false;
  }
  advance();
  return true;
}

void Session::apply_play(int seat, int card_index, int target) {
  auto& me = players_[seat];
  const Card card = me.hand[card_index];
  me.hand.erase(me.hand.begin() + card_index);
  history_.push_back(Revealed{seat, round_, card.cat});

  switch (card.cat) {
    case Cat::Inv:
      me.memory += 1;
      if (target >= 0 && !players_[target].hand.empty()) {
        const int i = rng_.below(static_cast<int>(players_[target].hand.size()));
        me.peeked.push_back(players_[target].hand[i]);
      }
      break;
    case Cat::Cover:
      me.memory += 1;
      me.shield += 1;
      break;
    case Cat::Rem:
      me.memory += 2;
      draw_to(seat, 1, 0.5);
      break;
    case Cat::Emp:
      me.memory += 1;
      if (target >= 0) players_[target].memory += 1;
      break;
    case Cat::Harm:
      if (target >= 0) {
        auto& t = players_[target];
        if (t.shield > 0) t.shield -= 1; else t.memory -= 2;
      }
      break;
    case Cat::Dec:
      me.memory += 1;
      if (target >= 0) {
        auto& t = players_[target];
        if (t.shield > 0) t.shield -= 1; else t.memory -= 1;
      }
      break;
  }
  for (auto& p : players_) p.memory = std::min(p.memory, kMemoryCap);
}

void Session::apply_shoot(int seat, int target) {
  --bullets_;
  players_[seat].shot_this_round = true;
  players_[seat].memory += 3;  // 死者的记忆涌入开枪者
  players_[seat].memory = std::min(players_[seat].memory, kMemoryCap);
  ++result_.shots;
  if (is_mole_ident(players_[target].id)) ++result_.shots_hit_mole;
  else ++result_.shots_hit_good;
  kill(target, true);
}

void Session::kill(int seat, bool by_gun) {
  if (!players_[seat].alive) return;
  players_[seat].alive = false;
  if (by_gun) ++result_.gun_deaths; else ++result_.mem_deaths;
}

void Session::resolve_gather() {
  // 抢：抢的人里随机一人拿到枪
  std::vector<int> grabbers;
  for (int s = 0; s < kSeats; ++s)
    if (players_[s].gather == 0) grabbers.push_back(s);

  if (grabbers.empty()) {  // 没人抢 -> 随机落到一个人手里，保证每局必有枪
    gun_holder_ = rng_.below(kSeats);
  } else {
    gun_holder_ = grabbers[rng_.below(static_cast<int>(grabbers.size()))];
  }
  bullets_ = 1;
  result_.gun_holder_is_mole = is_mole_ident(players_[gun_holder_].id);

  for (int s = 0; s < kSeats; ++s) {
    if (players_[s].gather == 1) {
      players_[s].memory = std::min(players_[s].memory + 1, kMemoryCap);
    } else if (players_[s].gather == 2) {
      if (!players_[gun_holder_].hand.empty()) {
        const int i = rng_.below(static_cast<int>(players_[gun_holder_].hand.size()));
        players_[s].peeked.push_back(players_[gun_holder_].hand[i]);
      }
    }
  }

  round_ = 1;
  for (auto& p : players_) p.shot_this_round = false;
  gun_asked_ = false;
  phase_ = Phase::Gun;
}

void Session::enter_play() {
  play_order_.clear();
  int start = gun_holder_ >= 0 ? gun_holder_ : 0;
  for (int k = 0; k < kSeats; ++k) {
    const int s = (start + k) % kSeats;
    if (players_[s].alive && !players_[s].shot_this_round) play_order_.push_back(s);
  }
  play_idx_ = 0;
  phase_ = Phase::Play;
}

void Session::refresh_unlocks() {
  for (int s = 0; s < kSeats; ++s) {
    auto& p = players_[s];
    if (!p.alive) continue;
    if (!p.faction_known && p.memory >= faction_unlock_) {
      p.faction_known = true;
      if (result_.first_faction_round < 0) result_.first_faction_round = round_;
    }
    if (!p.identity_known && p.memory >= identity_unlock_) p.identity_known = true;
  }
}

void Session::finish_round() {
  // 失忆会退：每轮结束全体 -1
  for (auto& p : players_)
    if (p.alive) p.memory -= 1;

  for (int s = 0; s < kSeats; ++s)
    if (players_[s].alive && players_[s].memory <= 0) kill(s, false);

  refresh_unlocks();
  check_end();
}

void Session::check_end() {
  bool mole_alive = false;
  int alive_count = 0;
  for (int s = 0; s < kSeats; ++s) {
    if (players_[s].alive) {
      ++alive_count;
      if (is_mole_ident(players_[s].id)) mole_alive = true;
    }
  }
  result_.mole_alive = mole_alive;

  auto finish = [&](Winner w) {
    result_.winner = w;
    result_.rounds = round_;
    over_ = true;
    phase_ = Phase::Done;
  };

  if (!mole_alive) { finish(Winner::Good); return; }
  if (alive_count <= 2) { finish(Winner::Mole); return; }
  if (round_ >= max_rounds_) { finish(Winner::Mole); return; }
}

int Session::next_play_seat() const {
  if (play_idx_ < static_cast<int>(play_order_.size())) return play_order_[play_idx_];
  return -1;
}

void Session::advance() {
  for (;;) {
    if (over_) return;
    switch (phase_) {
      case Phase::Gather:
        return;  // 等待剩余玩家选择
      case Phase::Gun: {
        // 枪不能用（没子弹 / 持枪者已死 / 本轮已开过）就直接进入出牌阶段。
        // 少了这一句，第一枪打完后状态会卡在 Gun 阶段，整局无法结束。
        const bool can_act = gun_holder_ >= 0 && bullets_ > 0 && players_[gun_holder_].alive &&
                             !players_[gun_holder_].shot_this_round;
        if (gun_asked_ || !can_act) {
          enter_play();
          continue;
        }
        return;
      }
      case Phase::Play:
        if (play_idx_ < static_cast<int>(play_order_.size())) return;
        phase_ = Phase::EndOfRound;
        continue;
      case Phase::EndOfRound: {
        finish_round();
        if (over_) return;
        ++round_;
        if (round_ > max_rounds_) { check_end(); return; }
        // 新一轮：每人抽 1 张（50% 是印记牌）
        for (int s = 0; s < kSeats; ++s)
          if (players_[s].alive) draw_to(s, 1, 0.5);
        for (auto& p : players_) p.shot_this_round = false;
        gun_asked_ = false;
        phase_ = Phase::Gun;
        continue;
      }
      case Phase::Done:
        return;
    }
  }
}

PlayerView Session::view_of(int seat) const {
  PlayerView v;
  v.seat = seat;
  v.round = round_;
  const auto& me = players_[seat];
  v.memory = me.memory;
  v.alive = me.alive;
  v.hand = me.hand;
  v.faction_known = me.faction_known;
  v.faction_is_mole = me.faction_known && is_mole_ident(me.id);
  v.identity_known = me.identity_known;
  v.identity = me.identity_known ? me.id : Ident::Hunter;
  v.gun_holder = gun_holder_;
  v.bullets = bullets_;
  v.history = history_;
  v.peeked = me.peeked;
  for (int s = 0; s < kSeats; ++s) {
    v.pub_memory[s] = players_[s].memory;
    v.pub_alive[s] = players_[s].alive;
    v.pub_hand[s] = static_cast<int>(players_[s].hand.size());
  }
  return v;
}

void Session::note_mole_aware(int r) {
  if (result_.mole_self_knew_round < 0) result_.mole_self_knew_round = r;
}

void Session::set_unlocks(int faction, int identity) {
  faction_unlock_ = faction;
  identity_unlock_ = identity;
}

}  // namespace md
