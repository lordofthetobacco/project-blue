#include "mcp/server.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "core/log.h"
#include "core/result.h"
#include "mcp/http.h"
#include "mcp/internal/socket.h"
#include "mcp/json.h"
#include "mcp/protocol.h"
#include "mcp/tool.h"

namespace blue::mcp {

namespace {

// How often blocked network threads wake up to check the stop flag.
constexpr int kWakeIntervalMs = 50;
// How long a send may wait for a slow reader before the connection drops.
constexpr int kSendTimeoutMs = 10000;

std::string ResponseBytes(int status, std::string_view text, bool keep_alive) {
  HttpResponse response;
  response.status = status;
  response.SetHeader("Content-Type", "text/plain; charset=utf-8");
  response.body = std::string(text);
  return SerializeHttpResponse(response, keep_alive);
}

}  // namespace

// The server's state. Network threads call into it through HostBridge
// (CallTool / ReadResource) and ServeConnection.
//
// Locking: calls_mutex_ guards the call queue (queued_, pending_,
// next_call_id_, stopping_ and every QueuedCall's fields);
// connections_mutex_ guards connections_; state_mutex_ guards state_ and
// last_error_. No two are held at once.
class Server::Impl final : public HostBridge {
 public:
  explicit Impl(ServerOptions options)
      : options_(std::move(options)),
        endpoint_(&options_, &registry_, &activity_),
        sockets_ready_(internal::InitSockets()) {}

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  ~Impl() override {
    Stop();
    if (sockets_ready_) {
      internal::ShutdownSockets();
    }
  }

  core::Result<void, std::string> Start(int port);
  void Stop();
  int Poll(int max_calls);
  bool CompleteCall(uint64_t call_id, ToolResult result);
  ServerStatus Status() const;

  ToolResult CallTool(std::shared_ptr<const ToolDefinition> tool,
                      Json arguments) override;
  core::Result<ResourceContents, std::string> ReadResource(
      std::shared_ptr<const ResourceDefinition> resource) override;

  Registry& registry() { return registry_; }
  bool running() const { return running_.load(); }
  int port() const { return port_.load(); }
  std::string last_error() const {
    std::lock_guard lock(state_mutex_);
    return last_error_;
  }

 private:
  // One tools/call or resources/read travelling from a network thread to
  // the host and back. Exactly one of `tool` / `resource` is set.
  struct QueuedCall {
    uint64_t id = 0;
    std::shared_ptr<const ToolDefinition> tool;
    std::shared_ptr<const ResourceDefinition> resource;
    Json arguments;
    // Set by the host side when the result is in place.
    bool finished = false;
    // Set by the waiting side when it gave up (timeout, stop).
    bool abandoned = false;
    ToolResult tool_result;
    std::optional<core::Result<ResourceContents, std::string>> resource_result;
  };

  struct Connection {
    internal::Socket socket;
    std::thread thread;
    std::atomic<bool> done{false};
  };

  // Queues `call` and waits for its result until `timeout`. Returns false
  // on timeout or stop (the call is then abandoned).
  bool Await(const std::shared_ptr<QueuedCall>& call,
             std::chrono::milliseconds timeout, bool& stopped);
  void RunCall(const std::shared_ptr<QueuedCall>& call);
  void ListenLoop();
  void ServeConnection(Connection& connection);
  // Joins and removes finished connections. Listener thread only.
  void ReapConnections();
  void SetState(ServerStatus::State state, std::string error);

  ServerOptions options_;
  Registry registry_;
  ActivityLog activity_;
  Endpoint endpoint_;
  bool sockets_ready_;

  mutable std::mutex calls_mutex_;
  std::condition_variable calls_cv_;
  std::deque<std::shared_ptr<QueuedCall>> queued_;
  std::map<uint64_t, std::shared_ptr<QueuedCall>> pending_;
  uint64_t next_call_id_ = 1;
  bool stopping_ = false;

  internal::Socket listener_;
  std::thread listener_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  std::atomic<int> port_{0};
  std::atomic<int> open_connections_{0};
  std::atomic<int64_t> total_connections_{0};
  std::mutex connections_mutex_;
  std::list<std::unique_ptr<Connection>> connections_;

  mutable std::mutex state_mutex_;
  ServerStatus::State state_ = ServerStatus::State::kStopped;
  std::string last_error_;
};

core::Result<void, std::string> Server::Impl::Start(int port) {
  Stop();
  port_.store(port);
  if (!sockets_ready_) {
    SetState(ServerStatus::State::kError, "socket library unavailable");
    return core::Result<void, std::string>::Error("socket library unavailable");
  }
  core::Result<internal::Socket, std::string> listener =
      internal::ListenLoopback(port);
  if (!listener.ok()) {
    activity_.Add("start failed: " + listener.error());
    core::log::Warn("MCP server: {}", listener.error());
    SetState(ServerStatus::State::kError, listener.error());
    return core::Result<void, std::string>::Error(listener.error());
  }
  listener_ = std::move(listener).value();
  port_.store(internal::LocalPort(listener_));
  {
    std::lock_guard lock(calls_mutex_);
    stopping_ = false;
  }
  stop_.store(false);
  running_.store(true);
  SetState(ServerStatus::State::kListening, std::string());
  activity_.Add(
      std::format("listening on http://127.0.0.1:{}/mcp", port_.load()));
  core::log::Info("MCP server listening on http://127.0.0.1:{}/mcp",
                  port_.load());
  listener_thread_ = std::thread([this] { ListenLoop(); });
  return core::Result<void, std::string>::Ok();
}

void Server::Impl::Stop() {
  if (!listener_thread_.joinable()) {
    // Not running (or Start failed): only leave the error state; the
    // message stays available through last_error().
    std::lock_guard lock(state_mutex_);
    state_ = ServerStatus::State::kStopped;
    return;
  }
  stop_.store(true);
  // Wake network threads waiting for the host; they answer with an error.
  {
    std::lock_guard lock(calls_mutex_);
    stopping_ = true;
  }
  calls_cv_.notify_all();
  // Wake threads blocked in send/recv.
  {
    std::lock_guard lock(connections_mutex_);
    for (const std::unique_ptr<Connection>& connection : connections_) {
      internal::ShutdownBoth(connection->socket.handle());
    }
  }
  listener_thread_.join();
  for (const std::unique_ptr<Connection>& connection : connections_) {
    connection->thread.join();
  }
  connections_.clear();
  listener_.Close();
  {
    std::lock_guard lock(calls_mutex_);
    // Waiters have abandoned their calls; drop what the host still holds.
    queued_.clear();
    pending_.clear();
  }
  endpoint_.ClearSessions();
  running_.store(false);
  {
    std::lock_guard lock(state_mutex_);
    state_ = ServerStatus::State::kStopped;
  }
  activity_.Add("stopped");
  core::log::Info("MCP server stopped");
}

void Server::Impl::ListenLoop() {
  while (!stop_.load()) {
    ReapConnections();
    const internal::WaitResult wait =
        internal::WaitReadable(listener_, kWakeIntervalMs);
    if (wait == internal::WaitResult::kTimeout) {
      continue;
    }
    if (wait == internal::WaitResult::kError) {
      if (!stop_.load()) {
        SetState(ServerStatus::State::kError, "listening socket failed");
        activity_.Add("listening socket failed");
        running_.store(false);
      }
      return;
    }
    internal::Socket socket = internal::Accept(listener_);
    if (!socket.valid()) {
      continue;
    }
    total_connections_.fetch_add(1);
    if (open_connections_.load() >= options_.max_connections) {
      internal::SendAll(
          socket, ResponseBytes(503, "Too many connections.", false), 1000);
      activity_.Add("rejected a connection: too many open");
      continue;
    }
    open_connections_.fetch_add(1);
    auto connection = std::make_unique<Connection>();
    connection->socket = std::move(socket);
    Connection* raw = connection.get();
    {
      std::lock_guard lock(connections_mutex_);
      connections_.push_back(std::move(connection));
    }
    raw->thread = std::thread([this, raw] { ServeConnection(*raw); });
  }
}

void Server::Impl::ReapConnections() {
  std::lock_guard lock(connections_mutex_);
  for (auto it = connections_.begin(); it != connections_.end();) {
    if ((*it)->done.load()) {
      (*it)->thread.join();
      it = connections_.erase(it);
    } else {
      ++it;
    }
  }
}

void Server::Impl::ServeConnection(Connection& connection) {
  const internal::Socket& socket = connection.socket;
  HttpLimits limits;
  limits.max_body_bytes = options_.max_body_bytes;
  std::string buffer;
  char chunk[16 * 1024];
  bool sent_continue = false;
  auto last_activity = std::chrono::steady_clock::now();

  while (!stop_.load()) {
    HttpParseResult parsed = ParseHttpRequest(buffer, limits);
    if (parsed.status == HttpParseResult::Status::kError) {
      activity_.Add(std::format("HTTP {}: {}", parsed.error_status,
                                parsed.error_message));
      internal::SendAll(
          socket,
          ResponseBytes(parsed.error_status, parsed.error_message, false),
          kSendTimeoutMs);
      break;
    }
    if (parsed.status == HttpParseResult::Status::kComplete) {
      buffer.erase(0, parsed.consumed);
      sent_continue = false;
      const HttpResponse response = endpoint_.Handle(parsed.request, *this);
      const bool keep_alive = parsed.request.keep_alive && !stop_.load();
      if (!internal::SendAll(socket,
                             SerializeHttpResponse(response, keep_alive),
                             kSendTimeoutMs) ||
          !keep_alive) {
        break;
      }
      last_activity = std::chrono::steady_clock::now();
      continue;
    }
    if (parsed.headers_complete && parsed.expects_continue && !sent_continue) {
      sent_continue = true;
      if (!internal::SendAll(socket, "HTTP/1.1 100 Continue\r\n\r\n",
                             kSendTimeoutMs)) {
        break;
      }
    }
    const internal::WaitResult wait =
        internal::WaitReadable(socket, kWakeIntervalMs);
    if (wait == internal::WaitResult::kError) {
      break;
    }
    if (wait == internal::WaitResult::kTimeout) {
      if (std::chrono::steady_clock::now() - last_activity >
          options_.idle_timeout) {
        if (!buffer.empty()) {
          internal::SendAll(socket,
                            ResponseBytes(408, "Request timeout.", false),
                            kSendTimeoutMs);
        }
        break;
      }
      continue;
    }
    const int64_t received = internal::Receive(socket, chunk, sizeof(chunk));
    if (received <= 0) {
      break;
    }
    buffer.append(chunk, static_cast<size_t>(received));
    last_activity = std::chrono::steady_clock::now();
  }
  // Send FIN now; the socket is closed when the listener reaps it.
  internal::ShutdownBoth(socket.handle());
  open_connections_.fetch_sub(1);
  connection.done.store(true);
}

bool Server::Impl::Await(const std::shared_ptr<QueuedCall>& call,
                         std::chrono::milliseconds timeout, bool& stopped) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lock(calls_mutex_);
  stopped = stopping_;
  if (stopping_) {
    return false;
  }
  call->id = next_call_id_++;
  queued_.push_back(call);
  calls_cv_.wait_until(lock, deadline,
                       [&] { return call->finished || stopping_; });
  if (call->finished) {
    return true;
  }
  stopped = stopping_;
  call->abandoned = true;
  const auto queued = std::find(queued_.begin(), queued_.end(), call);
  if (queued != queued_.end()) {
    queued_.erase(queued);
  }
  pending_.erase(call->id);
  return false;
}

ToolResult Server::Impl::CallTool(std::shared_ptr<const ToolDefinition> tool,
                                  Json arguments) {
  auto call = std::make_shared<QueuedCall>();
  const std::chrono::milliseconds timeout = tool->timeout;
  call->tool = std::move(tool);
  call->arguments = std::move(arguments);
  bool stopped = false;
  if (!Await(call, timeout, stopped)) {
    return ToolResult::Error(
        stopped ? std::string("The editor's MCP server is stopping.")
                : std::format("Timed out after {} ms waiting for the editor "
                              "(busy or a modal dialog open?). The action "
                              "may still be applied later; check the state "
                              "before retrying.",
                              timeout.count()));
  }
  return std::move(call->tool_result);
}

core::Result<ResourceContents, std::string> Server::Impl::ReadResource(
    std::shared_ptr<const ResourceDefinition> resource) {
  auto call = std::make_shared<QueuedCall>();
  call->resource = std::move(resource);
  bool stopped = false;
  if (!Await(call, options_.resource_timeout, stopped)) {
    return core::Result<ResourceContents, std::string>::Error(
        stopped ? "The editor's MCP server is stopping."
                : "Timed out waiting for the editor.");
  }
  return std::move(*call->resource_result);
}

int Server::Impl::Poll(int max_calls) {
  int ran = 0;
  while (ran < max_calls) {
    std::shared_ptr<QueuedCall> call;
    {
      std::lock_guard lock(calls_mutex_);
      if (queued_.empty()) {
        break;
      }
      call = std::move(queued_.front());
      queued_.pop_front();
    }
    ++ran;
    RunCall(call);
  }
  return ran;
}

void Server::Impl::RunCall(const std::shared_ptr<QueuedCall>& call) {
  if (call->tool != nullptr) {
    const ToolCall tool_call{call->id, call->tool->name, call->arguments};
    std::optional<ToolResult> result =
        call->tool->handler
            ? call->tool->handler(tool_call)
            : std::optional<ToolResult>(ToolResult::Error("No handler."));
    std::lock_guard lock(calls_mutex_);
    if (call->abandoned) {
      return;
    }
    if (!result) {
      pending_[call->id] = call;
      return;
    }
    call->tool_result = std::move(*result);
    call->finished = true;
  } else {
    core::Result<ResourceContents, std::string> contents =
        call->resource->handler
            ? call->resource->handler(call->resource->uri)
            : core::Result<ResourceContents, std::string>::Error("No handler.");
    std::lock_guard lock(calls_mutex_);
    if (call->abandoned) {
      return;
    }
    call->resource_result = std::move(contents);
    call->finished = true;
  }
  calls_cv_.notify_all();
}

bool Server::Impl::CompleteCall(uint64_t call_id, ToolResult result) {
  {
    std::lock_guard lock(calls_mutex_);
    const auto it = pending_.find(call_id);
    if (it == pending_.end()) {
      return false;
    }
    it->second->tool_result = std::move(result);
    it->second->finished = true;
    pending_.erase(it);
  }
  calls_cv_.notify_all();
  return true;
}

ServerStatus Server::Impl::Status() const {
  ServerStatus status;
  {
    std::lock_guard lock(state_mutex_);
    status.state = state_;
    status.last_error = last_error_;
  }
  status.port = port_.load();
  status.open_connections = open_connections_.load();
  status.total_connections = total_connections_.load();
  status.sessions = endpoint_.session_count();
  status.requests = endpoint_.request_count();
  status.tool_calls = endpoint_.tool_call_count();
  {
    std::lock_guard lock(calls_mutex_);
    status.waiting_calls = static_cast<int>(queued_.size() + pending_.size());
  }
  status.recent_activity = activity_.Snapshot();
  return status;
}

void Server::Impl::SetState(ServerStatus::State state, std::string error) {
  std::lock_guard lock(state_mutex_);
  state_ = state;
  last_error_ = std::move(error);
}

Server::Server(ServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

Server::~Server() = default;

void Server::RegisterTool(ToolDefinition tool) {
  impl_->registry().AddTool(std::move(tool));
}

void Server::RegisterResource(ResourceDefinition resource) {
  impl_->registry().AddResource(std::move(resource));
}

core::Result<void, std::string> Server::Start(int port) {
  return impl_->Start(port);
}

void Server::Stop() { impl_->Stop(); }

bool Server::running() const { return impl_->running(); }

int Server::port() const { return impl_->port(); }

std::string Server::last_error() const { return impl_->last_error(); }

int Server::Poll(int max_calls) { return impl_->Poll(max_calls); }

bool Server::CompleteCall(uint64_t call_id, ToolResult result) {
  return impl_->CompleteCall(call_id, std::move(result));
}

ServerStatus Server::Status() const { return impl_->Status(); }

}  // namespace blue::mcp
