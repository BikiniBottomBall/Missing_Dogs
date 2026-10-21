class_name MDClient
extends RefCounted
#
# 协议状态机：只吃一行 JSON，不关心这行从哪来。
#
# 单机（GDExtension 进程内）和联机（TCP）共用这一个类。
# 它不知道 MDLocalServer 存在，也不知道 socket 存在。

signal command_ready(cmd_json: String)
signal joined(seat: int)
signal game_over(result: Dictionary)
signal error_received(msg: String)
signal view_received()

var seat := -1
var round := 0
var memory := 0
var alive := true
var hand: Array = []
var pub_memory: Array = []
var pub_alive: Array = []
var pub_hand: Array = []
var history: Array = []
var gun_holder := -1
var bullets := 0
var faction_known := false
var faction_is_mole := false
var identity_known := false
var identity := 0
var act: Variant = null

var result: Dictionary = {}
var views := 0
var done := false

## 若为 true，说明收到的 view 里手牌张数与 pub_hand[自己] 不一致——隐藏信息泄露了
var leak := false

## 策略钩子：给定 act，返回一条命令 JSON。留空则用默认策略。
var policy: Callable = Callable()


func feed(line: String) -> void:
	var parsed = JSON.parse_string(line)
	if typeof(parsed) != TYPE_DICTIONARY:
		error_received.emit("无法解析的 JSON: " + line.substr(0, 80))
		return
	var d: Dictionary = parsed
	match str(d.get("t", "")):
		"joined":
			seat = int(d.get("seat", -1))
			joined.emit(seat)
		"view":
			_apply_view(d)
		"over":
			result = d
			done = true
			act = null
			game_over.emit(d)
		"error":
			error_received.emit(str(d.get("msg", "")))
		_:
			error_received.emit("未知消息类型: " + str(d.get("t", "")))


func _apply_view(d: Dictionary) -> void:
	views += 1
	round = int(d.get("round", 0))
	memory = int(d.get("memory", 0))
	alive = bool(d.get("alive", false))
	hand = d.get("hand", [])
	pub_memory = d.get("pub_memory", [])
	pub_alive = d.get("pub_alive", [])
	pub_hand = d.get("pub_hand", [])
	history = d.get("history", [])
	gun_holder = int(d.get("gun_holder", -1))
	bullets = int(d.get("bullets", 0))
	faction_known = bool(d.get("faction_known", false))
	faction_is_mole = bool(d.get("faction_is_mole", false))
	identity_known = bool(d.get("identity_known", false))
	identity = int(d.get("identity", 0))
	act = d.get("act", null)

	# 隐藏信息校验：手牌张数必须等于公开信息里自己的手牌数
	if seat >= 0 and seat < pub_hand.size() and hand.size() != int(pub_hand[seat]):
		leak = true

	view_received.emit()

	if act != null and typeof(act) == TYPE_DICTIONARY:
		var cmd := ""
		if policy.is_valid():
			cmd = str(policy.call(act, self))
		else:
			cmd = default_policy(act)
		if cmd != "":
			command_ready.emit(cmd)


## 默认策略：能打就打第一张，打不了就弃权。够跑通流程。
func default_policy(a: Dictionary) -> String:
	var k := str(a.get("k", "play"))
	var cards: Array = a.get("cards", [])
	var targets: Array = a.get("targets", [])
	var pa := -1
	var pb := -1
	match k:
		"gather":
			pa = 0
		"gun":
			pa = -1 if targets.is_empty() else int(targets[0])
		"play":
			if cards.is_empty() or targets.is_empty():
				pa = -1
			else:
				pa = int(cards[0])
				pb = int(targets[0])
	return JSON.stringify({"t": "cmd", "k": k, "a": pa, "b": pb})
