#include "md/net/wire.hpp"

#include <cstdint>
#include <utility>

namespace md::net {

namespace {

struct Reader {
  std::string_view s;
  std::size_t i = 0;

  bool eof() const { return i >= s.size(); }
  char peek() const { return eof() ? '\0' : s[i]; }

  void skip_ws() {
    while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
  }

  bool eat(char c) {
    skip_ws();
    if (!eof() && s[i] == c) { ++i; return true; }
    return false;
  }

  bool parse_string(std::string& out) {
    skip_ws();
    if (eof() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (!eof()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') { out.push_back(c); continue; }
      if (eof()) return false;
      const char e = s[i++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (i + 4 > s.size()) return false;
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s[i + k];
            cp <<= 4;
            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
            else return false;
          }
          i += 4;
          // 只需要处理 BMP，直接写出 UTF-8
          if (cp < 0x80) {
            out.push_back(char(cp));
          } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool parse_value(FlatValue& v) {
    skip_ws();
    if (eof()) return false;
    if (peek() == '"') {
      v.kind = FlatValue::Kind::String;
      return parse_string(v.text);
    }
    const std::size_t start = i;
    while (!eof() && s[i] != ',' && s[i] != '}' && s[i] != ' ' && s[i] != '\t' && s[i] != '\r' &&
           s[i] != '\n')
      ++i;
    const std::string_view raw = s.substr(start, i - start);
    if (raw.empty()) return false;
    if (raw == "true") { v.kind = FlatValue::Kind::Boolean; v.boolean = true; return true; }
    if (raw == "false") { v.kind = FlatValue::Kind::Boolean; v.boolean = false; return true; }
    if (raw == "null") { v.kind = FlatValue::Kind::Number; v.number = 0; return true; }

    // 整数解析：不用 std::stol，避免异常与本地化问题
    long acc = 0;
    bool neg = false;
    std::size_t k = 0;
    if (raw[0] == '-') { neg = true; k = 1; }
    if (k >= raw.size()) return false;
    for (; k < raw.size(); ++k) {
      const char c = raw[k];
      if (c < '0' || c > '9') break;  // 容忍小数尾巴，直接截断
      acc = acc * 10 + (c - '0');
    }
    v.kind = FlatValue::Kind::Number;
    v.number = neg ? -acc : acc;
    return true;
  }
};

void append_int(std::string& o, int v) { o += std::to_string(v); }

void append_bool(std::string& o, bool v) { o += v ? "true" : "false"; }

}  // namespace

std::string json_quote(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 2);
  o.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          o += "\\u00";
          o.push_back(hex[(c >> 4) & 0xF]);
          o.push_back(hex[c & 0xF]);
        } else {
          o.push_back(c);
        }
    }
  }
  o.push_back('"');
  return o;
}

bool FlatJson::parse(std::string_view text) {
  Reader r{text, 0};
  kv_.clear();
  if (!r.eat('{')) return false;
  if (r.eat('}')) return true;
  for (;;) {
    std::string key;
    if (!r.parse_string(key)) return false;
    if (!r.eat(':')) return false;
    FlatValue v;
    if (!r.parse_value(v)) return false;
    kv_.emplace_back(std::move(key), std::move(v));
    if (r.eat(',')) continue;
    if (r.eat('}')) return true;
    return false;
  }
}

const FlatValue* FlatJson::find(std::string_view key) const {
  for (const auto& kv : kv_)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

long FlatJson::num(std::string_view key, long def) const {
  const FlatValue* v = find(key);
  if (!v) return def;
  if (v->kind == FlatValue::Kind::Number) return v->number;
  if (v->kind == FlatValue::Kind::Boolean) return v->boolean ? 1 : 0;
  return def;
}

bool FlatJson::boolean(std::string_view key, bool def) const {
  const FlatValue* v = find(key);
  if (!v) return def;
  if (v->kind == FlatValue::Kind::Boolean) return v->boolean;
  if (v->kind == FlatValue::Kind::Number) return v->number != 0;
  return def;
}

std::string FlatJson::str(std::string_view key, std::string_view def) const {
  const FlatValue* v = find(key);
  if (!v) return std::string(def);
  if (v->kind == FlatValue::Kind::String) return v->text;
  if (v->kind == FlatValue::Kind::Number) return std::to_string(v->number);
  return v->boolean ? "true" : "false";
}

// ------------------------------------------------------------------ 编码

std::string encode_view(const PlayerView& v, const Pending* pending) {
  std::string o;
  o.reserve(768);
  o += "{\"t\":\"view\"";

  o += ",\"seat\":";        append_int(o, v.seat);
  o += ",\"round\":";       append_int(o, v.round);
  o += ",\"memory\":";      append_int(o, v.memory);
  o += ",\"alive\":";       append_bool(o, v.alive);

  o += ",\"faction_known\":";  append_bool(o, v.faction_known);
  o += ",\"faction_is_mole\":"; append_bool(o, v.faction_is_mole);
  o += ",\"identity_known\":"; append_bool(o, v.identity_known);
  o += ",\"identity\":";       append_int(o, static_cast<int>(v.identity));

  o += ",\"gun_holder\":"; append_int(o, v.gun_holder);
  o += ",\"bullets\":";    append_int(o, v.bullets);

  // 手牌：只发类别下标 + 是否是印记牌。对手永远看不到这个字段。
  o += ",\"hand\":[";
  for (std::size_t i = 0; i < v.hand.size(); ++i) {
    if (i) o += ',';
    o += "{\"ci\":"; append_int(o, cat_index(v.hand[i].cat));
    o += ",\"imp\":"; append_bool(o, v.hand[i].imprint);
    o += '}';
  }
  o += ']';

  o += ",\"pub_memory\":[";
  for (int s = 0; s < kSeats; ++s) { if (s) o += ','; append_int(o, v.pub_memory[s]); }
  o += ']';

  o += ",\"pub_alive\":[";
  for (int s = 0; s < kSeats; ++s) { if (s) o += ','; append_bool(o, v.pub_alive[s]); }
  o += ']';

  o += ",\"pub_hand\":[";
  for (int s = 0; s < kSeats; ++s) { if (s) o += ','; append_int(o, v.pub_hand[s]); }
  o += ']';

  o += ",\"history\":[";
  for (std::size_t i = 0; i < v.history.size(); ++i) {
    if (i) o += ',';
    o += "{\"s\":"; append_int(o, v.history[i].seat);
    o += ",\"r\":";  append_int(o, v.history[i].round);
    o += ",\"ci\":"; append_int(o, cat_index(v.history[i].cat));
    o += '}';
  }
  o += ']';

  o += ",\"peeked\":[";
  for (std::size_t i = 0; i < v.peeked.size(); ++i) {
    if (i) o += ',';
    append_int(o, cat_index(v.peeked[i].cat));
  }
  o += ']';

  // 现在轮到你了吗
  if (pending && pending->seat == v.seat) {
    const char* k = "play";
    if (pending->kind == DecisionKind::Gather) k = "gather";
    else if (pending->kind == DecisionKind::Gun) k = "gun";

    o += ",\"act\":{\"k\":\""; o += k; o += '"';
    o += ",\"cards\":[";
    for (std::size_t i = 0; i < pending->cards.size(); ++i) {
      if (i) o += ',';
      append_int(o, pending->cards[i]);
    }
    o += "],\"targets\":[";
    for (std::size_t i = 0; i < pending->targets.size(); ++i) {
      if (i) o += ',';
      append_int(o, pending->targets[i]);
    }
    o += "]}";
  } else {
    o += ",\"act\":null";
  }

  o += '}';
  return o;
}

bool decode_command(std::string_view json, Command& out) {
  FlatJson f;
  if (!f.parse(json)) return false;
  if (f.str("t") != "cmd") return false;

  const std::string k = f.str("k");
  if (k == "gather") out.kind = DecisionKind::Gather;
  else if (k == "gun") out.kind = DecisionKind::Gun;
  else if (k == "play") out.kind = DecisionKind::Play;
  else return false;

  out.a = static_cast<int>(f.num("a", -1));
  out.b = static_cast<int>(f.num("b", -1));
  out.seat = static_cast<int>(f.num("seat", -1));
  return true;
}

std::string encode_joined(int seat) {
  return "{\"t\":\"joined\",\"seat\":" + std::to_string(seat) + "}";
}

std::string encode_error(std::string_view msg) {
  return "{\"t\":\"error\",\"msg\":" + json_quote(msg) + "}";
}

std::string encode_result(md::Winner winner, int rounds, bool mole_alive) {
  const char* w = "none";
  if (winner == md::Winner::Good) w = "good";
  else if (winner == md::Winner::Mole) w = "mole";
  std::string o = "{\"t\":\"over\",\"winner\":\"";
  o += w;
  o += "\",\"rounds\":";
  o += std::to_string(rounds);
  o += ",\"mole_alive\":";
  o += mole_alive ? "true" : "false";
  o += '}';
  return o;
}

}  // namespace md::net
