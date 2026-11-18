#include "mcp/internal/socket.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include "core/result.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace blue::mcp::internal {

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;

NativeSocket Native(uintptr_t handle) { return static_cast<SOCKET>(handle); }

void CloseNative(NativeSocket socket) { closesocket(socket); }

int LastError() { return WSAGetLastError(); }

bool IsAddressInUse(int error) {
  return error == WSAEADDRINUSE || error == WSAEACCES;
}

std::string ErrorText(int error) {
  char* text = nullptr;
  FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, static_cast<DWORD>(error), 0,
                 reinterpret_cast<char*>(&text), 0, nullptr);
  std::string message = text != nullptr ? text : "unknown error";
  if (text != nullptr) {
    LocalFree(text);
  }
  while (!message.empty() &&
         (message.back() == '\n' || message.back() == '\r' ||
          message.back() == '.')) {
    message.pop_back();
  }
  return std::format("{} (error {})", message, error);
}

int PollOne(NativeSocket socket, short events, int timeout_ms) {
  WSAPOLLFD fd{};
  fd.fd = socket;
  fd.events = events;
  const int ready = WSAPoll(&fd, 1, timeout_ms);
  if (ready > 0 && (fd.revents & POLLNVAL) != 0) {
    return -1;
  }
  return ready;
}

constinit std::atomic<int> g_init_count{0};
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;

NativeSocket Native(uintptr_t handle) { return static_cast<int>(handle); }

void CloseNative(NativeSocket socket) { close(socket); }

int LastError() { return errno; }

bool IsAddressInUse(int error) {
  return error == EADDRINUSE || error == EACCES;
}

std::string ErrorText(int error) {
  return std::format("{} (errno {})", std::strerror(error), error);
}

int PollOne(NativeSocket socket, short events, int timeout_ms) {
  pollfd fd{};
  fd.fd = socket;
  fd.events = events;
  const int ready = poll(&fd, 1, timeout_ms);
  if (ready > 0 && (fd.revents & POLLNVAL) != 0) {
    return -1;
  }
  return ready;
}
#endif

uintptr_t ToHandle(NativeSocket socket) {
  return socket == kInvalidNative ? ~uintptr_t{0}
                                  : static_cast<uintptr_t>(socket);
}

sockaddr_in LoopbackAddress(int port) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  return address;
}

void SetNoDelay(NativeSocket socket) {
  int enabled = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
             reinterpret_cast<const char*>(&enabled), sizeof(enabled));
}

}  // namespace

bool InitSockets() {
#ifdef _WIN32
  if (g_init_count.fetch_add(1) == 0) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      g_init_count.fetch_sub(1);
      return false;
    }
  }
#endif
  return true;
}

void ShutdownSockets() {
#ifdef _WIN32
  if (g_init_count.fetch_sub(1) == 1) {
    WSACleanup();
  }
#endif
}

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalid;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = kInvalid;
  }
  return *this;
}

Socket::~Socket() { Close(); }

void Socket::Close() {
  if (valid()) {
    CloseNative(Native(handle_));
    handle_ = kInvalid;
  }
}

core::Result<Socket, std::string> ListenLoopback(int port) {
  using ResultType = core::Result<Socket, std::string>;
  if (port < 0 || port > 65535) {
    return ResultType::Error(std::format("invalid port {}", port));
  }
  Socket listener(ToHandle(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
  if (!listener.valid()) {
    return ResultType::Error("cannot create socket: " + ErrorText(LastError()));
  }
  const NativeSocket native = Native(listener.handle());
#ifdef _WIN32
  // Without this another process could bind the same port and steal
  // connections.
  BOOL exclusive = TRUE;
  setsockopt(native, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
             reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
  // Lets a restarted server rebind while old connections sit in TIME_WAIT;
  // an active listener on the port still makes bind fail.
  int reuse = 1;
  setsockopt(native, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  fcntl(native, F_SETFD, FD_CLOEXEC);
#endif
  const sockaddr_in address = LoopbackAddress(port);
  if (bind(native, reinterpret_cast<const sockaddr*>(&address),
           sizeof(address)) != 0) {
    const int error = LastError();
    if (IsAddressInUse(error)) {
      return ResultType::Error(std::format(
          "port {} is already in use by another program; choose another "
          "port",
          port));
    }
    return ResultType::Error(
        std::format("cannot bind 127.0.0.1:{}: {}", port, ErrorText(error)));
  }
  if (listen(native, 16) != 0) {
    return ResultType::Error("cannot listen: " + ErrorText(LastError()));
  }
  return ResultType::Ok(std::move(listener));
}

int LocalPort(const Socket& socket) {
  sockaddr_in address{};
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  if (getsockname(Native(socket.handle()),
                  reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

WaitResult WaitReadable(const Socket& socket, int timeout_ms) {
  const int ready = PollOne(Native(socket.handle()), POLLIN, timeout_ms);
  if (ready < 0) {
    return WaitResult::kError;
  }
  return ready == 0 ? WaitResult::kTimeout : WaitResult::kReady;
}

Socket Accept(const Socket& listener) {
  const NativeSocket accepted =
      accept(Native(listener.handle()), nullptr, nullptr);
  if (accepted == kInvalidNative) {
    return Socket();
  }
#ifndef _WIN32
  fcntl(accepted, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  int no_sigpipe = 1;
  setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
             sizeof(no_sigpipe));
#endif
#endif
  SetNoDelay(accepted);
  return Socket(ToHandle(accepted));
}

int64_t Receive(const Socket& socket, char* data, size_t size) {
  const int capped = size > (1u << 20) ? (1 << 20) : static_cast<int>(size);
#ifdef _WIN32
  const int received = recv(Native(socket.handle()), data, capped, 0);
#else
  const ssize_t received =
      recv(Native(socket.handle()), data, static_cast<size_t>(capped), 0);
#endif
  return received < 0 ? -1 : static_cast<int64_t>(received);
}

bool SendAll(const Socket& socket, std::string_view data, int timeout_ms) {
  const NativeSocket native = Native(socket.handle());
  while (!data.empty()) {
    if (PollOne(native, POLLOUT, timeout_ms) <= 0) {
      return false;
    }
    const int chunk =
        data.size() > (1u << 20) ? (1 << 20) : static_cast<int>(data.size());
#ifdef _WIN32
    const int sent = send(native, data.data(), chunk, 0);
#elif defined(MSG_NOSIGNAL)
    const ssize_t sent =
        send(native, data.data(), static_cast<size_t>(chunk), MSG_NOSIGNAL);
#else
    const ssize_t sent =
        send(native, data.data(), static_cast<size_t>(chunk), 0);
#endif
    if (sent <= 0) {
      return false;
    }
    data.remove_prefix(static_cast<size_t>(sent));
  }
  return true;
}

void ShutdownBoth(uintptr_t handle) {
#ifdef _WIN32
  shutdown(Native(handle), SD_BOTH);
#else
  shutdown(Native(handle), SHUT_RDWR);
#endif
}

Socket ConnectLoopback(int port) {
  Socket socket(ToHandle(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)));
  if (!socket.valid()) {
    return Socket();
  }
  const sockaddr_in address = LoopbackAddress(port);
  if (connect(Native(socket.handle()),
              reinterpret_cast<const sockaddr*>(&address),
              sizeof(address)) != 0) {
    return Socket();
  }
  SetNoDelay(Native(socket.handle()));
  return socket;
}

}  // namespace blue::mcp::internal
