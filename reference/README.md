# reference/ —— 已冻结的 C++ 方案

这里是一套**完整跑通、但方向不对**的实现，保留作参考，不再维护。

## 为什么冻结

最初的架构决定是："规则必须只有一份实现，且必须住在 C++ 里"，于是
Godot 只做表现层，规则全部由 C++ 的 `md::Session` / `md::net::Room` 承担，
通过 GDExtension 进单机、通过 TCP 进服务器。

后来确认了真实方向是：**玩法用 Godot 引擎做，C++ 只用来搭服务器。**
那么"规则全锁在 C++、GDScript 一行规则都不能写"这个前提就不成立了——
它会让写玩法变得极其笨重，对一个剧情向游戏是错的取舍。

所以这套东西被挪到这里冻结。**保留而不是删除**，是因为里面有几样东西还有用。

## 还有什么用

| 内容 | 价值 |
|---|---|
| `reference/architecture.md` | 架构分析、GDExtension 踩过的四个坑、本机二进制策略墙的记录 |
| `reference/core/` | 规则的一份可运行实现：牌池权重、记忆值经济、枪的结算 |
| `reference/core/src/ai.cpp` | 一个"只吃 PlayerView、无法作弊"的 AI 写法，做 AI 陪玩时可以照抄思路 |
| `reference/baseline-200.txt` | 200 局平衡基线（好人胜 66.5%、平均 5.42 轮、枪命中率 48.8%） |
| `reference/selftest-output.txt` | 26 项端到端断言的真实输出 |
| `reference/godot_gdextension/` | 一个能跑通的 GDExtension 例子，以后真要写 C++ 扩展可以直接抄 |

**注意**：`reference/core/` 里的规则数值和 `docs/design-validation.md` 是同一套，
但**代码不是权威**。以后以 Godot 里的实现为准，这里的只作对账参考。

## 怎么构建（如果你哪天需要）

```bash
cd reference
g++ -std=c++20 -O2 -I core/include core/src/*.cpp apps/md_sim/main.cpp -o build/md_sim
./build/md_sim 200 1 8 7 9
```

GDExtension 的构建说明在 `reference/architecture.md`。
