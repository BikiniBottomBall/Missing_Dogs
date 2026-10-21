// headless 游戏服务器：TCP + 换行分隔 JSON。
//
// 这一层刻意做得很薄——座位、回合、隐藏信息裁剪、掉线转 AI 全在
// md::net::Room 里（不依赖 socket，因此能在 apps/md_selftest 里无网络验证）。
//
// 它和 apps/md_sim 链接同一个 md_core，和 GDExtension 链接同一个 md_net，
// 所以"单机 vs 联机"的差别只在 transport。
//
// 用法：md_server [--port 24680] [--rounds 8] [--seed 1]
//
// 客户端协议（每行一条 JSON）：
//   -> {"t":"join"}                  占一个空座位
//   -> {"t":"fill"} / {"t":"start"}  用 AI 填满剩余座位并开局
//   -> {"t":"cmd","k":"gather","a":0}   k = gather | gun | play
//   <- {"t":"view", ...}             每次状态变化后推送
//   <- {"t":"joined","seat":0}
//   <- {"t":"over","winner":"good","rounds":6,"mole_alive":false}
//   <- {"t":"error","msg":"..."}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "md/core.hpp"
#include "md/net/room.hpp"
#include "md/net/wire.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kBadSock = -1;
#endif

namespace {

void close_sock(sock_t s) {
#ifdef _WIN32
  closesocket(s);
#else
  ::close(s);
#endif
}

bool send_all(sock_t s, const std::string& data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const int n =
        static_cast<int>(::send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0));
    if (n <= 0) return false;
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

struct Peer {
  int id = -1;  // 稳定标识，作为 Room 的 conn
  sock_t fd = kBadSock;
  std::string in;
};

class Server {
 public:
  Server(uint16_t port, uint64_t seed, int rounds, bool verbose)
      : port_(port), room_(seed, rounds), verbose_(verbose) {}

  int run();

 private:
  void deliver(const std::vector<md::net::Delivery>& out);
  Peer* find_peer(int id);

  uint16_t port_;
  md::net::Room room_;
  bool verbose_;
  std::vector<Peer> peers_;
  int next_id_ = 0;
};

Peer* Server::find_peer(int id) {
  for (Peer& p : peers_)
    if (p.id == id) return &p;
  return nullptr;
}

void Server::deliver(const std::vector<md::net::Delivery>& out) {
  for (const auto& d : out) {
    Peer* p = find_peer(d.conn);
    if (!p) continue;  // 对应连接已断，丢弃
    if (!send_all(p->fd, d.line + "\n")) {
      // 发送失败不在这里回收，交给 select 循环
    }
  }
}

int Server::run() {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    std::printf("[server] WSAStartup failed\n");
    return 1;
  }
#endif
  const sock_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd == kBadSock) {
    std::printf("[server] socket() failed\n");
    return 1;
  }
  int one = 1;
  ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
               sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port_);
  if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::printf("[server] bind() failed on port %u\n", unsigned(port_));
    close_sock(listen_fd);
    return 1;
  }
  if (::listen(listen_fd, 8) != 0) {
    std::printf("[server] listen() failed\n");
    close_sock(listen_fd);
    return 1;
  }
  std::printf("[server] listening on 0.0.0.0:%u\n", unsigned(port_));
  std::fflush(stdout);

  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(listen_fd, &rd);
    sock_t maxfd = listen_fd;
    for (const Peer& p : peers_) {
      FD_SET(p.fd, &rd);
      if (p.fd > maxfd) maxfd = p.fd;
    }

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 20000;  // 20ms
    const int n = ::select(static_cast<int>(maxfd) + 1, &rd, nullptr, nullptr, &tv);
    if (n < 0) {
      std::printf("[server] select() failed\n");
      break;
    }
    if (n == 0) continue;

    if (FD_ISSET(listen_fd, &rd)) {
      const sock_t c = ::accept(listen_fd, nullptr, nullptr);
      if (c != kBadSock) {
        Peer p;
        p.id = next_id_++;
        p.fd = c;
        peers_.push_back(std::move(p));
        if (verbose_) std::printf("[server] conn %d connected\n", peers_.back().id);
      }
    }

    for (std::size_t i = peers_.size(); i-- > 0;) {
      if (i >= peers_.size()) continue;
      if (!FD_ISSET(peers_[i].fd, &rd)) continue;

      char buf[4096];
      const int got = static_cast<int>(::recv(peers_[i].fd, buf, sizeof(buf), 0));
      if (got <= 0) {
        const int id = peers_[i].id;
        close_sock(peers_[i].fd);
        peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(i));
        if (verbose_) std::printf("[server] conn %d disconnected\n", id);
        std::vector<md::net::Delivery> out;
        room_.on_disconnect(id, out);
        deliver(out);
        continue;
      }
      peers_[i].in.append(buf, static_cast<std::size_t>(got));

      for (;;) {
        const std::size_t nl = peers_[i].in.find('\n');
        if (nl == std::string::npos) break;
        std::string line = peers_[i].in.substr(0, nl);
        peers_[i].in.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::vector<md::net::Delivery> out;
        room_.on_message(peers_[i].id, line, out);
        deliver(out);
        break;  // 缓冲区可能已被 on_message 之外的操作改动，重新进 select
      }
    }
  }

  for (Peer& p : peers_) close_sock(p.fd);
  close_sock(listen_fd);
#ifdef _WIN32
  WSACleanup();
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 24680;
  int rounds = md::kDefaultRounds;
  uint64_t seed = 1;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (std::strcmp(a, "--port") == 0 && has_next) port = static_cast<uint16_t>(std::atoi(argv[++i]));
    else if (std::strcmp(a, "--rounds") == 0 && has_next) rounds = std::atoi(argv[++i]);
    else if (std::strcmp(a, "--seed") == 0 && has_next) seed = static_cast<uint64_t>(std::atoll(argv[++i]));
    else if (std::strcmp(a, "-v") == 0) verbose = true;
  }

  Server server(port, seed, rounds, verbose);
  return server.run();
}
