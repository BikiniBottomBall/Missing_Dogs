"""
实验 A：验证核心假设 —— "玩家能否只靠牌反推身份"

两问：
  A1  自省：只看自己的手牌，能否推断"我是谁"
  A2  他省：只看别人打出的牌，能否推断"他是不是卧底"

模型：
  - 每个身份 = 一个隐藏的 6 类牌池
  - 关键设计：把牌分成两组
      * "阵营轴"三类（共感/杀伤/欺骗）—— 好人与卧底差异极大
      * "子身份轴"三类（调查/掩护/追忆）—— 三个好人子身份之间才区分
    这样阵营推断和子身份推断互不污染。
  - 玩家用贝叶斯后验推断（等价于游戏内置的"侦探笔记"）
  - A2 加入"策略噪声"：真人对局里人不会总打身份典型牌，
    有一定概率改打"当下最有用"的牌（与身份无关）
"""

import numpy as np

rng = np.random.default_rng(7)

CATS = ["调查", "掩护", "追忆", "共感", "杀伤", "欺骗"]
#        0     1     2     3     4     5
SUB_AXIS = [0, 1, 2]      # 子身份轴
SIDE_AXIS = [3, 4, 5]     # 阵营轴

IDS = ["追捕者", "目击者", "救援者", "卧底"]
IS_MOLE = np.array([False, False, False, True])

POOLS = {
    #            调查  掩护  追忆 | 共感  杀伤  欺骗
    "追捕者": np.array([36, 14, 16, 20,  6,  8], float),
    "目击者": np.array([18, 32, 16, 20,  6,  8], float),
    "救援者": np.array([12, 16, 38, 20,  6,  8], float),
    "卧底":   np.array([12, 12, 15,  1, 34, 26], float),
}
for k in POOLS:
    POOLS[k] /= POOLS[k].sum()

P = np.array([POOLS[i] for i in IDS])       # (4, 6)
LOGP = np.log(P)

# 效用牌池：与身份无关的"当下最有用"分布
UTILITY = np.array([20, 25, 20, 10, 10, 15], float)
UTILITY /= UTILITY.sum()


def posterior_from_counts(counts, prior=None):
    """counts: (T,6) -> (T,4) 后验"""
    prior = np.full(4, 0.25) if prior is None else prior
    ll = counts @ LOGP.T + np.log(prior)
    ll = ll - ll.max(axis=1, keepdims=True)
    w = np.exp(ll)
    return w / w.sum(axis=1, keepdims=True)


CUM_UTIL = np.cumsum(UTILITY)
CUM_P = np.cumsum(P, axis=1)


def sample_cards(true_ids, n, mix=1.0):
    """mix=1 全是身份典型牌；mix=0.7 表示 30% 的牌被"效用牌"替换。

    全向量化：逐张抽 n 张牌，每张牌先决定"走身份池还是走效用池"，
    再按对应的累积分布反解类别。
    """
    T = len(true_ids)
    go_identity = rng.random((T, n)) < mix

    u_util = rng.random((T, n))
    cat_util = (u_util[:, :, None] > CUM_UTIL).sum(axis=2)

    cat = np.empty((T, n), dtype=np.int64)
    for k in range(4):
        m = true_ids == k
        if not m.any():
            continue
        u_id = rng.random((m.sum(), n))
        cat_id = (u_id[:, :, None] > CUM_P[k]).sum(axis=2)
        cat[m] = np.where(go_identity[m], cat_id, cat_util[m])

    flat = cat + 6 * np.arange(T)[:, None]
    return np.bincount(flat.ravel(), minlength=6 * T).reshape(T, 6)


def report(title, rows, note=""):
    L = [f"## {title}", ""]
    if note:
        L += [note, ""]
    L += ["| 牌数 | 猜中阵营 | 猜中具体身份 | 自信(≥0.8)且正确 | 自信但错 | 还没把握 |",
          "|---|---|---|---|---|---|"]
    for r in rows:
        L.append(f"| {r['n']} | {r['side']*100:.1f}% | {r['ident']*100:.1f}% | "
                 f"{r['conf_ok']*100:.1f}% | {r['conf_bad']*100:.1f}% | {r['unsure']*100:.1f}% |")
    return "\n".join(L)


def sim(n_max=10, T=150_000, mix=1.0):
    rows = []
    for n in range(1, n_max + 1):
        true = rng.integers(0, 4, size=T)
        counts = sample_cards(true, n, mix)
        post = posterior_from_counts(counts)

        side = np.where(post[:, 3] > post[:, :3].sum(axis=1), 1, 0)
        true_side = IS_MOLE[true].astype(int)
        conf = np.maximum(post[:, 3], post[:, :3].sum(axis=1))
        rows.append(dict(
            n=n,
            side=(side == true_side).mean(),
            ident=(post.argmax(axis=1) == true).mean(),
            conf_ok=((conf >= 0.8) & (side == true_side)).mean(),
            conf_bad=((conf >= 0.8) & (side != true_side)).mean(),
            unsure=(conf < 0.8).mean(),
        ))
    return rows


if __name__ == "__main__":
    out = ["# 实验 A：牌 → 身份 的可推断性", "",
           f"牌池设计：阵营轴=共感/杀伤/欺骗，子身份轴=调查/掩护/追忆。每条 15 万次采样。", ""]

    out.append(report("A1 自省：只看自己手牌（牌全是身份典型牌）", sim(mix=1.0),
                      "玩家失忆开局把整手牌摊开看。"))
    out.append("")
    out.append(report("A2 他省：只看别人打出的牌（30% 打的是效用牌，不看身份）",
                      sim(mix=0.7), "模拟真人按局势出牌、而非按身份出牌。"))
    out.append("")
    out.append(report("A3 他省：一半的牌是效用牌（对手很会伪装）", sim(mix=0.5)))

    # 补充：单张牌的判别力，用来排牌池
    out.append("")
    out.append("## A4 单张牌的信息量（对数似然比，正=偏向好人）")
    out.append("")
    out.append("| 牌类 | 好人池占比 | 卧底池占比 | 似然比 | 对数是 | 偏向 |")
    out.append("|---|---|---|---|---|---|")
    good_avg = P[:3].mean(axis=0)
    for i, c in enumerate(CATS):
        g, m = good_avg[i], P[3][i]
        lr = g / m if m > 0 else float("inf")
        out.append(f"| {c} | {g*100:.1f}% | {m*100:.1f}% | {lr:.2f} | "
                   f"{np.log(lr):+.2f} | {'好人' if lr>1 else '卧底'} |")

    txt = "\n".join(out)
    open("sim/exp_a_report.md", "w", encoding="utf-8").write(txt)
    print("written sim/exp_a_report.md")
