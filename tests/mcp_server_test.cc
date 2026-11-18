#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "mcp/internal/socket.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"

namespace blue::mcp {
namespace {

using std::chrono::milliseconds;
using std::chrono::steady_clock;

struct ClientResponse {
  int status = 0;
  std::string head;
  std::string body;
};

// Sends `request` on `socket` and reads one response (Content-Length
// framed). nullopt when the connection fails or closes early.
std::optional<ClientResponse> Exchange(const internal::Socket& socket,
                                       const std::string& request) {
  if (!internal::SendAll(socket, request, 5000)) {
    return std::nullopt;
  }
  std::string buffer;
  char chunk[4096];
  while (true) {
    const size_t head_end = buffer.find("\r\n\r\n");
    if (head_end != std::string::npos) {
      ClientResponse response;
      response.head = buffer.substr(0, head_end);
      response.status = std::stoi(buffer.substr(9, 3));
      const size_t length_at = response.head.find("Content-Length: ");
      const size_t length =
          length_at == std::string::npos
              ? 0
              : std::stoul(response.head.substr(length_at + 16));
      if (buffer.size() >= head_end + 4 + length) {
        response.body = buffer.substr(head_end + 4, length);
        return response;
      }
    }
    if (internal::WaitReadable(socket, 10000) != internal::WaitResult::kReady) {
      return std::nullopt;
    }
    const int64_t received = internal::Receive(socket, chunk, sizeof(chunk));
    if (received <= 0) {
      return std::nullopt;
    }
    buffer.append(chunk, static_cast<size_t>(received));
  }
}

std::string Post(int port, const std::string& body,
                 const std::string& extra_headers = "") {
  return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
         "\r\nContent-Type: application/json\r\n"
         "Accept: application/json, text/event-stream\r\n" +
         extra_headers + "Content-Length: " + std::to_string(body.size()) +
         "\r\n\r\n" + body;
}

std::string SessionOf(const ClientResponse& response) {
  const size_t at = response.head.find("Mcp-Session-Id: ");
  REQUIRE(at != std::string::npos);
  return response.head.substr(at + 16, 32);
}

std::string CallBody(const std::string& tool) {
  return R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":")" +
         tool + R"(","arguments":{}}})";
}

constexpr const char* kInitialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{)"
    R"("protocolVersion":"2025-11-25","capabilities":{},)"
    R"("clientInfo":{"name":"loopback-test","version":"1"}}})";

ToolDefinition MakeTool(std::string name, ToolHandler handler,
                        milliseconds timeout = kDefaultToolTimeout) {
  ToolDefinition tool;
  tool.name = std::move(name);
  tool.description = "test";
  tool.input_schema = Json::Object().Set("type", "object");
  tool.handler = std::move(handler);
  tool.timeout = timeout;
  return tool;
}

// Polls `server` on this thread until `done` is set (at most 10 s).
void PollUntil(Server& server, const std::atomic<bool>& done) {
  const auto deadline = steady_clock::now() + std::chrono::seconds(10);
  while (!done.load() && steady_clock::now() < deadline) {
    server.Poll(4);
    std::this_thread::sleep_for(milliseconds(1));
  }
}

TEST_CASE("Server answers a real client over loopback") {
  Server server{ServerOptions()};
  server.RegisterTool(MakeTool("hello", [](const ToolCall& call) {
    return std::optional<ToolResult>(
        ToolResult::Text("hello #" + std::to_string(call.id)));
  }));
  CHECK(server.Poll(8) == 0);
  REQUIRE(server.Start(0).ok());
  REQUIRE(server.running());
  const int port = server.port();
  CHECK(port > 0);

  internal::Socket socket = internal::ConnectLoopback(port);
  REQUIRE(socket.valid());
  std::optional<ClientResponse> init =
      Exchange(socket, Post(port, kInitialize));
  REQUIRE(init.has_value());
  CHECK(init->status == 200);
  CHECK(init->head.find("Connection: keep-alive") != std::string::npos);
  const std::string session = "Mcp-Session-Id: " + SessionOf(*init) + "\r\n";

  // Same connection (keep-alive); the host polls on this thread.
  std::atomic<bool> done{false};
  std::optional<ClientResponse> call;
  std::thread client([&] {
    call = Exchange(socket, Post(port, CallBody("hello"), session));
    done.store(true);
  });
  PollUntil(server, done);
  client.join();
  REQUIRE(call.has_value());
  CHECK(call->status == 200);
  CHECK(call->body.find("hello #") != std::string::npos);
  CHECK(call->body.find("\"isError\":false") != std::string::npos);

  const ServerStatus status = server.Status();
  CHECK(status.state == ServerStatus::State::kListening);
  CHECK(status.open_connections == 1);
  CHECK(status.sessions == 1);
  CHECK(status.tool_calls == 1);
  CHECK(status.requests >= 2);
  CHECK_FALSE(status.recent_activity.empty());

  server.Stop();
  CHECK_FALSE(server.running());
  CHECK(server.Status().state == ServerStatus::State::kStopped);
}

TEST_CASE("Server rejects oversized bodies and unknown paths on the wire") {
  ServerOptions options;
  options.max_body_bytes = 64;
  Server server(options);
  REQUIRE(server.Start(0).ok());
  const int port = server.port();
  {
    internal::Socket socket = internal::ConnectLoopback(port);
    std::optional<ClientResponse> response =
        Exchange(socket, Post(port, std::string(65, ' ')));
    REQUIRE(response.has_value());
    CHECK(response->status == 413);
  }
  {
    internal::Socket socket = internal::ConnectLoopback(port);
    std::optional<ClientResponse> response =
        Exchange(socket, "GET /other HTTP/1.1\r\nHost: localhost\r\n\r\n");
    REQUIRE(response.has_value());
    CHECK(response->status == 404);
  }
  {
    internal::Socket socket = internal::ConnectLoopback(port);
    std::optional<ClientResponse> response = Exchange(socket, "BROKEN\r\n\r\n");
    REQUIRE(response.has_value());
    CHECK(response->status == 400);
  }
}

TEST_CASE("Tool calls time out when the host never polls") {
  Server server{ServerOptions()};
  server.RegisterTool(MakeTool(
      "slow",
      [](const ToolCall&) {
        return std::optional<ToolResult>(ToolResult::Text("late"));
      },
      milliseconds(200)));
  REQUIRE(server.Start(0).ok());
  const int port = server.port();
  internal::Socket socket = internal::ConnectLoopback(port);
  std::optional<ClientResponse> init =
      Exchange(socket, Post(port, kInitialize));
  REQUIRE(init.has_value());
  const auto start = steady_clock::now();
  std::optional<ClientResponse> call =
      Exchange(socket, Post(port, CallBody("slow"),
                            "Mcp-Session-Id: " + SessionOf(*init) + "\r\n"));
  const auto elapsed = steady_clock::now() - start;
  REQUIRE(call.has_value());
  CHECK(call->body.find("\"isError\":true") != std::string::npos);
  CHECK(call->body.find("Timed out") != std::string::npos);
  CHECK(elapsed >= milliseconds(190));
  CHECK(elapsed < milliseconds(5000));
  // The abandoned call is gone: polling now runs nothing.
  CHECK(server.Poll(8) == 0);
  CHECK(server.Status().waiting_calls == 0);
}

TEST_CASE("Pending tool calls finish through CompleteCall") {
  Server server{ServerOptions()};
  std::atomic<uint64_t> pending_id{0};
  server.RegisterTool(MakeTool("later", [&](const ToolCall& call) {
    pending_id.store(call.id);
    return std::optional<ToolResult>();
  }));
  REQUIRE(server.Start(0).ok());
  const int port = server.port();
  internal::Socket socket = internal::ConnectLoopback(port);
  std::optional<ClientResponse> init =
      Exchange(socket, Post(port, kInitialize));
  REQUIRE(init.has_value());
  const std::string session = "Mcp-Session-Id: " + SessionOf(*init) + "\r\n";

  std::atomic<bool> done{false};
  std::optional<ClientResponse> call;
  std::thread client([&] {
    call = Exchange(socket, Post(port, CallBody("later"), session));
    done.store(true);
  });
  const auto deadline = steady_clock::now() + std::chrono::seconds(10);
  while (pending_id.load() == 0 && steady_clock::now() < deadline) {
    server.Poll(1);
    std::this_thread::sleep_for(milliseconds(1));
  }
  REQUIRE(pending_id.load() != 0);
  // A few frames pass while the work is pending.
  for (int i = 0; i < 3; ++i) {
    CHECK(server.Poll(4) == 0);
  }
  CHECK_FALSE(done.load());
  CHECK(server.Status().waiting_calls == 1);
  CHECK_FALSE(
      server.CompleteCall(pending_id.load() + 100, ToolResult::Text("wrong")));
  CHECK(server.CompleteCall(pending_id.load(), ToolResult::Text("finished")));
  CHECK_FALSE(
      server.CompleteCall(pending_id.load(), ToolResult::Text("twice")));
  client.join();
  REQUIRE(call.has_value());
  CHECK(call->body.find("finished") != std::string::npos);
}

TEST_CASE("Stop returns promptly while a request waits for the host") {
  Server server{ServerOptions()};
  server.RegisterTool(MakeTool("never", [](const ToolCall&) {
    return std::optional<ToolResult>(ToolResult::Text("x"));
  }));
  REQUIRE(server.Start(0).ok());
  const int port = server.port();
  internal::Socket socket = internal::ConnectLoopback(port);
  std::optional<ClientResponse> init =
      Exchange(socket, Post(port, kInitialize));
  REQUIRE(init.has_value());
  const std::string session = "Mcp-Session-Id: " + SessionOf(*init) + "\r\n";
  std::atomic<bool> done{false};
  std::thread client([&] {
    Exchange(socket, Post(port, CallBody("never"), session));
    done.store(true);
  });
  const auto deadline = steady_clock::now() + std::chrono::seconds(10);
  while (server.Status().waiting_calls == 0 && steady_clock::now() < deadline) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  REQUIRE(server.Status().waiting_calls == 1);
  const auto start = steady_clock::now();
  server.Stop();
  CHECK(steady_clock::now() - start < milliseconds(1000));
  client.join();
  CHECK(done.load());
  CHECK(server.Status().waiting_calls == 0);
  CHECK(server.Poll(4) == 0);
}

TEST_CASE("A port in use fails with a readable error; restart works") {
  Server first{ServerOptions()};
  REQUIRE(first.Start(0).ok());
  const int port = first.port();
  {
    // Leave a closed connection behind (TIME_WAIT on one side).
    internal::Socket socket = internal::ConnectLoopback(port);
    std::optional<ClientResponse> response =
        Exchange(socket, Post(port, kInitialize));
    REQUIRE(response.has_value());
  }

  Server second{ServerOptions()};
  auto started = second.Start(port);
  REQUIRE_FALSE(started.ok());
  CHECK(started.error().find("in use") != std::string::npos);
  CHECK(second.Status().state == ServerStatus::State::kError);
  CHECK_FALSE(second.last_error().empty());
  CHECK_FALSE(second.running());
  second.Stop();
  CHECK(second.Status().state == ServerStatus::State::kStopped);

  first.Stop();
  REQUIRE(first.Start(port).ok());
  CHECK(first.port() == port);
  // Start while running restarts.
  REQUIRE(first.Start(0).ok());
  CHECK(first.running());
}

}  // namespace
}  // namespace blue::mcp
