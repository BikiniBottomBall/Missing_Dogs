"""
实验 D：枪的解锁时序

规则设想是"记忆值达到阈值才能开枪"，目的是让前几轮先打牌、不打枪。
但如果记忆值涨得太快，这道门就等于不存在——第 1 轮就人人有资格开枪。

在实验 C 的可用经济参数下，扫描解锁阈值 2..8，看"全场第一次有资格开枪"发生在第几轮。
"""
import os
from exp_c_memory import batch

HERE = os.path.dirname(os.path.abspath(__file__))

if __name__ == "__main__":
    L = ["# 实验 D：枪的解锁时序", "",
         "经济参数固定为实验 C 的可用区：开局 4 点、每轮衰减 1、伤害 ×1。1 万局/格。", "",
         "| 开枪所需记忆值 | 全场首次能开枪的中位轮 | 3 轮内就有人能开枪 | 5 轮内有人能开枪 | 8 轮内始终没人够格 |",
         "|---|---|---|---|---|"]
    pad = []
    for th in (2, 3, 4, 5, 6, 7, 8):
        s = batch(start=4, decay=1.0, dmg_mult=1.0, mem_thresh=float(th))
        L.append(f"| {th} | {s['first_armed_med']} | {s['p_armed_by3']*100:.1f}% | "
                 f"{s['p_armed_by5']*100:.1f}% | {s['p_never_armed']*100:.1f}% |")
        pad.append((th, s["first_armed_med"]))

    good = [th for th, m in pad if 3 <= m <= 5]
    L += ["", "## 判定", "",
          "要让枪在前 2 轮无法使用、但中盘能解锁，阈值得落在：" +
          ("**" + "、".join(str(x) for x in good) + "**" if good else "无解")]
    open(os.path.join(HERE, "exp_d_report.md"), "w", encoding="utf-8").write("\n".join(L))
    print("ok")
