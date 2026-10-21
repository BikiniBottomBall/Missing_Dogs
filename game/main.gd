@tool
extends Node3D
#
# 3D 牌桌 demo。
#
# 注意：这个场景是**纯代码生成**的。main.tscn 里只有一个空 Node3D + 这个脚本，
# 桌子/座位/灯光都在 _ready() 里建出来。所以加了 @tool —— 否则编辑器里打开
# 是一片空白，只有按 F5 才看得到东西。
#
# 编辑器里只搭静态部分（桌子、四个座位、那把枪），不跑对局；
# 刻意做得很丑：桌子是方块，人是胶囊，牌是薄片 + 3D 文字。要验的不是画面，
# 是"这个循环好不好玩"：
#
#   抢枪（抢/躲/盯）-> 出牌或开枪 -> 记忆值变化 -> 一轮结束全体 -1
#   -> 记忆到 7 知道自己是好是坏，到 9 知道是谁 -> 分出胜负
#
# 操作：
#   抢枪阶段      点屏幕中间的三个按钮
#   出牌          点一张牌；需要目标时座位会高亮，再点人
#   开枪          持枪且第 2 轮起，右下角"开枪"按钮，再点人
#   取消          再点任意一张牌
#
# 无头自检：godot --headless --path game -- --auto

const RULES_SCRIPT := preload("res://rules.gd")
const AI_SCRIPT := preload("res://ai.gd")

const SEAT_RADIUS := 1.15
const CARD_W := 0.09
const CARD_D := 0.13
const PLAYER_SEAT := 0

const CAT_COLORS := [
	Color(0.35, 0.55, 0.85),
	Color(0.40, 0.70, 0.45),
	Color(0.75, 0.65, 0.35),
	Color(0.80, 0.45, 0.65),
	Color(0.80, 0.30, 0.30),
	Color(0.55, 0.35, 0.75),
]
const SEAT_IDLE := Color(0.50, 0.50, 0.55)
const SEAT_SELF := Color(0.90, 0.80, 0.40)
const SEAT_TARGET := Color(0.95, 0.35, 0.35)

var rules: MDRules
var _root: Node3D          # 所有程序化生成的节点都挂在这下面
var hand_root: Node3D
var seat_areas: Array = []
var seat_meshes: Array = []
var seat_labels: Array = []
var status_label: Label
var hint_label: Label
var gather_box: VBoxContainer
var shoot_button: Button

var busy := false
var auto_mode := false

# 目标选择状态
var selecting := false
var select_mode := "card"  # "card" | "shoot"
var select_card := -1
var select_valid: Array = []


func _ready() -> void:
	# 批量统计模式：不起场景，只跑规则 + AI，用来看平衡
	var args := OS.get_cmdline_user_args()
	if "--batch" in args:
		var n := 500
		var bi := args.find("--batch")
		if bi >= 0 and bi + 1 < args.size():
			n = int(args[bi + 1])
		_run_batch(n)
		return

	# 重复进入（编辑器里重新加载脚本）时先把上次生成的清掉，避免叠一层
	var old := get_node_or_null("GEN")
	if old != null:
		remove_child(old)
		old.queue_free()
	_root = Node3D.new()
	_root.name = "GEN"
	self.add_child(_root)

	_build_room()
	_build_ui()

	if Engine.is_editor_hint():
		# 编辑器里只摆静态场景。手牌和状态文字依赖对局状态，不生成。
		gather_box.visible = false
		shoot_button.visible = false
		for s in MDRules.SEATS:
			var who := "你" if s == PLAYER_SEAT else "对手%d" % s
			seat_labels[s].text = "%s\n记忆 --" % who
		return

	auto_mode = "--auto" in OS.get_cmdline_user_args()
	rules = RULES_SCRIPT.new()
	rules.setup(randi())

	_refresh()
	if auto_mode:
		_run_auto()
	else:
		_after_state_change()


# ---------------------------------------------------------------- 场景搭建

func _build_room() -> void:
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-55, -40, 0)
	light.light_energy = 1.2
	_root.add_child(light)

	var env := WorldEnvironment.new()
	var e := Environment.new()
	e.background_mode = Environment.BG_COLOR
	e.background_color = Color(0.07, 0.07, 0.09)
	e.ambient_light_color = Color(0.45, 0.45, 0.5)
	e.ambient_light_energy = 0.8
	env.environment = e
	_root.add_child(env)

	var cam := Camera3D.new()
	cam.look_at_from_position(Vector3(0, 1.02, 1.9), Vector3(0, 0.05, 0))
	# 编辑器里不要 current，否则 3D 视口会被这个相机接管，没法自由观察
	if not Engine.is_editor_hint():
		cam.current = true
	_root.add_child(cam)

	var floor_mesh := MeshInstance3D.new()
	var floor_box := BoxMesh.new()
	floor_box.size = Vector3(8, 0.02, 8)
	floor_mesh.mesh = floor_box
	floor_mesh.position = Vector3(0, -0.5, 0)
	floor_mesh.material_override = _mat(Color(0.10, 0.10, 0.12))
	_root.add_child(floor_mesh)

	var table := MeshInstance3D.new()
	var table_box := BoxMesh.new()
	table_box.size = Vector3(1.7, 0.06, 1.7)
	table.mesh = table_box
	table.material_override = _mat(Color(0.22, 0.17, 0.13))
	_root.add_child(table)

	# 桌面中央那把枪
	var gun := MeshInstance3D.new()
	var gun_box := BoxMesh.new()
	gun_box.size = Vector3(0.16, 0.03, 0.05)
	gun.mesh = gun_box
	gun.position = Vector3(0, 0.05, -0.12)
	gun.material_override = _mat(Color(0.25, 0.25, 0.28))
	_root.add_child(gun)

	for s in MDRules.SEATS:
		var pos := _seat_position(s)

		var area := Area3D.new()
		area.position = pos + Vector3(0, 0.2, 0)
		add_child(area)

		var cs := CollisionShape3D.new()
		var shape := CylinderShape3D.new()
		shape.radius = 0.26
		shape.height = 0.7
		cs.shape = shape
		area.add_child(cs)

		var body := MeshInstance3D.new()
		var cap := CapsuleMesh.new()
		cap.radius = 0.12
		cap.height = 0.42
		body.mesh = cap
		body.material_override = _mat(SEAT_SELF if s == PLAYER_SEAT else SEAT_IDLE)
		area.add_child(body)

		area.input_event.connect(_on_seat_clicked.bind(s))
		seat_areas.append(area)
		seat_meshes.append(body)

		var lbl := Label3D.new()
		lbl.font_size = 48
		lbl.pixel_size = 0.0016
		lbl.billboard = BaseMaterial3D.BILLBOARD_ENABLED
		lbl.position = pos + Vector3(0, 0.62, 0)
		add_child(lbl)
		seat_labels.append(lbl)

	hand_root = Node3D.new()
	hand_root.position = Vector3(0, 0.06, 0.55)
	_root.add_child(hand_root)


func _build_ui() -> void:
	var layer := CanvasLayer.new()
	_root.add_child(layer)

	status_label = Label.new()
	status_label.position = Vector2(18, 14)
	status_label.add_theme_font_size_override("font_size", 18)
	layer.add_child(status_label)

	hint_label = Label.new()
	hint_label.position = Vector2(18, 150)
	hint_label.add_theme_font_size_override("font_size", 20)
	hint_label.add_theme_color_override("font_color", Color(0.95, 0.8, 0.4))
	layer.add_child(hint_label)

	gather_box = VBoxContainer.new()
	gather_box.anchor_left = 0.5
	gather_box.anchor_top = 0.5
	gather_box.anchor_right = 0.5
	gather_box.anchor_bottom = 0.5
	gather_box.offset_left = -70
	gather_box.offset_top = 10
	gather_box.offset_right = 70
	gather_box.offset_bottom = 170
	gather_box.alignment = BoxContainer.ALIGNMENT_CENTER
	layer.add_child(gather_box)

	var title := Label.new()
	title.text = "桌上有枪。你只有一瞬间。"
	title.add_theme_font_size_override("font_size", 22)
	gather_box.add_child(title)
	for i in MDRules.GATHER_NAMES.size():
		var b := Button.new()
		b.text = MDRules.GATHER_NAMES[i]
		b.pressed.connect(_on_gather_pressed.bind(i))
		gather_box.add_child(b)

	shoot_button = Button.new()
	shoot_button.text = "开枪"
	shoot_button.anchor_left = 1.0
	shoot_button.anchor_top = 1.0
	shoot_button.anchor_right = 1.0
	shoot_button.anchor_bottom = 1.0
	shoot_button.offset_left = -110
	shoot_button.offset_top = -60
	shoot_button.offset_right = -20
	shoot_button.offset_bottom = -24
	shoot_button.pressed.connect(_on_shoot_pressed)
	layer.add_child(shoot_button)


func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	return m


func _seat_position(s: int) -> Vector3:
	var a := deg_to_rad(90.0 * float(s))
	return Vector3(sin(a) * SEAT_RADIUS, 0.0, cos(a) * SEAT_RADIUS)


# ---------------------------------------------------------------- 刷新

func _refresh() -> void:
	for s in MDRules.SEATS:
		var p: Dictionary = rules.players[s]
		var who := "你" if s == PLAYER_SEAT else "对手%d" % s
		var tags: Array = []
		if s == rules.gun_holder and rules.bullets > 0:
			tags.append("持枪")
		if not p["alive"]:
			tags.append("出局")
		var suffix := "" if tags.is_empty() else " · " + " ".join(tags)
		seat_labels[s].text = "%s\n记忆 %d%s" % [who, maxi(p["memory"], 0), suffix]

	for child in hand_root.get_children():
		child.queue_free()

	var hand: Array = rules.players[PLAYER_SEAT]["hand"]
	var n := hand.size()
	for i in n:
		var x := (float(i) - float(n - 1) * 0.5) * (CARD_W + 0.015)
		var card := _make_card(hand[i]["cat"], i)
		card.position = Vector3(x, 0.0, -absf(x) * 0.25)
		hand_root.add_child(card)

	_refresh_seat_colors()
	_refresh_gather_panel()
	_refresh_shoot_button()
	_refresh_status()


func _refresh_seat_colors() -> void:
	for s in MDRules.SEATS:
		var base: Color = SEAT_SELF if s == PLAYER_SEAT else SEAT_IDLE
		if not rules.players[s]["alive"]:
			base = Color(0.22, 0.22, 0.24)
		elif selecting and s in select_valid:
			base = SEAT_TARGET
		(seat_meshes[s].material_override as StandardMaterial3D).albedo_color = base


func _refresh_gather_panel() -> void:
	gather_box.visible = rules.is_gather_pending()


func _refresh_shoot_button() -> void:
	var mine: bool = rules.gun_holder == PLAYER_SEAT
	shoot_button.visible = mine and rules.bullets > 0 and not rules.is_over() and rules.can_shoot() \
		and rules.current == PLAYER_SEAT


func _refresh_status() -> void:
	var p: Dictionary = rules.players[PLAYER_SEAT]
	var lines: Array = []

	if rules.is_gather_pending():
		lines.append("抢枪阶段")
	else:
		lines.append("第 %d / %d 轮" % [mini(rules.round_num, MDRules.MAX_ROUNDS), MDRules.MAX_ROUNDS])
	lines.append("你的记忆 %d   (阵营 %d / 身份 %d)" % [
		maxi(p["memory"], 0), MDRules.FACTION_UNLOCK, MDRules.IDENTITY_UNLOCK,
	])

	if rules.gun_holder >= 0:
		var who := "你" if rules.gun_holder == PLAYER_SEAT else "对手%d" % rules.gun_holder
		lines.append("枪在%s手上，剩 %d 发" % [who, rules.bullets])

	var know := rules.self_knowledge(PLAYER_SEAT)
	match know:
		0: lines.append("关于自己：一片空白")
		1: lines.append("关于自己：你隐约知道自己站在哪一边")
		2: lines.append("关于自己：你想起了自己是谁")

	var peeked: Array = p["peeked"]
	if not peeked.is_empty():
		var names: Array = []
		for c in peeked:
			names.append(MDRules.CAT_NAMES[c])
		lines.append("你盯到的一张牌：" + "、".join(names))

	status_label.text = "\n".join(lines)

	if rules.is_over():
		hint_label.text = "对局结束 —— " + ("好人胜" if rules.winner == 0 else "卧底胜")
	elif selecting:
		hint_label.text = "点一个高亮的座位作为目标（再点任意一张牌可取消）"
	elif rules.current == PLAYER_SEAT and rules.can_shoot():
		hint_label.text = "轮到你了：出一张牌，或者开枪"
	elif rules.current == PLAYER_SEAT:
		hint_label.text = "轮到你了：点一张牌打出"
	elif not rules.is_gather_pending():
		hint_label.text = "等待对手…"
	else:
		hint_label.text = ""


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
	area.add_child(lbl)

	area.input_event.connect(_on_card_clicked.bind(index))
	return area


# ---------------------------------------------------------------- 交互

func _on_gather_pressed(choice: int) -> void:
	if not rules.is_gather_pending():
		return
	rules.set_gather(PLAYER_SEAT, choice)
	for s in range(1, MDRules.SEATS):
		rules.set_gather(s, AI_SCRIPT.choose_gather(rules, s))
	_refresh()
	_after_state_change()


func _on_shoot_pressed() -> void:
	if not rules.can_shoot() or rules.gun_holder != PLAYER_SEAT:
		return
	_begin_select("shoot")


func _on_card_clicked(
	_camera: Node, event: InputEvent, _pos: Vector3, _normal: Vector3, _shape: int, index: int
) -> void:
	if busy or rules.is_over():
		return
	if not (event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT):
		return
	if rules.is_gather_pending():
		return

	# 选择中再点牌 = 取消
	if selecting:
		_cancel_select()
		_refresh()
		return

	if rules.current != PLAYER_SEAT or not rules.can_act(PLAYER_SEAT):
		return

	var hand: Array = rules.players[PLAYER_SEAT]["hand"]
	if index < 0 or index >= hand.size():
		return
	var cat: int = hand[index]["cat"]

	if rules.card_needs_target(cat):
		_begin_select("card", index)
		_refresh()
		return

	busy = true
	rules.play(PLAYER_SEAT, index, -1)
	busy = false
	_refresh()
	_after_state_change()


func _on_seat_clicked(
	_camera: Node, event: InputEvent, _pos: Vector3, _normal: Vector3, _shape: int, seat: int
) -> void:
	if not selecting or busy:
		return
	if not (event is InputEventMouseButton and event.pressed and event.button_index == MOUSE_BUTTON_LEFT):
		return
	if not (seat in select_valid):
		return

	var mode := select_mode
	var card := select_card
	_cancel_select()
	busy = true

	if mode == "shoot":
		rules.shoot(seat)
	else:
		rules.play(PLAYER_SEAT, card, seat)

	busy = false
	_refresh()
	_after_state_change()


func _begin_select(mode: String, card_index: int = -1) -> void:
	selecting = true
	select_mode = mode
	select_card = card_index
	select_valid = []
	for s in rules.alive_seats():
		if s != PLAYER_SEAT:
			select_valid.append(s)


func _cancel_select() -> void:
	selecting = false
	select_mode = "card"
	select_card = -1
	select_valid = []


# ---------------------------------------------------------------- 回合推进

func _after_state_change() -> void:
	if auto_mode or rules.is_over():
		return
	if rules.is_gather_pending():
		return
	if rules.current == PLAYER_SEAT:
		return
	_ai_step()


func _ai_step() -> void:
	busy = true
	await get_tree().create_timer(0.45).timeout
	busy = false
	if rules.is_over() or rules.current == PLAYER_SEAT:
		return

	var seat: int = rules.current

	# 持枪者先看要不要开枪
	if rules.can_shoot() and rules.gun_holder == seat:
		var tgt: int = AI_SCRIPT.choose_shoot(rules)
		if tgt >= 0:
			rules.shoot(tgt)
			_refresh()
			_after_state_change()
			return

	var choice: Dictionary = AI_SCRIPT.choose(rules, seat)
	var ok := false
	if choice["index"] >= 0:
		ok = rules.play(seat, choice["index"], choice["target"])
	if not ok:
		players_mark_acted(seat)
	_refresh()
	_after_state_change()


func players_mark_acted(seat: int) -> void:
	# 兜底：某座位打不出牌时不要让整局卡死
	rules.players[seat]["acted"] = true
	rules._advance()


# ---------------------------------------------------------------- 无头自检

## 用 AI 把一局跑完，返回步数（用来发现死循环）
func _simulate(r: MDRules) -> int:
	if r.is_gather_pending():
		for s in MDRules.SEATS:
			r.set_gather(s, AI_SCRIPT.choose_gather(r, s))

	var guard := 0
	while not r.is_over() and guard < 6000:
		guard += 1
		var seat: int = r.current
		if r.can_shoot() and r.gun_holder == seat:
			var tgt: int = AI_SCRIPT.choose_shoot(r)
			if tgt >= 0 and r.shoot(tgt):
				continue
		var choice: Dictionary = AI_SCRIPT.choose(r, seat)
		var ok := false
		if choice["index"] >= 0:
			ok = r.play(seat, choice["index"], choice["target"])
		if not ok:
			r.players[seat]["acted"] = true
			r._advance()
	return guard


func _run_batch(n: int) -> void:
	var good := 0
	var mole := 0
	var stuck := 0
	var rounds_sum := 0
	var timeout_games := 0
	var mem_deaths := 0
	var gun_deaths := 0
	var shots := 0
	var hit_mole := 0
	var hit_good := 0
	var gather_counts := [0, 0, 0]
	var got_faction := 0
	var got_identity := 0
	var player_died := 0
	var t0 := Time.get_ticks_msec()

	for i in n:
		var r: MDRules = RULES_SCRIPT.new()
		r.setup(i * 7919 + 1)

		_simulate(r)

		# 抢枪分布要在结算之后再读，否则 gather_choice 还是 -1
		for s in MDRules.SEATS:
			gather_counts[r.gather_choice[s]] += 1

		if r.is_over():
			if r.winner == 0:
				good += 1
			else:
				mole += 1
		else:
			stuck += 1
		rounds_sum += r.round_num
		if r.round_num >= MDRules.MAX_ROUNDS:
			timeout_games += 1
		mem_deaths += r.mem_deaths
		gun_deaths += r.gun_deaths
		if not r.players[PLAYER_SEAT]["alive"]:
			player_died += 1

		var any_f := false
		var any_i := false
		for s in MDRules.SEATS:
			var m: int = r.players[s]["memory"]
			if m >= MDRules.IDENTITY_UNLOCK:
				any_i = true
			if m >= MDRules.FACTION_UNLOCK:
				any_f = true
		if any_f:
			got_faction += 1
		if any_i:
			got_identity += 1

		for h in r.history:
			if h["kind"] == "shoot":
				shots += 1
				if r.players[h["target"]]["ident"] == MDRules.Ident.MOLE:
					hit_mole += 1
				else:
					hit_good += 1

	var ms := Time.get_ticks_msec() - t0
	var f := float(n)
	print("===== %d 局 AI 自对局 =====" % n)
	print("耗时 %d ms" % ms)
	print("")
	print("【胜负】")
	print("  好人胜 %.1f%%" % (float(good) / f * 100.0))
	print("  卧底胜 %.1f%%" % (float(mole) / f * 100.0))
	if stuck > 0:
		print("  卡住（未结束） %d 局" % stuck)
	print("")
	print("【长度】")
	print("  平均 %.2f 轮" % (float(rounds_sum) / f))
	print("  打满 %d 轮的局 %.1f%%" % [MDRules.MAX_ROUNDS, float(timeout_games) / f * 100.0])
	print("")
	print("【死亡】")
	print("  记忆清零 %.2f 人/局" % (float(mem_deaths) / f))
	print("  枪杀     %.2f 人/局" % (float(gun_deaths) / f))
	print("  你（座位0）出局率 %.1f%%" % (float(player_died) / f * 100.0))
	print("")
	print("【枪】")
	print("  开枪 %.2f 次/局" % (float(shots) / f))
	if shots > 0:
		print("  命中卧底 %.1f%%（打中卧底 %d / 打死好人 %d）" % [
			float(hit_mole) / float(shots) * 100.0, hit_mole, hit_good,
		])
	print("")
	print("【抢枪阶段的选择分布】")
	var total_choices := n * MDRules.SEATS
	for i in 3:
		print("  %s %.1f%%" % [
			MDRules.GATHER_NAMES[i], float(gather_counts[i]) / float(total_choices) * 100.0,
		])
	print("")
	print("【自我认知】")
	print("  有人解锁阵营（记忆>=%d）的局 %.1f%%" % [MDRules.FACTION_UNLOCK, float(got_faction) / f * 100.0])
	print("  有人解锁身份（记忆>=%d）的局 %.1f%%" % [MDRules.IDENTITY_UNLOCK, float(got_identity) / f * 100.0])
	get_tree().quit(0)


func _run_auto() -> void:
	if rules.is_gather_pending():
		for s in MDRules.SEATS:
			rules.set_gather(s, AI_SCRIPT.choose_gather(rules, s))

	var guard := 0
	while not rules.is_over() and guard < 6000:
		guard += 1
		var seat: int = rules.current
		if rules.can_shoot() and rules.gun_holder == seat:
			var tgt: int = AI_SCRIPT.choose_shoot(rules)
			if tgt >= 0 and rules.shoot(tgt):
				continue
		var choice: Dictionary = AI_SCRIPT.choose(rules, seat)
		var ok := false
		if choice["index"] >= 0:
			ok = rules.play(seat, choice["index"], choice["target"])
		if not ok:
			players_mark_acted(seat)

	print("AUTO_RESULT winner=%d rounds=%d guard=%d gun_holder=%d bullets=%d" % [
		rules.winner, rules.round_num, guard, rules.gun_holder, rules.bullets,
	])
	for s in MDRules.SEATS:
		var p: Dictionary = rules.players[s]
		print("  seat %d ident=%s memory=%d alive=%s" % [
			s, MDRules.IDENT_NAMES[p["ident"]], maxi(p["memory"], 0), str(p["alive"]),
		])
	_refresh()
	await get_tree().process_frame
	get_tree().quit(0 if rules.is_over() else 1)
