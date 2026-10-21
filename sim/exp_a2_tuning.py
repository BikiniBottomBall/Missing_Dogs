"""
实验 A'：牌池强度调参

目标（设计意图）：
  自省    n=5 张手牌时，"自信且猜对" 应落在 55%~65%  —— 强提示，但不该一眼看穿
  自省    n=8 时应达到 ~80%                        —— 临近终局可以下判断
  他省    n=5 时应在 70%~80%                       —— 读别人略易于读自己，社交推理才是主菜
  全程    "自信但猜错" 不应超过 5%                  —— 超过就会产生"游戏在耍我"的观感
"""
import numpy as np

rng = np.random.default_rng(11)
CATS = ["调查", "掩护", "追忆", "共感", "杀伤", "欺骗"]
UTILITY = np.array([20, 25, 20, 10, 10, 15], float)
UTILITY /= UTILITY.sum()
CUM_UTIL = np.cumsum(UTILITY)

# 子身份轴：调查 / 掩护 / 追忆 三个好人子身份彼此分开
SUB = {
    "追捕者": [36, 14, 16],
    "目击者": [18, 32, 16],
    "救援者": [12, 16, 38],
}
# 阵营轴候选：(共感, 杀伤, 欺骗) —— 好人3个都相同，卧底单独一套
SIDE_CANDS = {
    "V1 强分离(原始)": dict(good=(20, 6, 8), mole=(1, 34, 26)),
    "V2 中等分离":     dict(good=(18, 8, 10), mole=(6, 28, 22)),
    "V3 弱分离":       dict(good=(17, 10, 11), mole=(10, 24, 19)),
    "V4 很弱分离":     dict(good=(16, 12, 12), mole=(13, 21, 17)),
}


def build(spec):
    g = np.array(spec["good"], float)
    m = np.array(spec["mole"], float)
    pools = {}
    for k, sub in SUB.items():
        v = np.concatenate([np.array(sub, float), g])
        pools[k] = v / v.sum()
    v = np.concatenate([np.array([12, 12, 15], float), m])
    pools["卧底"] = v / v.sum()
    return np.array([pools[i] for i in ["追捕者", "目击者", "救援者", "卧底"]])


def sample(true_ids, n, mix, P, CUM_P):
    T = len(true_ids)
    go_id = rng.random((T, n)) < mix
    u_util = rng.random((T, n))
    cat_util = (u_util[:, :, None] > CUM_UTIL).sum(axis=2)
    cat = np.empty((T, n), dtype=np.int64)
    for k in range(4):
        m = true_ids == k
        if not m.any():
            continue
        u_id = rng.random((m.sum(), n))
        cat_id = (u_id[:, :, None] > CUM_P[k]).sum(axis=2)
        cat[m] = np.where(go_id[m], cat_id, cat_util[m])
    flat = cat + 6 * np.arange(T)[:, None]
    return np.bincount(flat.ravel(), minlength=6 * T).reshape(T, 6)


def curve(P, mix, n_max=9, T=120_000):
    LOGP = np.log(P)
    CUM_P = np.cumsum(P, axis=1)
    out = {}
    for n in range(1, n_max + 1):
        true = rng.integers(0, 4, size=T)
        c = sample(true, n, mix, P, CUM_P)
        ll = c @ LOGP.T + np.log(0.25)
        ll -= ll.max(axis=1, keepdims=True)
        w = np.exp(ll)
        post = w / w.sum(axis=1, keepdims=True)
        p_mole = post[:, 3]
        p_good = 1 - p_mole
        side_pred = (p_mole > p_good).astype(int)
        true_side = (true == 3).astype(int)
        conf = np.maximum(p_mole, p_good)
        out[n] = dict(
            side=(side_pred == true_side).mean(),
            ok=((conf >= 0.8) & (side_pred == true_side)).mean(),
            bad=((conf >= 0.8) & (side_pred != true_side)).mean(),
            unsure=(conf < 0.8).mean(),
            ident=(post.argmax(axis=1) == true).mean(),
        )
    return out


if __name__ == "__main__":
    lines = ["# 实验 A'：牌池强度调参", "",
             "目标：自省 n=5 落在 55–65% 的“自信且对”，n=8 约 80%；“自信但错”全程 <5%。", ""]
    summary = []
    for name, spec in SIDE_CANDS.items():
        P = build(spec)
        c1 = curve(P, mix=1.00)   # 自省：整手都是身份典型牌
        c2 = curve(P, mix=0.70)   # 他省：30% 效用牌
        c3 = curve(P, mix=0.50)   # 他省：一半在伪装

        lines.append(f"## {name}")
        lines.append("")
        lines.append("| 牌数 | 自省:猜中 | 自省:自信对 | 自省:自信错 | 自省:没把握 "
                     "| 他省30%:自信对 | 他省50%:自信对 | 猜中具体身份 |")
        lines.append("|---|---|---|---|---|---|---|---|")
        for n in range(1, 10):
            a, b, d = c1[n], c2[n], c3[n]
            lines.append(f"| {n} | {a['side']*100:.1f}% | **{a['ok']*100:.1f}%** | {a['bad']*100:.1f}% | "
                         f"{a['unsure']*100:.1f}% | {b['ok']*100:.1f}% | {d['ok']*100:.1f}% | {a['ident']*100:.1f}% |")
        lines.append("")
        summary.append((name, c1[5]["ok"], c1[8]["ok"], c2[5]["ok"]))

    lines.append("## 汇总（关键格）")
    lines.append("")
    lines.append("| 方案 | 自省 n=5 | 自省 n=8 | 他省30% n=5 | 判定 |")
    lines.append("|---|---|---|---|---|")
    for name, s5, s8, o5 in summary:
        verdict = "✅ 可用" if (0.52 <= s5 <= 0.68 and 0.72 <= o5 <= 0.85) else "❌ 偏离"
        lines.append(f"| {name} | {s5*100:.1f}% | {s8*100:.1f}% | {o5*100:.1f}% | {verdict} |")

    open("sim/exp_a2_report.md", "w", encoding="utf-8").write("\n".join(lines))
    print("ok")
