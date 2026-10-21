#include "md_local_server.h"

#include <string>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "md/net/wire.hpp"

namespace godot {

MDLocalServer::MDLocalServer() = default;
MDLocalServer::~MDLocalServer() = default;

void MDLocalServer::_bind_methods() {
  ClassDB::bind_method(D_METHOD("create", "seed", "max_rounds"), &MDLocalServer::create);
  ClassDB::bind_method(D_METHOD("start", "seed", "max_rounds"), &MDLocalServer::start);
  ClassDB::bind_method(D_METHOD("send", "conn", "line"), &MDLocalServer::send);
  ClassDB::bind_method(D_METHOD("disconnect", "conn"), &MDLocalServer::disconnect);
  ClassDB::bind_method(D_METHOD("is_over"), &MDLocalServer::is_over);
  ClassDB::bind_method(D_METHOD("result_json"), &MDLocalServer::result_json);

  ADD_SIGNAL(MethodInfo("delivery", PropertyInfo(Variant::INT, "conn"),
                        PropertyInfo(Variant::STRING, "line")));
}

void MDLocalServer::enqueue(std::vector<md::net::Delivery>& out) {
  for (auto& d : out) queue_.push_back(std::move(d));
  out.clear();
}

void MDLocalServer::flush() {
  // 防重入：GDScript 在 delivery 回调里直接调 send() 是正常用法，
  // 此时不能递归进 Room，把新产生的投递攒到队列里，由外层循环继续排空。
  if (flushing_) return;
  flushing_ = true;
  while (!queue_.empty()) {
    const md::net::Delivery d = queue_.front();  // 拷贝，防止 emit 期间 vector 重分配
    queue_.erase(queue_.begin());
    emit_signal("delivery", d.conn, String(d.line.c_str()));
  }
  flushing_ = false;
}

void MDLocalServer::start(int64_t seed, int64_t max_rounds) {
  create(seed, max_rounds);
  if (!room_) return;

  std::vector<md::net::Delivery> out;
  room_->on_message(0, "{\"t\":\"join\"}", out);
  room_->on_message(0, "{\"t\":\"start\"}", out);
  enqueue(out);
  flush();
}

void MDLocalServer::create(int64_t seed, int64_t max_rounds) {
  room_ = std::make_unique<md::net::Room>(static_cast<uint64_t>(seed),
                                          static_cast<int>(max_rounds));
  queue_.clear();
}

void MDLocalServer::send(int64_t conn, const String& line) {
  if (!room_) return;
  const CharString utf8 = line.utf8();
  std::vector<md::net::Delivery> out;
  room_->on_message(static_cast<int>(conn),
                    std::string_view(utf8.get_data(), static_cast<std::size_t>(utf8.length())),
                    out);
  enqueue(out);
  flush();
}

void MDLocalServer::disconnect(int64_t conn) {
  if (!room_) return;
  std::vector<md::net::Delivery> out;
  room_->on_disconnect(static_cast<int>(conn), out);
  enqueue(out);
  flush();
}

bool MDLocalServer::is_over() const { return room_ && room_->over(); }

String MDLocalServer::result_json() const {
  if (!room_) return String("{}");
  const md::MatchResult& r = room_->result();
  return String(md::net::encode_result(r.winner, r.rounds, r.mole_alive).c_str());
}

}  // namespace godot
