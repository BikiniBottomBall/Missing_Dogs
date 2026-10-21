# Missing_Dogs

四人局 **剧情向卡牌游戏**。四个失忆的人、一把枪、一副会泄露身份的牌。

Godot 4 开发，先做单机剧情，后加 4 人联机。

## 现状

**规则核心 + 线协议 + headless 服务器已完成。** Godot 客户端尚未开始（本机未安装引擎）。

`md_selftest` 26 项端到端断言全绿；`md_sim` 200 局基线见 [`docs/architecture.md`](docs/architecture.md)。

## 核心机制

**记忆值**是唯一的核心数值，同时充当三种角色：

- **命** —— 降到 0 即出局（清空记忆）
- **情报** —— 攒到 5 解锁"我是好是坏"，到 8 解锁"我到底是谁"
- **货币** —— 出牌能抢回记忆，杀人能继承死者的记忆

玩家只用两件事推理：**自己手牌的构成**说明"我是谁"，**别人打出的牌**说明"他是谁"。

每轮结束记忆 −1（失忆会退），所以你被迫在"打伤害"和"抢回自己的记忆"之间取舍。

## 目录

| 路径 | 内容 |
|---|---|
| `core/` | **规则核心**，零依赖 C++20 静态库。不引用 Godot，不引用 socket |
| `net/` | **线协议**（Command 解析 / PlayerView 编码）+ `Room` 会话层，同样不碰 socket |
| `apps/md_sim/` | 批量对局模拟器，用于平衡性回归 |
| `apps/md_selftest/` | 无网络端到端自检，26 项断言 |
| `apps/md_server/` | headless 服务器，TCP + 换行分隔 JSON（薄壳，只做投递） |
| [`docs/architecture.md`](docs/architecture.md) | Godot + C++ 服务器架构、数据流、实测基线、已知问题 |
| [`docs/design-validation.md`](docs/design-validation.md) | 规则验证报告：四项模拟实验的结论、牌池权重表、定稿规则表 v0.1 |
| `sim/` | 设计期的 Python 验证脚本（与 `core/` 独立，保留作对账用） |

## 构建

C++20，无第三方依赖。

```bash
# 模拟器
g++ -std=c++20 -O2 -I core/include core/src/*.cpp apps/md_sim/main.cpp -o build/md_sim
./build/md_sim 200 1 8 7 9          # 局数 种子 最大轮数 阵营解锁 身份解锁
./build/md_sim 400 1 8 6 9 --csv    # 扫参用，输出单行 CSV
./build/md_sim 1 1 8 7 9 --trace    # 逐动作打印一局

# 端到端自检
g++ -std=c++20 -O2 -I core/include -I net/include \
    core/src/*.cpp net/src/*.cpp apps/md_selftest/main.cpp -o build/md_selftest
./build/md_selftest

# headless 服务器（Windows 需额外 -lws2_32）
g++ -std=c++20 -O2 -I core/include -I net/include \
    core/src/*.cpp net/src/*.cpp apps/md_server/main.cpp -o build/md_server -lws2_32
./build/md_server --port 24680 --seed 1 -v
```

也可以用 CMake：`cmake -B build && cmake --build build`。

> ⚠️ 本机（Windows + Application Control）会拦截任何链接 `ws2_32` 的可执行文件，
> 所以 `md_server.exe` 能编译但不能在本机运行。细节见架构文档。

## 复跑验证

需要 Python 3.12 + numpy。

```bash
cd sim
python exp_a_inference.py    # 牌能否推断身份
python exp_a2_tuning.py      # 牌池强度调参
python exp_b_metrics.py      # 体验指标：卧底自察 / 好人误报 / 指控精度
python exp_c_memory.py       # 记忆经济参数网格
python exp_d_gunlock.py      # 枪的解锁时序
```

## 已被数据否掉的三条设定

记录在此，避免以后重蹈：

1. **开局 1 点记忆** —— 会让 56% 的对局出现记忆清零出局，其中 78% 发生在前两轮。改为开局 4 点 + 每轮 −1。
2. **记忆达到阈值才能开枪** —— 记忆每轮涨 2 点以上，阈值 3~5 第 1 轮就人人达标；要拖到中盘得设 7~8，但那时 38%~67% 的对局根本没有枪。改用结构条件。
3. **杀错人补一颗子弹** —— 4 人局里最快 3 轮清场。改为一局一发不补，杀错改为记忆污染。
