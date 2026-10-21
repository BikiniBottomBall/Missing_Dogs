"""
实验 B：把"猜身份"拆成三个真正会影响体验的量

上一版指标有偏：1v3 里好人占 75%，玩家只要默认"我是好人"就已经 75% 正确了。
"自信 >= 0.8" 这条线几乎被先验白送，测出来的不是牌的信息量。

真正该测：
  B1 卧底的自我发现曲线   —— 卧底玩家在第 n 张牌时，有多大概率察觉到"我不是好人"
  B2 好人的误报率         —— 好人会不会被自己的牌吓到，错以为自己才是卧底
  B3 他省的精度与召回     —— 看别人打了 n 张牌，指控的准确率/覆盖率
  B4 子身份可分辨性       —— "我是哪种好人"能不能分得开（决定子身份要不要给机制）
"""
import numpy as np

rng = np.random.default_rng(23)
CATS = ["调查", "掩护", "追忆", "共感", "杀伤", "欺骗"]
UTILITY = np.array([20, 25, 20, 10, 10, 15], float)
UTILITY /= UTILITY.sum()
CUM_UTIL = np.cumsum(UTILITY)

SUB = {"追捕者": [36, 14, 16], "目击者": [18, 32, 16], "救援者": [12, 16, 38]}
SIDE_CANDS = {
    "V1 强分离":   dict(good=(20, 6, 8),  mole=(1, 34, 26)),
    "V2 中等分离": dict(good=(18, 8, 10), mole=(6, 28, 22)),
    "V3 弱分离":   dict(good=(17, 10, 11), mole=(10, 24, 19)),
    "V4 很弱分离": dict(good=(16, 12, 12), mole=(13, 21, 17)),
}


def build(spec):
    g, m = np.array(spec["good"], float), np.array(spec["mole"], float)
    rows = [np.concatenate([np.array(SUB[k], float), g]) for k in SUB]
    rows.append(np.concatenate([np.array([12, 12, 15], float), m]))
    P = np.array(rows)
    return P / P.sum(axis=1, keepdims=True)


def draw(P, k, n, T, mix=1.0):
    """从身份 k 的牌池抽 n 张牌（mix 比例走身份池，其余走效用池），返回 (T,6) 计数"""
    go_id = rng.random((T, n)) < mix
    cat_util = (rng.random((T, n))[:, :, None] > CUM_UTIL).sum(axis=2)
    cat_id = (rng.random((T, n))[:, :, None] > np.cumsum(P[k])).sum(axis=2)
    cat = np.where(go_id, cat_id, cat_util)
    flat = cat + 6 * np.arange(T)[:, None]
    return np.bincount(flat.ravel(), minlength=6 * T).reshape(T, 6)


def post(P, counts):
    ll = counts @ np.log(P).T + np.log(0.25)
    ll -= ll.max(axis=1, keepdims=True)
    w = np.exp(ll)
    return w / w.sum(axis=1, keepdims=True)


def run(name, P, T=80_000, n_max=10):
    # ---------- B1 卧底自我发现 ----------
    b1 = {}
    for n in range(1, n_max + 1):
        c = draw(P, 3, n, T)
        pm = post(P, c)[:, 3]
        b1[n] = ((pm > 0.5).mean(), (pm > 0.8).mean())

    # ---------- B2 好人误报 ----------
    b2 = {}
    for n in range(1, n_max + 1):
        alarms = []
        for k in range(3):
            c = draw(P, k, n, T // 3)
            alarms.append((post(P, c)[:, 3] > 0.5).mean())
        b2[n] = float(np.mean(alarms))

    # ---------- B3 他省精度/召回 ----------
    b3 = {}
    for n in range(2, n_max + 1):
        tp = fp = fn = tn = 0
        for k in range(4):
            c = draw(P, k, n, T // 4, mix=0.7)
            flag = post(P, c)[:, 3] > 0.5
            if k == 3:
                tp += flag.sum(); fn += (~flag).sum()
            else:
                fp += flag.sum(); tn += (~flag).sum()
        prec = tp / max(tp + fp, 1)
        rec = tp / max(tp + fn, 1)
        b3[n] = (prec, rec, (tp + tn) / (tp + fp + fn + tn))

    # ---------- B4 子身份可分辨性 ----------
    b4 = {}
    for n in range(2, n_max + 1):
        accs = []
        for k in range(3):
            c = draw(P, k, n, T // 3)
            p = post(P, c)
            sub = p[:, :3].argmax(axis=1)
            accs.append((sub == k).mean())
        b4[n] = float(np.mean(accs))
    return b1, b2, b3, b4


if __name__ == "__main__":
    L = ["# 实验 B：三个真正影响体验的量", "",
         "1v3 局中好人占 75%，因此“我大概率是好人”是白送的先验。",
         "真正决定体验的是：卧底多快发现、好人多容易被吓到、指控别人有多准。", ""]

    for name, spec in SIDE_CANDS.items():
        P = build(spec)
        b1, b2, b3, b4 = run(name, P)
        L += [f"## {name}", ""]
        L += ["| 牌数 | 卧底察觉(>0.5) | 卧底确信(>0.8) | 好人误报 | 指控精度 | 指控召回 | 指控总正确率 | 认出子身份 |",
              "|---|---|---|---|---|---|---|---|"]
        for n in range(1, 11):
            d1 = f"{b1[n][0]*100:.1f}% | {b1[n][1]*100:.1f}%"
            if n >= 2:
                pr, rc, acc = b3[n]
                tail = f"{pr*100:.1f}% | {rc*100:.1f}% | {acc*100:.1f}% | {b4[n]*100:.1f}%"
            else:
                tail = "— | — | — | —"
            L.append(f"| {n} | {d1} | {b2[n]*100:.1f}% | {tail} |")
        L.append("")
    open("sim/exp_b_report.md", "w", encoding="utf-8").write("\n".join(L))
    print("ok")
