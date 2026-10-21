#include "md/core.hpp"

#include <algorithm>
#include <cmath>

namespace md {

namespace {

// AI 心里那套"世界模型"。它假设对手有 30% 的牌是随手打的效用牌，
// 这个假设并不精确——真人 AI 也会误判，这正是想要的。
struct Model {
  std::array<std::array<double, kCats>, kIdents> pool{};
  std::array<double, kCats> good{};
  std::array<double, kCats> mole{};
  std::array<double, kCats> util{};
  std::array<double, kCats> logLR{};
};

const Model& model() {
  static const Model m = [] {
    Model r;
    auto raw = identity_weights();
    auto util = utility_weights();
    double ut = 0.0;
    for (double x : util) ut += x;
    for (int i = 0; i < kCats; ++i) r.util[i] = util[i] / ut;

    for (int k = 0; k < kIdents; ++k) {
      double t = 0.0;
      for (double x : raw[k]) t += x;
      for (int i = 0; i < kCats; ++i) r.pool[k][i] = raw[k][i] / t;
    }
    for (int i = 0; i < kCats; ++i) {
      double g = 0.0;
      for (int k = 0; k < 3; ++k) g += r.pool[k][i];
      r.good[i] = g / 3.0;
      r.mole[i] = r.pool[3][i];
    }

    const double mix = 0.7;
    for (int i = 0; i < kCats; ++i) {
      const double pg = mix * r.good[i] + (1.0 - mix) * r.util[i];
      const double pm = mix * r.mole[i] + (1.0 - mix) * r.util[i];
      r.logLR[i] = std::log(pm) - std::log(pg);
    }
    return r;
  }();
  return m;
}

}  // namespace

SelfBelief ai_belief(const PlayerView& v) {
  const auto& m = model();
  SelfBelief b;

  // ---- 我是谁
  if (v.faction_known) {
    b.p_mole = v.faction_is_mole ? 1.0 : 0.0;
  } else {
    double L = 0.0;
    for (const auto& c : v.hand)
      if (c.imprint) L += m.logLR[cat_index(c.cat)];
    const double prior = 0.25;
    b.p_mole = 1.0 / (1.0 + std::exp(-(L + std::log(prior / (1.0 - prior)))));
  }

  // ---- 他是谁（只看公开打出的牌）
  std::array<double, kSeats> L{};
  L.fill(0.0);
  for (const auto& r : v.history)
    if (r.seat >= 0 && r.seat < kSeats) L[r.seat] += m.logLR[cat_index(r.cat)];

  double mx = -1e18;
  for (int s = 0; s < kSeats; ++s)
    if (s != v.seat) mx = std::max(mx, L[s]);

  std::array<double, kSeats> w{};
  double sum = 0.0;
  for (int s = 0; s < kSeats; ++s) {
    if (s == v.seat) continue;
    w[s] = std::exp(L[s] - mx);
    sum += w[s];
  }
  for (int s = 0; s < kSeats; ++s) {
    if (s == v.seat) continue;
    b.p_other[s] = (sum > 0.0) ? w[s] / sum : 0.0;
  }
  b.p_other[v.seat] = b.p_mole;
  return b;
}

Command ai_decide(const PlayerView& v, const Pending& p, Rng& rng) {
  Command c;
  c.kind = p.kind;
  c.seat = p.seat;
  const SelfBelief b = ai_belief(v);
  const bool self_mole = b.p_mole > 0.5;

  if (p.kind == DecisionKind::Gather) {
    const double r = rng.unit();
    if (self_mole) {
      c.a = (r < 0.80) ? 0 : (r < 0.92 ? 2 : 1);  // 卧底更想要枪
    } else {
      c.a = (r < 0.50) ? 0 : (r < 0.80 ? 2 : 1);
    }
    return c;
  }

  if (p.kind == DecisionKind::Gun) {
    int best = -1;
    double bestScore = -1e18;
    for (int t : p.targets) {
      double s;
      if (self_mole) {
        s = v.pub_memory[t] + (v.pub_memory[t] >= kIdentityUnlock ? 10.0 : 0.0);
      } else {
        s = b.p_other[t] * 100.0;
      }
      if (s > bestScore) {
        bestScore = s;
        best = t;
      }
    }
    bool shoot = false;
    if (best >= 0) {
      if (self_mole) {
        shoot = (v.pub_memory[best] >= kIdentityUnlock) || (v.round >= 5);
      } else {
        shoot = (b.p_other[best] >= 0.60);
      }
    }
    c.a = shoot ? best : -1;
    return c;
  }

  if (p.kind != DecisionKind::Play) return c;

  int bestIdx = -1;
  int bestTarget = -1;
  double bestVal = -1e18;

  for (int i : p.cards) {
    const Cat cat = v.hand[i].cat;
    const bool needs_target =
        (cat == Cat::Inv || cat == Cat::Emp || cat == Cat::Harm || cat == Cat::Dec);

    int tgt = -1;
    if (cat == Cat::Harm || cat == Cat::Dec || cat == Cat::Inv) {
      double bs = -1e18;
      for (int t : p.targets) {
        const double s = self_mole ? static_cast<double>(v.pub_memory[t]) : b.p_other[t];
        if (s > bs) { bs = s; tgt = t; }
      }
    } else if (cat == Cat::Emp) {
      double bs = 1e18;
      for (int t : p.targets) {
        const double s = self_mole ? static_cast<double>(v.pub_memory[t])
                                   : (v.pub_memory[t] + 30.0 * b.p_other[t]);
        if (s < bs) { bs = s; tgt = t; }
      }
    }
    if (needs_target && tgt < 0) continue;  // 没有合法目标，这张牌打不出去

    const double mem = v.memory;
    double val = 0.0;
    if (!self_mole) {
      // 好人打自己人是灾难，所以只在把握足够大时才动杀心。
      // 早期 p 约 0.33，杀伤牌会被压到追忆/掩护之下。
      switch (cat) {
        case Cat::Rem:   val = 1.30 + (mem < 6 ? 0.50 : 0.0); break;
        case Cat::Cover: val = 0.90 + (mem < 5 ? 0.80 : 0.0); break;
        case Cat::Inv:   val = 0.95; break;
        case Cat::Emp:   val = 1.00; break;
        case Cat::Harm:  val = 0.85 + 7.00 * std::max(0.0, b.p_other[tgt] - 0.42); break;
        case Cat::Dec:   val = 0.80 + 4.00 * std::max(0.0, b.p_other[tgt] - 0.45); break;
      }
    } else {
      switch (cat) {
        case Cat::Rem:   val = 1.20 + (mem < 6 ? 0.40 : 0.0); break;
        case Cat::Cover: val = 1.00 + (mem < 5 ? 0.60 : 0.0); break;
        case Cat::Inv:   val = 0.60; break;
        case Cat::Emp:   val = 1.10; break;
        case Cat::Harm:  val = 1.40 + 2.00 * (v.pub_memory[tgt] / 10.0); break;
        case Cat::Dec:   val = 1.20 + 1.50 * (v.pub_memory[tgt] / 10.0); break;
      }
    }
    val += rng.unit() * 0.05;

    if (val > bestVal) {
      bestVal = val;
      bestIdx = i;
      bestTarget = tgt;
    }
  }

  c.a = bestIdx;
  c.b = bestTarget;
  return c;
}

}  // namespace md
