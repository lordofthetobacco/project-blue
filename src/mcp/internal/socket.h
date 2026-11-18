#ifndef BLUE_MCP_INTERNAL_SOCKET_H_
#define BLUE_MCP_INTERNAL_SOCKET_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/result.h"

// The only platform-specific code of blue_mcp: TCP sockets on loopback over
// Winsock2 or BSD sockets. Blocking I/O with timeouts through poll, so every
// wait can be bounded and the caller can check a stop flag between waits.
namespace blue::mcp::internal {

// Initializes the socket library (WSAStartup on Windows); reference
// counted, pair with ShutdownSockets. Returns false on failure.
bool InitSockets();
void ShutdownSockets();

// An owned socket handle; closed on destruction. Move-only.
class Socket {
 public:
  Socket() = default;
  explicit Socket(uintptr_t handle) : handle_(handle) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;
  ~Socket();

  bool valid() const { return handle_ != kInvalid; }
  uintptr_t handle() const { return handle_; }

  // Closes the socket (no-op when invalid).
  void Close();

 private:
  static constexpr uintptr_t kInvalid = ~uintptr_t{0};

  uintptr_t handle_ = kInvalid;
};

// Returns a listening socket on 127.0.0.1:`port` (0 = ephemeral). The port
// is bound exclusively: a port in use fails with a readable message.
core::Result<Socket, std::string> ListenLoopback(int port);

// Returns the local port of a bound socket, or 0.
int LocalPort(const Socket& socket);

enum class WaitResult { kReady, kTimeout, kError };

// Waits until `socket` is readable (data, a pending connection, or the peer
// closed) or `timeout_ms` passes.
WaitResult WaitReadable(const Socket& socket, int timeout_ms);

// Accepts a pending connection (call after WaitReadable). The returned
// socket has TCP_NODELAY set. Invalid on failure.
Socket Accept(const Socket& listener);

// Receives up to `size` bytes. Returns the byte count, 0 when the peer
// closed the connection, or -1 on error.
int64_t Receive(const Socket& socket, char* data, size_t size);

// Sends all of `data`, waiting at most `timeout_ms` for the peer to make
// room each time the send buffer is full. Returns false on error/timeout.
bool SendAll(const Socket& socket, std::string_view data, int timeout_ms);

// Shuts down both directions, waking threads blocked on the socket.
void ShutdownBoth(uintptr_t handle);

// Connects to 127.0.0.1:`port` (tests and tools). Invalid on failure.
Socket ConnectLoopback(int port);

}  // namespace blue::mcp::internal

#endif  // BLUE_MCP_INTERNAL_SOCKET_H_
