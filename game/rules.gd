class_name MDRules
extends RefCounted
#
# 纯规则层：不引用任何节点、不碰渲染，因此可以无头跑、可以单测。
# 数值来自 docs/design-validation.md（已经过蒙特卡洛验证的那一版）。

const SEATS := 4
const MEMORY_START := 4
const MEMORY_CAP := 10
const FACTION_UNLOCK := 7
const IDENTITY_UNLOCK := 9
const MAX_ROUNDS := 8
const HAND_CAP := 7

enum Cat { INVESTIGATE, COVER, REMINISCE, EMPATHY, HARM, DECEIVE }
enum Ident { HUNTER, WITNESS, RESCUER, MOLE }

const CAT_NAMES := ["调查", "掩护", "追忆", "共感", "杀伤", "欺骗"]
const IDENT_NAMES := ["追捕者", "目击者", "救援者", "卧底"]

# 前三位是三个好人子身份，第四位是卧底。列顺序同 Cat。
const POOLS := [
	[36, 14, 16, 18, 8, 10],
	[18, 32, 16, 18, 8, 10],
	[12, 16, 38, 18, 8, 10],
	[12, 12, 15, 6, 28, 22],
]
# 通用牌：与身份无关，不泄露信息
const UTILITY := [20, 25, 20, 10, 10, 15]

var rng := RandomNumberGenerator.new()
var round_num := 0
var players: Array = []
var current := 0
var history: Array = []
var over := false
var winner := -1  # 0 好人胜 / 1 卧底胜 / -1 未结束


func setup(seed_value: int) -> void:
	rng.seed = seed_value
	players.clear()
	history.clear()
	over = false
	winner = -1
	round_num = 1

	var idents := [Ident.HUNTER, Ident.WITNESS, Ident.RESCUER, Ident.MOLE]
	for i in range(idents.size() - 1, 0, -1):
		var j := rng.randi_range(0, i)
		var tmp = idents[i]
		idents[i] = idents[j]
		idents[j] = tmp

	for s in SEATS:
		players.append({
			"ident": idents[s],
			"memory": MEMORY_START,
			"alive": true,
			"hand": [],
			"shield": 0,
			"acted": false,
		})

	for s in SEATS:
		_draw(s, 3, 0.0)
		_draw(s, 2, 1.0)

	current = _first_alive()


# ---------------------------------------------------------------- 抽牌

func _draw(seat: int, n: int, imprint_chance: float) -> void:
	var p: Dictionary = players[seat]
	var pool: Array = POOLS[p["ident"]]
	for _i in n:
		if p["hand"].size() >= HAND_CAP:
			return
		var imprint: bool = rng.randf() < imprint_chance
		p["hand"].append({"cat": _weighted(pool if imprint else UTILITY), "imprint": imprint})


func _weighted(weights: Array) -> int:
	var total := 0.0
	for w in weights:
		total += float(w)
	var r := rng.randf() * total
	var acc := 0.0
	for i in weights.size():
		acc += float(weights[i])
		if r < acc:
			return i
	return weights.size() - 1


# ---------------------------------------------------------------- 回合

func can_act(seat: int) -> bool:
	return not over and players[seat]["alive"] and not players[seat]["acted"]


func card_needs_target(cat: int) -> bool:
	return cat in [Cat.INVESTIGATE, Cat.EMPATHY, Cat.HARM, Cat.DECEIVE]


func play(seat: int, hand_index: int, target: int) -> bool:
	if not can_act(seat):
		return false
	var p: Dictionary = players[seat]
	if hand_index < 0 or hand_index >= p["hand"].size():
		return false

	var card: Dictionary = p["hand"][hand_index]
	var cat: int = card["cat"]

	if card_needs_target(cat):
		if target < 0 or target >= SEATS or target == seat or not players[target]["alive"]:
			return false
	else:
		target = -1

	p["hand"].remove_at(hand_index)
	history.append({"seat": seat, "round": round_num, "cat": cat})
	_apply(seat, cat, target)
	p["acted"] = true
	_advance()
	return true


func _apply(seat: int, cat: int, target: int) -> void:
	var me: Dictionary = players[seat]
	match cat:
		Cat.INVESTIGATE:
			me["memory"] += 1
		Cat.COVER:
			me["memory"] += 1
			me["shield"] += 1
		Cat.REMINISCE:
			me["memory"] += 2
			_draw(seat, 1, 0.5)
		Cat.EMPATHY:
			me["memory"] += 1
			if target >= 0:
				players[target]["memory"] += 1
		Cat.HARM:
			if target >= 0:
				_deal(target, 2)
		Cat.DECEIVE:
			me["memory"] += 1
			if target >= 0:
				_deal(target, 1)
	me["memory"] = mini(me["memory"], MEMORY_CAP)


func _deal(seat: int, amount: int) -> void:
	var t: Dictionary = players[seat]
	if t["shield"] > 0:
		t["shield"] -= 1
	else:
		t["memory"] -= amount


func _advance() -> void:
	if _check_over():
		return
	for i in range(1, SEATS + 1):
		var s := (current + i) % SEATS
		if players[s]["alive"] and not players[s]["acted"]:
			current = s
			return
	_finish_round()


func _finish_round() -> void:
	# 失忆会退：每轮结束全体 −1
	for s in SEATS:
		var p: Dictionary = players[s]
		if not p["alive"]:
			continue
		p["memory"] = mini(p["memory"], MEMORY_CAP)
		p["memory"] -= 1
		if p["memory"] <= 0:
			p["alive"] = false

	if _check_over():
		return

	round_num += 1
	if round_num > MAX_ROUNDS:
		_finish(1 if _mole_alive() else 0)
		return

	for s in SEATS:
		players[s]["acted"] = false
		if players[s]["alive"]:
			_draw(s, 1, 0.5)

	current = _first_alive()


func _first_alive() -> int:
	for i in SEATS:
		if players[i]["alive"]:
			return i
	return 0


func _mole_alive() -> bool:
	for i in SEATS:
		if players[i]["alive"] and players[i]["ident"] == Ident.MOLE:
			return true
	return false


func _check_over() -> bool:
	if not _mole_alive():
		_finish(0)
		return true
	var alive := 0
	for p in players:
		if p["alive"]:
			alive += 1
	if alive <= 2:
		_finish(1)
		return true
	return false


func _finish(w: int) -> void:
	winner = w
	over = true


# ---------------------------------------------------------------- 查询

## 某个座位解锁了什么程度的自我认知
func self_knowledge(seat: int) -> int:
	var m: int = players[seat]["memory"]
	if m >= IDENTITY_UNLOCK:
		return 2
	if m >= FACTION_UNLOCK:
		return 1
	return 0


func alive_seats() -> Array:
	var out: Array = []
	for i in SEATS:
		if players[i]["alive"]:
			out.append(i)
	return out


func highest_memory_opponent(seat: int) -> int:
	var best := -1
	var best_mem := -999
	for i in SEATS:
		if i == seat or not players[i]["alive"]:
			continue
		if players[i]["memory"] > best_mem:
			best_mem = players[i]["memory"]
			best = i
	return best


func lowest_memory_other(seat: int) -> int:
	var best := -1
	var best_mem := 999
	for i in SEATS:
		if i == seat or not players[i]["alive"]:
			continue
		if players[i]["memory"] < best_mem:
			best_mem = players[i]["memory"]
			best = i
	return best
