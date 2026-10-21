class_name MDLocalLink
extends RefCounted
#
# 本地 transport：把 MDLocalServer（GDExtension，进程内）包装成和 TCP 一样的接口。
#
# 关键点是它**不自动入座**——和联机一样，由 MDClientHost 在 connected 之后
# 自己发 join / start。这样两种 transport 的时序完全一致。
#
# 注意：这里用 ClassDB.instantiate 动态取类，而不是直接写 MDLocalServer.new()。
# 因为当 GDExtension 没能加载（比如被代码完整性策略拦下）时，直接引用这个
# 类型会让**整个脚本编译失败**，连"检测到不可用并报错"都做不到。

signal line_received(conn: int, line: String)
signal connected
signal connect_failed(reason: String)

var _srv = null


func open(seed: int, rounds: int) -> void:
	if not ClassDB.class_exists("MDLocalServer"):
		connect_failed.emit("MDLocalServer 未注册：GDExtension 没有加载")
		return

	_srv = ClassDB.instantiate("MDLocalServer")
	if _srv == null:
		connect_failed.emit("MDLocalServer 实例化失败")
		return

	_srv.delivery.connect(_on_delivery)
	_srv.create(seed, rounds)
	connected.emit()


func _on_delivery(conn: int, line: String) -> void:
	line_received.emit(conn, line)


func send(conn: int, line: String) -> void:
	if _srv != null:
		_srv.send(conn, line)


func is_over() -> bool:
	return _srv != null and _srv.is_over()


func result_json() -> String:
	return _srv.result_json() if _srv != null else "{}"


## 本地 transport 没有网络延迟，不需要轮询
func poll() -> void:
	pass
