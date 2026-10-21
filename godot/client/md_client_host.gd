class_name MDClientHost
extends RefCounted
#
# 把 MDClient 和任意 transport 绑起来。
#
# 这个文件就是"单机与联机走同一套协议"的兑现处：
#   _on_line()    —— 把任意来源的一行喂给 MDClient
#   _on_command() —— 把 MDClient 的决策交给当前 transport
#   _on_connected() —— 两种 transport 都在这里发 join / start
#
# 除了这三处，单机和联机没有任何分叉。

var client: MDClient
var _link


func _init(link) -> void:
	_link = link
	client = MDClient.new()
	_link.line_received.connect(_on_line)
	_link.connected.connect(_on_connected)
	client.command_ready.connect(_on_command)


func _on_connected() -> void:
	_link.send(0, '{"t":"join"}')
	_link.send(0, '{"t":"start"}')


func _on_line(conn: int, line: String) -> void:
	client.feed(line)


func _on_command(cmd: String) -> void:
	_link.send(0, cmd)


## 本地 transport 同步完成；TCP 需要每帧调用
func poll() -> void:
	_link.poll()
