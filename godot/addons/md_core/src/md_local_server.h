// GDExtension 胶水层：把 md::net::Room 暴露给 GDScript。
//
// 关键点：这里暴露的不是 md::Session，而是 md::net::Room ——
// 也就是服务器用的同一个对象。于是单机（进程内）和联机（TCP）在
// 客户端看来完全一致：都是一条条 JSON 行，只是行的来源不同。
#ifndef MD_LOCAL_SERVER_H
#define MD_LOCAL_SERVER_H

#include <memory>
#include <vector>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "md/net/room.hpp"

namespace godot {

class MDLocalServer : public RefCounted {
  GDCLASS(MDLocalServer, RefCounted)

  std::unique_ptr<md::net::Room> room_;
  std::vector<md::net::Delivery> queue_;
  bool flushing_ = false;

  void enqueue(std::vector<md::net::Delivery>& out);
  void flush();

 protected:
  static void _bind_methods();

 public:
  MDLocalServer();
  ~MDLocalServer() override;

  // 只建房、不自动入座。让本地模式也能像联机那样由客户端自己发 join / start，
  // 这样两种 transport 的时序完全一致。
  void create(int64_t seed, int64_t max_rounds);

  // 开一局：conn 0 是本地玩家，座位 1..3 由内置 AI 接管。
  // 等价于 create() + 自动发 join + start，图省事用。
  void start(int64_t seed, int64_t max_rounds);

  // 等同于网络上传入一行 JSON。所有合法性校验都在 Room 里。
  void send(int64_t conn, const String& line);

  void disconnect(int64_t conn);

  bool is_over() const;
  String result_json() const;
};

}  // namespace godot

#endif  // MD_LOCAL_SERVER_H
