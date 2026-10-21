class_name MDTcpLink
extends RefCounted
#
# 远程 transport：换行分隔的 JSON over TCP。
#
# 与 MDLocalLink 的接口完全一致，所以 MDClient 和 MDClientHost 一行都不用改。
# 帧格式和 apps/md_server 完全对应（每行一条 JSON）。

signal line_received(conn: int, line: String)
signal connected
signal connect_failed(reason: String)

const LOCAL_CONN := 0

var _peer := StreamPeerTCP.new()
var _buf := ""
var _was_connected := false


func open(host: String, port: int) -> Error:
	return _peer.connect_to_host(host, port)


func poll() -> void:
	_peer.poll()
	var status := _peer.get_status()

	if status == StreamPeerTCP.STATUS_CONNECTED:
		if not _was_connected:
			_was_connected = true
			connected.emit()
		var avail := _peer.get_available_bytes()
		if avail > 0:
			var res := _peer.get_data(avail)
			if int(res[0]) == OK:
				_buf += (res[1] as PackedByteArray).get_string_from_utf8()
				_drain()
	elif status == StreamPeerTCP.STATUS_ERROR:
		if not _was_connected:
			connect_failed.emit("connection error")


func _drain() -> void:
	while true:
		var nl := _buf.find("\n")
		if nl < 0:
			return
		var line := _buf.substr(0, nl)
		_buf = _buf.substr(nl + 1)
		line = line.strip_edges()
		if line != "":
			line_received.emit(LOCAL_CONN, line)


func send(conn: int, line: String) -> void:
	if _peer.get_status() == StreamPeerTCP.STATUS_CONNECTED:
		_peer.put_data((line + "\n").to_utf8_buffer())


func is_connected() -> bool:
	return _peer.get_status() == StreamPeerTCP.STATUS_CONNECTED


## "是否结束"由 MDClient 判定，transport 不关心
func is_over() -> bool:
	return false
