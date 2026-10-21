#include "md/net/room.hpp"

#include <algorithm>

#include "md/net/wire.hpp"

namespace md::net {

Room::Room(uint64_t seed, int max_rounds)
    : seed_(seed), max_rounds_(max_rounds), ai_rng_(seed ^ 0xA5A5A5A5ull) {}

bool Room::over() const { return session_ && session_->over(); }

const md::MatchResult& Room::result() const {
  static const md::MatchResult kEmpty{};
  return session_ ? session_->result() : kEmpty;
}

int Room::seat_of_conn(int conn) const {
  for (int s = 0; s < md::kSeats; ++s)
    if (seats_[s].occupied && seats_[s].conn == conn) return s;
  return -1;
}

Room::TurnInfo Room::turn() const {
  TurnInfo t;
  if (!session_ || session_->over()) return t;
  const md::Pending p = session_->pending();
  t.seat = p.seat;
  t.cards = p.cards;
  t.targets = p.targets;
  switch (p.kind) {
    case md::DecisionKind::Gather: t.kind = "gather"; break;
    case md::DecisionKind::Gun: t.kind = "gun"; break;
    case md::DecisionKind::Play: t.kind = "play"; break;
    default: break;
  }
  return t;
}

void Room::send(int seat, const std::string& line, std::vector<Delivery>& out) {
  if (seat < 0 || seat >= md::kSeats) return;
  const int conn = seats_[seat].conn;
  if (conn < 0) return;  // AI 座位不需要投递
  out.push_back(Delivery{conn, line});
}

void Room::broadcast(std::vector<Delivery>& out) {
  if (!session_) return;
  const md::Pending p = session_->pending();
  for (int s = 0; s < md::kSeats; ++s) {
    if (seats_[s].conn < 0) continue;
    const md::PlayerView v = session_->view_of(s);
    const md::Pending* pp = (p.seat == s) ? &p : nullptr;
    send(s, encode_view(v, pp), out);
  }

  if (session_->over() && !announced_over_) {
    announced_over_ = true;
    const md::MatchResult& r = session_->result();
    const std::string line = encode_result(r.winner, r.rounds, r.mole_alive);
    for (int s = 0; s < md::kSeats; ++s) send(s, line, out);
  }
}

void Room::pump_ai() {
  if (!session_) return;
  int guard = 0;
  while (!session_->over() && guard++ < 4000) {
    const md::Pending p = session_->pending();
    if (p.kind == md::DecisionKind::Over) break;
    if (!seat_is_ai(p.seat)) break;

    const md::PlayerView v = session_->view_of(p.seat);
    const md::Command c = md::ai_decide(v, p, ai_rng_);
    if (!session_->submit(c)) {
      md::Command fb;
      fb.kind = p.kind;
      fb.seat = p.seat;
      fb.a = (p.kind == md::DecisionKind::Play || p.kind == md::DecisionKind::Gun) ? -1 : 1;
      if (!session_->submit(fb)) break;
    }
  }
}

void Room::fill_with_ai() {
  for (int s = 0; s < md::kSeats; ++s) {
    if (!seats_[s].occupied) {
      seats_[s].occupied = true;
      seats_[s].ai = true;
      seats_[s].conn = -1;
    }
  }
}

void Room::start_game(std::vector<Delivery>& out) {
  if (session_) return;
  session_ = std::make_unique<md::Session>(seed_, max_rounds_);
  session_->setup();
  pump_ai();
  broadcast(out);
}

void Room::start_ai_only(std::vector<Delivery>& out) {
  fill_with_ai();
  start_game(out);
}

void Room::dispatch_cmd(int conn, std::string_view line, std::vector<Delivery>& out) {
  const int seat = seat_of_conn(conn);
  auto err = [&](const char* m) { out.push_back(Delivery{conn, encode_error(m)}); };

  if (!session_) { err("not started"); return; }
  if (seat < 0) { err("not seated"); return; }
  if (session_->over()) { err("game over"); return; }

  const md::Pending p = session_->pending();
  if (p.seat != seat) { err("not your turn"); return; }

  md::Command c;
  if (!decode_command(line, c)) { err("bad command"); return; }

  // 座位一律以连接为准，绝不信任客户端自报的 seat
  c.seat = seat;
  if (c.kind != p.kind) { err("wrong decision kind"); return; }
  if (!session_->submit(c)) { err("illegal move"); return; }

  pump_ai();
  broadcast(out);
}

void Room::on_message(int conn, std::string_view line, std::vector<Delivery>& out) {
  FlatJson f;
  if (!f.parse(line)) {
    out.push_back(Delivery{conn, encode_error("bad json")});
    return;
  }
  const std::string t = f.str("t");

  if (t == "join") {
    const int existing = seat_of_conn(conn);
    if (existing >= 0) {
      out.push_back(Delivery{conn, encode_joined(existing)});
      return;
    }
    if (session_) {
      out.push_back(Delivery{conn, encode_error("game already started")});
      return;
    }
    for (int s = 0; s < md::kSeats; ++s) {
      if (!seats_[s].occupied) {
        seats_[s].occupied = true;
        seats_[s].ai = false;
        seats_[s].conn = conn;
        out.push_back(Delivery{conn, encode_joined(s)});
        bool full = true;
        for (int i = 0; i < md::kSeats; ++i) full = full && seats_[i].occupied;
        if (full) start_game(out);
        return;
      }
    }
    out.push_back(Delivery{conn, encode_error("room full")});
    return;
  }

  if (t == "fill" || t == "start") {
    if (session_) {
      out.push_back(Delivery{conn, encode_error("game already started")});
      return;
    }
    if (seat_of_conn(conn) < 0) {
      out.push_back(Delivery{conn, encode_error("join first")});
      return;
    }
    fill_with_ai();
    start_game(out);
    return;
  }

  if (t == "cmd") {
    dispatch_cmd(conn, line, out);
    return;
  }

  out.push_back(Delivery{conn, encode_error("unknown message")});
}

void Room::on_disconnect(int conn, std::vector<Delivery>& out) {
  const int seat = seat_of_conn(conn);
  if (seat < 0) return;

  seats_[seat].conn = -1;
  if (session_ && !session_->over()) {
    // 对局进行中掉线：交给 AI，别让其他三个人卡死
    seats_[seat].ai = true;
    seats_[seat].occupied = true;
    pump_ai();
    broadcast(out);
  } else {
    seats_[seat].occupied = false;
    seats_[seat].ai = false;
  }
}

}  // namespace md::net
