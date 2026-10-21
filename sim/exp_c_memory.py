"""
实验 C'：记忆经济的参数网格

要同时回答两个问题：
  C1 「记忆清零 = 出局」这种死法，在什么参数下才真的会发生、又不会第一轮就死
  C2 「记忆 >= 3 才能开枪」这条解锁线，会不会因为记忆一直被压低而永远开不了枪
     （这是我和你的规则之间潜在的正面冲突）

网格：开局记忆 × 每轮衰减 × 伤害倍率。每个配置 1 万局，向量化并行。
"""
import numpy as np

rng = np.random.default_rng(41)

SELF_GAIN = np.array([1, 1, 2, 1, 0, 1], float)   # 调查 掩护 追忆 共感 杀伤 欺骗
ALLY_GAIN = np.array([0, 0, 0, 1, 0, 0], float)
DMG       = np.array([0, 0, 0, 0, 2, 1], float)

SUB = [[36, 14, 16], [18, 32, 16], [12, 16, 38]]
GOOD_SIDE, MOLE_SIDE = [18, 8, 10], [6, 28, 22]


def build():
    g, m = np.array(GOOD_SIDE, float), np.array(MOLE_SIDE, float)
    rows = [np.concatenate([np.array(s, float), g]) for s in SUB]
    rows.append(np.concatenate([np.array([12, 12, 15], float), m]))
    P = np.array(rows)
    P /= P.sum(axis=1, keepdims=True)
    return P, np.cumsum(P, axis=1)


POOLS, CUM = build()


def batch(G=10000, start=3.0, decay=0.0, dmg_mult=1.0, rounds=8, mem_thresh=3.0):
    mole_pos = rng.integers(0, 4, G)
    role = rng.integers(0, 3, (G, 4))
    role[np.arange(G), mole_pos] = 3
    is_good = role != 3

    mem = np.full((G, 4), float(start))
    alive = np.ones((G, 4), bool)
    first_death = np.full(G, 99)
    n_dead = np.zeros(G, int)
    ever_armed = np.zeros((G, 4), bool)
    armed_round = np.full((G, 4), 99)
    first_armed = np.full(G, 99)          # 全场首次"有人够格开枪"的轮次

    for r in range(1, rounds + 1):
        for p in range(4):
            act = alive[:, p]
            if not act.any():
                continue

            cum_p = CUM[role[:, p]]                        # (G,6)
            u = rng.random((G, 1))
            cat = (u > cum_p).sum(axis=1)

            mem[:, p] += np.where(act, SELF_GAIN[cat], 0.0)

            ally = (ALLY_GAIN[cat] > 0) & act
            if ally.any():
                sc = rng.random((G, 4))
                sc[:, p] = -1
                sc[~alive] = -1
                a = sc.argmax(axis=1)
                add = np.zeros((G, 4))
                add[np.arange(G), a] = 1.0
                mem += add * ally[:, None]

            hit = (DMG[cat] > 0) & act
            if hit.any():
                sc = rng.random((G, 4))
                sc[:, p] = -1
                sc[~alive] = -1
                good_tgt = (mem * (alive & is_good)).argmax(axis=1)
                rand_tgt = sc.argmax(axis=1)
                tgt = np.where(role[:, p] == 3, good_tgt, rand_tgt)
                dmg = DMG[cat] * dmg_mult
                ok = hit & alive[np.arange(G), tgt] & (tgt != p)
                np.add.at(mem, (np.arange(G)[ok], tgt[ok]), -dmg[ok])

        mem = np.where(alive, mem, -99)
        np.clip(mem, -99, 10, out=mem)

        now_armed = (mem >= mem_thresh) & alive & ~ever_armed
        ever_armed |= now_armed
        armed_round[now_armed & (armed_round == 99)] = r
        fresh = now_armed.any(axis=1) & (first_armed == 99)
        first_armed[fresh] = r

        died = (mem <= 0) & alive
        if died.any():
            idx = np.where(died)
            notyet = first_death[idx[0]] == 99
            first_death[idx[0][notyet]] = r
            alive &= ~died
            n_dead += died.sum(axis=1)

        mem = np.where(alive, mem - decay, mem)
        np.clip(mem, -99, 10, out=mem)

        died = (mem <= 0) & alive
        if died.any():
            idx = np.where(died)
            notyet = first_death[idx[0]] == 99
            first_death[idx[0][notyet]] = r
            alive &= ~died
            n_dead += died.sum(axis=1)

        if (alive.sum(axis=1) <= 1).all():
            break

    fd = np.where(first_death == 99, np.nan, first_death)
    ar = np.where(armed_round < 99, armed_round, np.nan)
    return dict(
        p_death=(n_dead > 0).mean(),
        fd_med=float(np.nanmedian(fd)) if np.isfinite(fd).any() else -1,
        early=((first_death <= 2) & (first_death < 99)).mean(),
        surv=alive.sum(axis=1).mean(),
        armed_any=ever_armed.any(axis=1).mean(),
        armed_med=float(np.nanmedian(ar)) if np.isfinite(ar).any() else -1,
        armed_rate=ever_armed.mean(),
        first_armed_med=float(np.median(first_armed[first_armed < 99])) if (first_armed < 99).any() else -1,
        p_armed_by3=(first_armed <= 3).mean(),
        p_armed_by5=(first_armed <= 5).mean(),
        p_never_armed=(first_armed == 99).mean(),
    )


if __name__ == "__main__":
    L = ["# 实验 C'：记忆经济参数网格", "",
         "1 万局/格。`有人能开枪` = 该局至少一人在 8 轮内记忆值达到 3。", ""]
    L += ["| 开局记忆 | 每轮衰减 | 伤害倍率 | 记忆清零出局率 | 首死中位轮 | 前2轮就死 | 8轮后剩余人数 | 有人能开枪 | 解锁时中位轮 |",
          "|---|---|---|---|---|---|---|---|---|"]
    rows = []
    for start in (2, 3, 4, 5, 6):
        for decay in (0.0, 0.5, 1.0):
            for dm in (1.0, 1.5):
                s = batch(start=start, decay=decay, dmg_mult=dm)
                rows.append((start, decay, dm, s))
                L.append(f"| {start} | {decay} | {dm} | {s['p_death']*100:.1f}% | {s['fd_med']} | "
                         f"{s['early']*100:.1f}% | {s['surv']:.2f} | {s['armed_any']*100:.1f}% | {s['armed_med']} |")

    ok = [(a, d, m) for a, d, m, s in rows
          if 0.30 <= s["p_death"] <= 0.60 and 4 <= s["fd_med"] <= 7 and s["early"] < 0.12
          and s["armed_any"] > 0.90]
    L += ["", "## 可用区（记忆清零出局率 30–60% / 首死中位轮 4–7 / 前 2 轮死亡 <12% / 枪能解锁）", ""]
    L.append("**" + ("、".join(f"开局{a}·衰减{d}·伤害×{m}" for a, d, m in ok) if ok else "无解——需要改规则") + "**")

    open("sim/exp_c2_report.md", "w", encoding="utf-8").write("\n".join(L))
    print("ok")
