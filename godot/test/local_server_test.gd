extends Node
#
# 单机（进程内）自检：验证 GDExtension 能加载、能跑完整一局，
# 且按玩家裁剪的隐藏信息没有泄漏。
#
# 跑法：godot --headless --path godot
#
# 注意这里用的是和联机完全相同的 JSON 协议——单机与联机的客户端代码
# 差别只在"这一行从哪来"，而不在怎么处理这一行。

var srv: MDLocalServer
var views := 0
var overs := 0
var my_turns := 0
var leak := false
var errors: PackedStringArray = []
var final: Dictionary = {}

func _ready() -> void:
	srv = MDLocalServer.new()
	srv.delivery.connect(_on_delivery)
	srv.start(7, 8)

	if views == 0:
		errors.append("start() 之后没有收到任何 view")
	if overs == 0:
		errors.append("整局没有产生 over 消息")

	print("---- GDExtension 单机自检 ----")
	print("views=%d  my_turns=%d  overs=%d  leak=%s" % [views, my_turns, overs, str(leak)])
	if not final.is_empty():
		print("对局结果: ", JSON.stringify(final))
	for e in errors:
		print("  [FAIL] ", e)

	var ok := errors.is_empty() and views > 0 and overs > 0 and not leak
	if ok:
		print("  [ok] 进程内跑完一局，隐藏信息未泄露")
	print("自检结果: ", "PASS" if ok else "FAIL")
	get_tree().quit(0 if ok else 1)

func _on_delivery(conn: int, line: String) -> void:
	if conn != 0:
		return
	var d = JSON.parse_string(line)
	if d == null or typeof(d) != TYPE_DICTIONARY:
		errors.append("无法解析的 JSON: " + line.substr(0, 80))
		return
	match str(d.get("t", "")):
		"view":
			views += 1
			var hand: Array = d.get("hand", [])
			var pub: Array = d.get("pub_hand", [])
			var seat: int = int(d.get("seat", -1))
			if seat >= 0 and seat < pub.size() and hand.size() != int(pub[seat]):
				leak = true
			var act = d.get("act", null)
			if act != null and typeof(act) == TYPE_DICTIONARY:
				my_turns += 1
				srv.send(0, _make_cmd(act))
		"over":
			overs += 1
			final = d
		"error":
			errors.append("服务器报错: " + str(d.get("msg", "")))

func _make_cmd(act: Dictionary) -> String:
	var k := str(act.get("k", "play"))
	var cards: Array = act.get("cards", [])
	var targets: Array = act.get("targets", [])
	var a := -1
	var b := -1
	match k:
		"gather":
			a = 0
		"gun":
			a = -1 if targets.is_empty() else int(targets[0])
		"play":
			if cards.is_empty() or targets.is_empty():
				a = -1
			else:
				a = int(cards[0])
				b = int(targets[0])
	var cmd := {"t": "cmd", "k": k, "a": a, "b": b}
	return JSON.stringify(cmd)
