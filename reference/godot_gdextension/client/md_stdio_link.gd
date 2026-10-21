class_name MDStdioLink
extends RefCounted
#
# 进程级 transport：把 C++ 服务器当成子进程，用 stdin/stdout 通信。
#
# 与 MDLocalLink / MDTcpLink 接口完全一致，所以 MDClient 与 MDClientHost
# 一行都不用改。
#
# 为什么需要它：某些环境下 GDExtension 的 dll 会被代码完整性策略拦掉，
# ws2_32 也会被拦，但普通 exe 可以用。这条路径让"客户端 ↔ 服务器"仍然
# 真的跨了进程边界。
#
# 帧协议比 TCP 版多一个同步哨兵 {"t":"sync"}：服务器处理完一条命令后必发，
# 客户端读到它就知道本轮响应结束。因为这个 FileAccess 的读是阻塞的。

signal line_received(conn: int, line: String)
signal connected
signal connect_failed(reason: String)

const SYNC := '{"t":"sync"}'

var _f: FileAccess = null
var _pid := -1
var _busy := false
var _queue: Array[String] = []


func open(exe_path: String, seed: int, rounds: int) -> Error:
	var r: Dictionary = OS.execute_with_pipe(
		exe_path, ["--seed", str(seed), "--rounds", str(rounds)]
	)
	if r.is_empty() or not r.has("stdio") or r["stdio"] == null:
		connect_failed.emit("无法启动子进程: " + exe_path)
		return FAILED
	_f = r["stdio"]
	_pid = int(r.get("pid", -1))

	var hello := _f.get_line().strip_edges()
	if hello != '{"t":"ready"}':
		connect_failed.emit("子进程没有发 ready: " + hello)
		return FAILED

	connected.emit()
	return OK


func send(_conn: int, line: String) -> void:
	if _f == null:
		return
	_queue.append(line)
	if _busy:
		return  # 正在读响应，交给外层循环处理，避免嵌套读造成死锁

	_busy = true
	while not _queue.is_empty():
		var cmd: String = _queue.pop_front()
		_f.store_line(cmd)
		_f.flush()

		var guard := 0
		while guard < 20000:
			guard += 1
			var raw := _f.get_line()
			var t := raw.strip_edges()
			if t == "":
				if _f.get_error() != OK:
					break
				continue
			if t == SYNC:
				break
			line_received.emit(0, t)
	_busy = false


func poll() -> void:
	pass


func is_over() -> bool:
	return false


func shutdown() -> void:
	if _f != null:
		_f.close()
		_f = null
	if _pid > 0:
		OS.kill(_pid)
		_pid = -1
