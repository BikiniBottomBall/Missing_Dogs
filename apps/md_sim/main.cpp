// 批量对局模拟器：用 AI 把规则跑几千局，拿到胜率基线。
//
// 用法：
//   md_sim [局数] [种子基数] [最大轮数] [--trace]
#include "md/core.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace md;

namespace {

struct Stats {
  int games = 0;
  int good = 0, mole = 0, unfinished = 0;
  long long rounds = 0;
  long long mem_deaths = 0, gun_deaths = 0;
  long long shots = 0, hit_mole = 0, hit_good = 0, gun_mole_holder = 0;
  long long goods_unlocked = 0;
  long long goods_total = 0;
  std::vector<int> round_hist;
  std::vector<int> aware_hist;  // 下标 = 轮次，0 = 到终局都没察觉
  long long aware_rounds_sum = 0;
  int aware_n = 0;
  long long first_faction_sum = 0;
  int first_faction_n = 0;
  int first_faction_none = 0;
};

struct RunOutcome {
  MatchResult r;
  bool mole_is_seat[kSeats]{};
};

MatchResult run_game(uint64_t seed, Rng& airng, int max_rounds, int facet, int ident,
                     Stats& st, bool trace) {
  Session s(seed, max_rounds);
  s.set_unlocks(facet, ident);
  s.setup();

  int guard = 0;
  while (!s.over()) {
    if (++guard > 4000) break;
    Pending p = s.pending();
    if (p.kind == DecisionKind::Over) break;

    PlayerView v = s.view_of(p.seat);

    // 记录卧底第一次"怀疑自己是卧底"的轮次（AI 自己的信念，不含真值）
    if (is_mole_ident(s.ident_of(p.seat))) {
      const SelfBelief b = ai_belief(v);
      if (b.p_mole > 0.5) s.note_mole_aware(s.round());
    }

    const Command c = ai_decide(v, p, airng);
    if (!s.submit(c)) {
      Command fb;
      fb.kind = p.kind;
      fb.seat = p.seat;
      fb.a = (p.kind == DecisionKind::Play || p.kind == DecisionKind::Gun) ? -1 : 1;
      if (!s.submit(fb)) break;
    }

    if (trace) {
      const char* what = "?";
      switch (c.kind) {
        case DecisionKind::Gather: what = "抢枪"; break;
        case DecisionKind::Gun: what = c.a >= 0 ? "开枪" : "收枪"; break;
        case DecisionKind::Play: what = c.a >= 0 ? cat_name(v.hand[c.a].cat) : "弃权"; break;
        default: what = "-"; break;
      }
      std::printf("  r%d 座位%d(%s,记忆%d) -> %s", s.round(), p.seat,
                  ident_name(s.ident_of(p.seat)), v.memory, what);
      if (c.kind == DecisionKind::Play && c.b >= 0) std::printf(" 目标座位%d", c.b);
      if (c.kind == DecisionKind::Gun && c.a >= 0) std::printf(" 目标座位%d", c.a);
      std::printf("\n");
    }
  }

  // 统计好人"完整想起自己身份"的人数
  int unlocked = 0;
  for (int i = 0; i < kSeats; ++i) {
    if (!is_mole_ident(s.ident_of(i)) && s.view_of(i).identity_known) ++unlocked;
  }
  st.goods_unlocked += unlocked;
  st.goods_total += 3;

  const MatchResult r = s.result();
  st.rounds += r.rounds;
  st.mem_deaths += r.mem_deaths;
  st.gun_deaths += r.gun_deaths;
  st.shots += r.shots;
  st.hit_mole += r.shots_hit_mole;
  st.hit_good += r.shots_hit_good;
  if (r.gun_holder_is_mole) ++st.gun_mole_holder;

  if (r.first_faction_round < 0) ++st.first_faction_none;
  else {
    st.first_faction_sum += r.first_faction_round;
    ++st.first_faction_n;
  }

  const int aware = r.mole_self_knew_round < 0 ? 0 : r.mole_self_knew_round;
  if (aware > 0 && aware < static_cast<int>(st.aware_hist.size())) {
    ++st.aware_hist[aware];
    st.aware_rounds_sum += aware;
    ++st.aware_n;
  } else {
    ++st.aware_hist[0];
  }

  if (r.rounds >= 0 && r.rounds < static_cast<int>(st.round_hist.size())) ++st.round_hist[r.rounds];
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  int games = 200;
  uint64_t seed_base = 1;
  int max_rounds = kDefaultRounds;
  int facet = kFactionUnlock;
  int ident = kIdentityUnlock;
  bool trace = false;
  bool csv = false;

  int pos = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--trace") == 0) { trace = true; continue; }
    if (std::strcmp(argv[i], "--csv") == 0) { csv = true; continue; }
    const long v = std::atol(argv[i]);
    if (pos == 0) games = static_cast<int>(v);
    else if (pos == 1) seed_base = static_cast<uint64_t>(v);
    else if (pos == 2) max_rounds = static_cast<int>(v);
    else if (pos == 3) facet = static_cast<int>(v);
    else if (pos == 4) ident = static_cast<int>(v);
    ++pos;
  }
  if (games <= 0) games = 200;

  Stats st;
  st.round_hist.assign(max_rounds + 2, 0);
  st.aware_hist.assign(max_rounds + 2, 0);

  Rng airng(seed_base * 0x9E3779B97F4A7C15ull + 0x1234);

  for (int g = 0; g < games; ++g) {
    const uint64_t seed = seed_base + static_cast<uint64_t>(g) * 7919ull;
    const MatchResult r = run_game(seed, airng, max_rounds, facet, ident, st, trace);
    ++st.games;
    switch (r.winner) {
      case Winner::Good: ++st.good; break;
      case Winner::Mole: ++st.mole; break;
      default: ++st.unfinished; break;
    }
  }

  const double n = static_cast<double>(st.games);

  if (csv) {
    std::printf("%d,%d,%.1f,%.1f,%.2f,%.2f,%.1f,%.1f\n", facet, ident,
                st.good / n * 100.0, st.mole / n * 100.0, st.rounds / n,
                st.goods_unlocked / n,
                st.aware_hist[0] / n * 100.0,
                st.shots > 0 ? st.hit_mole / double(st.shots) * 100.0 : 0.0);
    return 0;
  }

  std::printf("\n");
  std::printf("Missing_Dogs 规则核心 —— 自动对局基线\n");
  std::printf("局数 %d | 最大轮数 %d | 阵营解锁 %d | 身份解锁 %d | 种子基数 %llu\n", games,
              max_rounds, facet, ident, static_cast<unsigned long long>(seed_base));
  std::printf("\n【胜率】\n");
  std::printf("  好人胜   %5.1f%%  (%d)\n", st.good / n * 100.0, st.good);
  std::printf("  卧底胜   %5.1f%%  (%d)\n", st.mole / n * 100.0, st.mole);
  if (st.unfinished) std::printf("  未分胜负 %5.1f%%  (%d)\n", st.unfinished / n * 100.0, st.unfinished);

  std::printf("\n【对局长度】\n");
  std::printf("  平均轮数 %.2f\n", st.rounds / n);
  std::printf("  轮数分布 ");
  for (int r = 1; r < static_cast<int>(st.round_hist.size()); ++r)
    if (st.round_hist[r]) std::printf("%d轮:%d ", r, st.round_hist[r]);
  std::printf("\n");

  std::printf("\n【死亡构成】\n");
  std::printf("  记忆清零 %.2f 人/局\n", st.mem_deaths / n);
  std::printf("  枪杀     %.2f 人/局\n", st.gun_deaths / n);
  std::printf("  合计     %.2f 人/局\n", (st.mem_deaths + st.gun_deaths) / n);

  std::printf("\n【枪】\n");
  std::printf("  枪在好人手里的局 %.1f%% / 在卧底手里 %.1f%%\n",
              (st.games - st.gun_mole_holder) / n * 100.0, st.gun_mole_holder / n * 100.0);
  std::printf("  平均开枪 %.2f 次/局", st.shots / n);
  if (st.shots > 0)
    std::printf("；命中卧底 %.1f%%（%lld 次打中卧底 / %lld 次打死好人）",
                st.hit_mole / double(st.shots) * 100.0, st.hit_mole, st.hit_good);
  std::printf("\n");

  std::printf("\n【记忆与身份】\n");
  std::printf("  终局时完整想起身份的好人 %.2f / 3\n", st.goods_unlocked / n);
  if (st.first_faction_n)
    std::printf("  全场首次解锁阵营：平均第 %.2f 轮（%d 局始终没人解锁）\n",
                st.first_faction_sum / double(st.first_faction_n), st.first_faction_none);

  // 卧底察觉轮次：中位数 + 分布
  std::vector<int> samples;
  for (int r = 1; r < static_cast<int>(st.aware_hist.size()); ++r)
    for (int k = 0; k < st.aware_hist[r]; ++k) samples.push_back(r);

  if (!samples.empty()) {
    std::sort(samples.begin(), samples.end());
    const int med = samples[samples.size() / 2];
    std::printf("  卧底察觉自己是卧底：中位第 %d 轮；到终局仍未察觉 %.1f%%\n", med,
                st.aware_hist[0] / n * 100.0);
  } else {
    std::printf("  卧底察觉自己是卧底：从未察觉\n");
  }

  std::string line = "  察觉轮次分布 ";
  char buf[64];
  for (int r = 0; r < static_cast<int>(st.aware_hist.size()); ++r) {
    if (!st.aware_hist[r]) continue;
    if (r == 0) std::snprintf(buf, sizeof(buf), "未察觉:%d ", st.aware_hist[r]);
    else std::snprintf(buf, sizeof(buf), "%d轮:%d ", r, st.aware_hist[r]);
    line += buf;
  }
  std::printf("%s\n\n", line.c_str());
  return 0;
}
