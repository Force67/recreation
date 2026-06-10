#include "net/transport.h"

#include <cstring>

#include "core/log.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace rec::net {

UdpSocket::UdpSocket() {
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  handle_ = static_cast<i64>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#if !defined(_WIN32)
  if (handle_ >= 0) fcntl(static_cast<int>(handle_), F_SETFL, O_NONBLOCK);
#endif
}

UdpSocket::~UdpSocket() {
  if (handle_ >= 0) {
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
  }
}

bool UdpSocket::Bind(u16 port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(port);
  if (bind(static_cast<int>(handle_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    REC_ERROR("failed to bind udp port {}", port);
    return false;
  }
  return true;
}

bool UdpSocket::Send(const Endpoint& to, ByteSpan data) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(to.port);
  if (inet_pton(AF_INET, to.address.c_str(), &addr.sin_addr) != 1) return false;
  auto sent = sendto(static_cast<int>(handle_), reinterpret_cast<const char*>(data.data()),
                     data.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  return sent == static_cast<decltype(sent)>(data.size());
}

std::optional<std::vector<u8>> UdpSocket::Receive(Endpoint* from) {
  u8 buffer[2048];
  sockaddr_in addr{};
  socklen_t addr_len = sizeof(addr);
  auto received = recvfrom(static_cast<int>(handle_), reinterpret_cast<char*>(buffer),
                           sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&addr), &addr_len);
  if (received <= 0) return std::nullopt;
  if (from) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    from->address = ip;
    from->port = ntohs(addr.sin_port);
  }
  return std::vector<u8>(buffer, buffer + received);
}

}  // namespace rec::net
