class_name MDAi
extends RefCounted
#
# 第一版 AI：够活就行，不打磨。
# 以后要换人格、换策略，只改这一个文件。


## 抢枪阶段选什么：0 抢 / 1 躲 / 2 盯
static func choose_gather(rules: MDRules, seat: int) -> int:
	var mem: int = rules.players[seat]["memory"]
	var r := rules.rng.randf()
	if mem <= 4:
		return MDRules.Gather.DODGE   # 记忆太薄，先自保
	if r < 0.60:
		return MDRules.Gather.GRAB
	if r < 0.85:
		return MDRules.Gather.WATCH
	return MDRules.Gather.DODGE


## 拿到枪之后要不要开、打谁。返回 -1 表示不开。
static func choose_shoot(rules: MDRules) -> int:
	if not rules.can_shoot():
		return -1
	var holder: int = rules.gun_holder
	var best := rules.highest_memory_opponent(holder)
	if best < 0:
		return -1
	# 只打"快要想起自己是谁"的人，或者拖到中盘还没打过
	if rules.players[best]["memory"] >= MDRules.FACTION_UNLOCK or rules.round_num >= 5:
		return best
	return -1


## 出牌：返回 {"index": 手牌下标, "target": 目标座位}
static func choose(rules: MDRules, seat: int) -> Dictionary:
	var p: Dictionary = rules.players[seat]
	var hand: Array = p["hand"]
	if hand.is_empty():
		return {"index": -1, "target": -1}

	var my_mem: int = p["memory"]
	var suspect := rules.highest_memory_opponent(seat)
	var helper := rules.lowest_memory_other(seat)

	var best_index := 0
	var best_score := -999.0
	var best_target := -1

	for i in hand.size():
		var cat: int = hand[i]["cat"]
		var target := -1
		var score := 0.0

		match cat:
			MDRules.Cat.REMINISCE:
				score = 1.4 if my_mem < 7 else 0.8
			MDRules.Cat.COVER:
				score = 1.1 if my_mem < 6 else 0.6
			MDRules.Cat.INVESTIGATE:
				target = suspect
				score = 1.0
			MDRules.Cat.EMPATHY:
				target = helper
				score = 1.2 if my_mem < 7 else 0.7
			MDRules.Cat.HARM:
				target = suspect
				if target >= 0:
					score = 1.3 + 0.15 * float(rules.players[target]["memory"])
			MDRules.Cat.DECEIVE:
				target = suspect
				if target >= 0:
					score = 1.1 + 0.10 * float(rules.players[target]["memory"])

		if rules.card_needs_target(cat) and target < 0:
			continue
		if score > best_score:
			best_score = score
			best_index = i
			best_target = target

	return {"index": best_index, "target": best_target}
