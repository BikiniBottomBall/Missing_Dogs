extends Node3D
#
# 第一版 3D 牌桌原型。
#
# 刻意做得很丑：桌子是方块，人是胶囊，牌是薄片 + 3D 文字。
# 这一版要验证的不是画面，而是 **牌桌循环能不能转起来**：
#   点一张牌 -> 结算 -> 记忆值变化 -> 轮到别人 -> 一轮结束记忆全体 -1 -> 直到分出胜负
#
# 你自己是座位 0。目标座位目前是自动挑的（杀伤/欺骗/调查打记忆最高的人，
# 共感给记忆最低的人），因为"点牌再点人"的两段式交互留到下一版。
#
# 无头自检：godot --headless --path game -- --auto

const RULES_SCRIPT := preload("res://rules.gd")
const AI_SCRIPT := preload("res://ai.gd")

const SEAT_RADIUS := 1.15
const CARD_W := 0.09
const CARD_D := 0.13
const PLAYER_SEAT := 0

const CAT_COLORS := [
	Color(0.35, 0.55, 0.85),  # 调查
	Color(0.40, 0.70, 0.45),  # 掩护
	Color(0.75, 0.65, 0.35),  # 追忆
	Color(0.80, 0.45, 0.65),  # 共感
	Color(0.80, 0.30, 0.30),  # 杀伤
	Color(0.55, 0.35, 0.75),  # 欺骗
]

var rules: MDRules
var hand_root: Node3D
var seat_labels: Array = []
var status_label: Label
var busy := false
var auto_mode := false


func _ready() -> void:
	auto_mode = "--auto" in OS.get_cmdline_user_args()
	_build_room()
	_build_ui()

	rules = RULES_SCRIPT.new()
	rules.setup(randi())

	_refresh()
	if auto_mode:
		_run_auto()
	else:
		_maybe_ai_turn()


# ---------------------------------------------------------------- 场景搭建

func _build_room() -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-55, -40, 0)
	light.light_energy = 1.1
	add_child(light)

	var cam := Camera3D.new()
	cam.position = Vector3(0, 1.05, 1.85)
	cam.look_at_from_position(Vector3(0, 1.05, 1.85), Vector3(0, 0.05, 0))
	cam.current = true
	add_child(cam)

	# 地板
	var floor_mesh := MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(8, 0.02, 8)
	floor_mesh.mesh = floor_box
	floor_mesh.position = Vector3(0, -0.5, 0)
	floor_mesh.material_override = _mat(Color(0.10, 0.10, 0.12))
	add_child(floor_mesh)

	# 桌子
	var table := MeshInstance3D.new()
	var table_box := BoxMesh.new()
	table_box.size = Vector3(1.7, 0.06, 1.7)
	table.mesh = table_box
	table.material_override = _mat(Color(0.22, 0.17, 0.13))
	add_child(table)

	# 四个座位 + 记忆值标签
	for s in MDRules.SEATS:
		var pos := _seat_position(s)

		var body := MeshInstance3D.new()
		var cap := CapsuleMesh.new()
		cap.radius = 0.12
		cap.height = 0.42
		body.mesh = cap
		body.position = pos + Vector3(0, 0.18, 0)
		body.material_override = _mat(Color(0.5, 0.5, 0.55) if s != PLAYER_SEAT else Color(0.9, 0.8, 0.4))
		add_child(body)

		var lbl := Label3D.new()
		lbl.font_size = 48
		lbl.pixel_size = 0.0016
		lbl.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		lbl.position = pos + Vector3(0, 0.62, 0)
		lbl.text = ""
		add_child(lbl)
		seat_labels.append(lbl)

	hand_root = Node3D.new()
	hand_root.position = Vector3(0, 0.06, 0.52)
	add_child(hand_root)


func _build_ui() -> void:
	var layer := CanvasLayer.new()
	add_child(layer)
	status_label = Label.new()
	status_label.position = Vector2(16, 12)
	status_label.add_theme_font_size_override("font_size", 18)
	layer.add_child(status_label)


func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	return m


func _seat_position(s: int) -> Vector3:
	var a := deg_to_rad(90.0 * float(s))
	return Vector3(sin(a) * SEAT_RADIUS, 0.0, cos(a) * SEAT_RADIUS)


# ---------------------------------------------------------------- 渲染刷新

func _refresh() -> void:
	for s in MDRules.SEATS:
		var p: Dictionary = rules.players[s]
		var name_text := "你" if s == PLAYER_SEAT else "对手%d" % s
		if not p["alive"]:
			seat_labels[s].text = "%s\n记忆 %d · 出局" % [name_text, maxi(p["memory"], 0)]
		else:
			seat_labels[s].text = "%s\n记忆 %d" % [name_text, p["memory"]]

	for child in hand_root.get_children():
		child.queue_free()

	var hand: Array = rules.players[PLAYER_SEAT]["hand"]
	var n := hand.size()
	for i in n:
		var x := (float(i) - float(n - 1) * 0.5) * (CARD_W + 0.015)
		var card := _make_card(hand[i]["cat"], i)
		card.position = Vector3(x, 0.0, -absf(x) * 0.25)
		hand_root.add_child(card)

	_update_status()


func _update_status() -> void:
	var p: Dictionary = rules.players[PLAYER_SEAT]
	var lines := []
	lines.append("第 %d / %d 轮" % [mini(rules.round_num, MDRules.MAX_ROUNDS), MDRules.MAX_ROUNDS])
	lines.append("你的记忆 %d（阵营解锁 %d / 身份解锁 %d）" % [
		p["memory"], MDRules.FACTION_UNLOCK, MDRules.IDENTITY_UNLOCK,
	])
	var know := rules.self_knowledge(PLAYER_SEAT)
	if know == 0:
		lines.append("自我认知：一片空白")
	elif know == 1:
		lines.append("自我认知：你隐约知道自己站在哪一边了")
	else:
		lines.append("自我认知：你想起了自己是谁")
	if rules.over:
		lines.append("")
		lines.append("对局结束：" + ("好人胜" if rules.winner == 0 else "卧底胜"))
	elif rules.current == PLAYER_SEAT:
		lines.append("")
		lines.append("轮到你了 —— 点一张牌打出")
	else:
		lines.append("")
		lines.append("等待对手…")
	status_label.text = "\n".join(lines)


func _make_card(cat: int, index: int) -> Area3D:
	var area := Area3D.new()

	var mi := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = Vector3(CARD_W, 0.006, CARD_D)
	mi.mesh = box
	mi.material_override = _mat(CAT_COLORS[cat])
	area.add_child(mi)

	var cs := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = Vector3(CARD_W, 0.03, CARD_D)
	cs.shape = shape
	area.add_child(cs)

	var lbl := Label3D.new()
	lbl.text = MDRules.CAT_NAMES[cat]
	lbl.font_size = 40
	lbl.pixel_size = 0.0009
	lbl.rotation_degrees = Vector3(-90, 0, 0)
	lbl.position = Vector3(0, 0.006, 0)
	lbl.modulate = Color(1, 1, 1)
	area.add_child(lbl)

	area.input_event.connect(_on_card_clicked.bind(index))
	return area


# ---------------------------------------------------------------- 交互

func _on_card_clicked(
	_camera: Node, event: InputEvent, _pos: Vector3, _normal: Vector3, _shape: int, index: int
) -> void:
	if busy or rules.over:
		return
	if not (event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT):
		return
	if rules.current != PLAYER_SEAT:
		return
	_play_card(PLAYER_SEAT, index)


func _auto_target(seat: int, cat: int) -> int:
	if cat == MDRules.Cat.EMPATHY:
		return rules.lowest_memory_other(seat)
	return rules.highest_memory_opponent(seat)


func _play_card(seat: int, index: int) -> void:
	var hand: Array = rules.players[seat]["hand"]
	if index < 0 or index >= hand.size():
		return
	var cat: int = hand[index]["cat"]
	var target := -1
	if rules.card_needs_target(cat):
		target = _auto_target(seat, cat)

	busy = true
	rules.play(seat, index, target)
	_refresh()
	busy = false
	_maybe_ai_turn()


func _maybe_ai_turn() -> void:
	if auto_mode:
		return
	if rules.over or rules.current == PLAYER_SEAT:
		return
	busy = true
	await get_tree().create_timer(0.45).timeout
	busy = false
	if rules.over:
		return
	var seat: int = rules.current
	var choice: Dictionary = AI_SCRIPT.choose(rules, seat)
	var ok: bool = false
	if choice["index"] >= 0:
		ok = rules.play(seat, choice["index"], choice["target"])
	if not ok:
		# AI 打不出牌（理论上不该发生）：直接标记它这轮已行动，避免卡死
		rules.players[seat]["acted"] = true
		rules._advance()
	_refresh()
	_maybe_ai_turn()


# ---------------------------------------------------------------- 无头自检

func _run_auto() -> void:
	var guard := 0
	while not rules.over and guard < 4000:
		guard += 1
		var seat: int = rules.current
		var choice: Dictionary = AI_SCRIPT.choose(rules, seat)
		var ok: bool = false
		if choice["index"] >= 0:
			ok = rules.play(seat, choice["index"], choice["target"])
		if not ok:
			rules.players[seat]["acted"] = true
			rules._advance()
	print("AUTO_RESULT winner=%d rounds=%d guard=%d" % [rules.winner, rules.round_num, guard])
	for s in MDRules.SEATS:
		var p: Dictionary = rules.players[s]
		print("  seat %d ident=%s memory=%d alive=%s" % [
			s, MDRules.IDENT_NAMES[p["ident"]], p["memory"], str(p["alive"]),
		])
	_refresh()
	await get_tree().process_frame
	get_tree().quit(0 if rules.over else 1)
