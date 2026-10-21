extends Node
#
# 客户端层自检。分两部分：
#
#   A. 不依赖任何外部二进制：MDClient 的协议状态机、MDClientHost 的接线
#   B. 需要外部条件：本地 GDExtension、stdio 子进程、TCP loopback
#      —— 条件不满足时**显式跳过**并说明原因，不算通过
#
# 跑法：godot --headless --path godot
# 可用环境变量 MD_STDIO_EXE 指定 stdio 服务器位置。

var fails := 0
var skips := 0
var passes := 0


func _ready() -> void:
	print("---- 客户端层自检 ----")
	print("引擎: %s" % Engine.get_version_info().string)

	_test_client_protocol()
	_test_host_wiring()
	_test_local_extension()
	await _test_stdio_transport()
	await _test_tcp_loopback()

	print("\n--------------------------------------------------")
	print("通过 %d 项，跳过 %d 项，失败 %d 项" % [passes, skips, fails])
	if skips > 0:
		print("（跳过的项目说明外部条件不足，不计入通过）")
	print("自检结果: ", "PASS" if fails == 0 else "FAIL")
	get_tree().quit(0 if fails == 0 else 1)


func _ok(what: String) -> void:
	passes += 1
	print("  [ok]   ", what)


func _bad(what: String) -> void:
	fails += 1
	print("  [FAIL] ", what)


func _skip(what: String, why: String) -> void:
	skips += 1
	print("  [skip] %s  —— %s" % [what, why])


func _check(what: String, cond: bool) -> void:
	if cond:
		_ok(what)
	else:
		_bad(what)


# ---------------------------------------------------------------------------
# A1. MDClient 协议状态机（用真实编码格式的样例行）
# ---------------------------------------------------------------------------
const VIEW_OK := '{"t":"view","seat":0,"round":2,"memory":5,"alive":true,' \
	+ '"faction_known":false,"faction_is_mole":false,"identity_known":false,"identity":0,' \
	+ '"gun_holder":2,"bullets":1,' \
	+ '"hand":[{"ci":1,"imp":false},{"ci":4,"imp":true},{"ci":0,"imp":false}],' \
	+ '"pub_memory":[5,3,7,4],"pub_alive":[true,true,true,true],"pub_hand":[3,2,5,4],' \
	+ '"history":[{"s":2,"r":1,"ci":4},{"s":1,"r":1,"ci":2}],"peeked":[3],' \
	+ '"act":{"k":"play","cards":[0,1,2],"targets":[1,2,3]}}'

# 同样一份 view，但 pub_hand[0] 说有 4 张，实际只发了 3 张 —— 泄露
const VIEW_LEAK := '{"t":"view","seat":0,"round":2,"memory":5,"alive":true,' \
	+ '"faction_known":false,"faction_is_mole":false,"identity_known":false,"identity":0,' \
	+ '"gun_holder":2,"bullets":1,' \
	+ '"hand":[{"ci":1,"imp":false},{"ci":4,"imp":true},{"ci":0,"imp":false}],' \
	+ '"pub_memory":[5,3,7,4],"pub_alive":[true,true,true,true],"pub_hand":[4,2,5,4],' \
	+ '"history":[],"peeked":[],"act":null}'


func _test_client_protocol() -> void:
	print("\n[A1] MDClient 协议状态机")

	var c := MDClient.new()
	var cmds: Array[String] = []
	c.command_ready.connect(func(cmd): cmds.append(cmd))
	c.feed('{"t":"joined","seat":0}')
	c.feed(VIEW_OK)

	_check("joined 记录座位", c.seat == 0)
	_check("view 计数", c.views == 1)
	_check("记忆值", c.memory == 5)
	_check("轮次", c.round == 2)
	_check("手牌 3 张", c.hand.size() == 3)
	_check("公共信息 4 人", c.pub_hand.size() == 4)
	_check("历史出牌 2 条", c.history.size() == 2)
	_check("act 被解析", c.act != null and typeof(c.act) == TYPE_DICTIONARY)
	_check("手牌未泄露", not c.leak)
	_check("轮到行动时给出了命令", cmds.size() == 1)

	if cmds.size() == 1:
		var d = JSON.parse_string(cmds[0])
		_check("命令是合法 JSON", typeof(d) == TYPE_DICTIONARY)
		if typeof(d) == TYPE_DICTIONARY:
			_check("命令类型 play", str(d.get("t")) == "cmd" and str(d.get("k")) == "play")
			_check("命令带上了手牌与目标", int(d.get("a", -99)) == 0 and int(d.get("b", -99)) == 1)

	var c2 := MDClient.new()
	c2.feed('{"t":"joined","seat":0}')
	c2.feed(VIEW_LEAK)
	_check("检测到手牌张数不一致", c2.leak)

	var c3 := MDClient.new()
	var over: Array[Dictionary] = []
	c3.game_over.connect(func(r): over.append(r))
	c3.feed('{"t":"over","winner":"good","rounds":6,"mole_alive":false}')
	_check("over 置 done", c3.done)
	_check("over 触发信号", over.size() == 1)

	var c4 := MDClient.new()
	var errs: Array[String] = []
	c4.error_received.connect(func(m): errs.append(m))
	c4.feed('{"t":"error","msg":"not your turn"}')
	c4.feed('这不是 JSON')
	c4.feed('{"t":"mystery"}')
	_check("服务器错误被上报", errs.size() >= 1 and errs[0] == "not your turn")
	_check("非法 JSON 被上报", errs.size() >= 2)
	_check("未知消息类型被上报", errs.size() == 3)

	var c5 := MDClient.new()
	var got: Array[String] = []
	c5.policy = func(_act, _self): return '{"t":"cmd","k":"gather","a":2}'
	c5.command_ready.connect(func(cmd): got.append(cmd))
	c5.feed(VIEW_OK)
	_check("自定义策略生效", got.size() == 1 and got[0].contains('"a":2'))


# ---------------------------------------------------------------------------
# A2. MDClientHost 的接线：两种 transport 共用同一处
# ---------------------------------------------------------------------------
class FakeLink extends RefCounted:
	signal line_received(conn: int, line: String)
	signal connected
	var sent: Array[String] = []

	func send(_conn: int, line: String) -> void:
		sent.append(line)

	func poll() -> void:
		pass

	func fire_connected() -> void:
		connected.emit()


func _test_host_wiring() -> void:
	print("\n[A2] MDClientHost 接线（假 transport）")
	var link := FakeLink.new()
	var host := MDClientHost.new(link)

	link.fire_connected()
	_check("连上后发 join", link.sent.size() >= 1 and link.sent[0].contains('"join"'))
	_check("连上后发 start", link.sent.size() >= 2 and link.sent[1].contains('"start"'))

	link.sent.clear()
	link.line_received.emit(0, '{"t":"joined","seat":0}')
	link.line_received.emit(0, VIEW_OK)
	_check("收到 view 后把命令交回 transport", link.sent.size() == 1)
	if link.sent.size() == 1:
		_check("交回的是 play 命令", link.sent[0].contains('"k":"play"'))


# ---------------------------------------------------------------------------
# B. 需要外部条件的部分
# ---------------------------------------------------------------------------
func _find_stdio_exe() -> String:
	var candidates: Array[String] = []
	var env := OS.get_environment("MD_STDIO_EXE")
	if env != "":
		candidates.append(env)
	var repo := ProjectSettings.globalize_path("res://").path_join("..").simplify_path()
	for rel in [
		"build/md_stdio", "build/md_stdio.exe",
		"build-gdext/Release/md_stdio.exe", "build-gdext/md_stdio",
	]:
		candidates.append(repo.path_join(rel))
	for c in candidates:
		if FileAccess.file_exists(c):
			return c
	return ""


func _test_local_extension() -> void:
	print("\n[B1] 本地 transport（GDExtension 进程内）")
	if not ClassDB.class_exists("MDLocalServer"):
		_skip("本地 transport", "MDLocalServer 未注册：GDExtension 没有加载")
		return

	var link := MDLocalLink.new()
	var host := MDClientHost.new(link)
	var failed: Array[String] = []
	link.connect_failed.connect(func(m): failed.append(m))

	link.open(7, 8)
	if not failed.is_empty():
		_skip("本地 transport", failed[0])
		return

	_check("跑完整局", host.client.done)
	_check("拿到座位号", host.client.seat == 0)
	_check("有历史出牌记录", host.client.history.size() > 0)
	_check("手牌未泄露", not host.client.leak)


func _test_stdio_transport() -> void:
	print("\n[B2] stdio transport（真子进程）")
	var exe := _find_stdio_exe()
	if exe == "":
		_skip("stdio transport", "找不到 md_stdio 可执行文件")
		return
	print("     使用: ", exe)

	var link := MDStdioLink.new()
	var host := MDClientHost.new(link)
	var failed: Array[String] = []
	link.connect_failed.connect(func(m): failed.append(m))

	if link.open(exe, 5, 8) != OK:
		_skip("stdio transport", "子进程启动失败：" + ", ".join(failed))
		return

	var guard := 0
	while not host.client.done and guard < 200:
		guard += 1
		link.poll()

	_check("跑完整局", host.client.done)
	_check("拿到座位号", host.client.seat == 0)
	_check("手牌未泄露", not host.client.leak)
	link.shutdown()


func _test_tcp_loopback() -> void:
	print("\n[B3] TCP transport（真 socket）")
	if not ClassDB.class_exists("MDLocalServer"):
		_skip("TCP transport", "对端需要 MDLocalServer 驱动规则，当前不可用")
		return

	var relay := LoopbackRelay.new(11, 8)
	if not relay.ok:
		_skip("TCP transport", "监听端口失败")
		return

	var link := MDTcpLink.new()
	var host := MDClientHost.new(link)
	var failed: Array[String] = []
	link.connect_failed.connect(func(m): failed.append(m))

	if link.open("127.0.0.1", relay.port) != OK:
		_skip("TCP transport", "发起连接失败")
		return

	var frames := 0
	while not host.client.done and frames < 3000:
		await get_tree().process_frame
		relay.poll()
		link.poll()
		frames += 1

	_check("跑完整局", host.client.done)
	_check("拿到座位号", host.client.seat == 0)
	_check("手牌未泄露", not host.client.leak)
	if not failed.is_empty():
		print("     连接错误: ", failed)


# ---------------------------------------------------------------------------
# 对端：把 TCP 连接桥接到同一个 MDLocalServer（用动态取类，扩展缺失时不影响解析）
# ---------------------------------------------------------------------------
class LoopbackRelay:
	var server := TCPServer.new()
	var srv = null
	var peer: StreamPeerTCP = null
	var buf := ""
	var pending: Array[String] = []
	var port := 0
	var ok := false

	func _init(seed: int, rounds: int) -> void:
		ok = server.listen(0, "127.0.0.1") == OK
		if not ok:
			return
		port = server.get_local_port()
		srv = ClassDB.instantiate("MDLocalServer")
		if srv == null:
			ok = false
			return
		srv.delivery.connect(_on_delivery)
		srv.create(seed, rounds)

	func _on_delivery(conn: int, line: String) -> void:
		if conn != 0:
			return
		if peer == null:
			pending.append(line)
		else:
			peer.put_data((line + "\n").to_utf8_buffer())

	func poll() -> void:
		if peer == null:
			if server.is_connection_available():
				peer = server.take_connection()
				for line in pending:
					peer.put_data((line + "\n").to_utf8_buffer())
				pending.clear()
			return

		var avail := peer.get_available_bytes()
		if avail <= 0:
			return
		var res := peer.get_data(avail)
		if int(res[0]) != OK:
			return
		buf += (res[1] as PackedByteArray).get_string_from_utf8()
		while true:
			var nl := buf.find("\n")
			if nl < 0:
				break
			var line := buf.substr(0, nl)
			buf = buf.substr(nl + 1)
			line = line.strip_edges()
			if line != "":
				srv.send(0, line)
