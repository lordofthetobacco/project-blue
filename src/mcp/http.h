#ifndef BLUE_MCP_HTTP_H_
#define BLUE_MCP_HTTP_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// A minimal HTTP/1.1 server-side message layer: an incremental request
// parser and a response serializer. Pure functions, no sockets, so the
// transport's edge cases are unit tested on byte strings.
namespace blue::mcp {

struct HttpHeader {
  std::string name;
  std::string value;
};

// A parsed request. Header names keep their spelling; look them up with
// FindHeader (case-insensitive).
struct HttpRequest {
  // Returns the value of the first header named `name` (case-insensitive),
  // or nullptr.
  const std::string* FindHeader(std::string_view name) const;

  std::string method;
  // The request target as sent, e.g. "/mcp" or "/mcp?x=1".
  std::string target;
  // "HTTP/1.1" or "HTTP/1.0".
  std::string version;
  std::vector<HttpHeader> headers;
  // The body, de-chunked when it was sent with chunked transfer coding.
  std::string body;
  // Whether the connection may stay open after the response (HTTP/1.1
  // default unless "Connection: close"; HTTP/1.0 only with keep-alive).
  bool keep_alive = true;
};

struct HttpLimits {
  // Request line plus headers; larger requests get 431.
  size_t max_header_bytes = 16 * 1024;
  // Larger bodies get 413.
  size_t max_body_bytes = 8 * 1024 * 1024;
  // More headers than this get 431.
  int max_header_count = 100;
};

// The outcome of ParseHttpRequest.
struct HttpParseResult {
  enum class Status {
    // More bytes are needed. `headers_complete` and `expects_continue`
    // tell the caller whether to send "100 Continue".
    kIncomplete,
    // `request` is filled; `consumed` bytes of the buffer belong to it.
    kComplete,
    // The request is malformed or over a limit: answer with `error_status`
    // and close the connection.
    kError,
  };

  Status status = Status::kIncomplete;
  size_t consumed = 0;
  bool headers_complete = false;
  bool expects_continue = false;
  int error_status = 0;
  std::string error_message;
  HttpRequest request;
};

// Parses the first request in `buffer` (bytes received so far on a
// connection, possibly followed by pipelined requests). Supports
// Content-Length and chunked bodies (trailers are skipped); a request
// without either has an empty body. Rejects obsolete line folding, bare
// LF line endings in the head, conflicting lengths and invalid tokens.
HttpParseResult ParseHttpRequest(std::string_view buffer,
                                 const HttpLimits& limits);

// A response to serialize. Content-Length and Connection are added by
// SerializeHttpResponse; do not set them.
struct HttpResponse {
  // Sets header `name`, replacing an existing one (case-insensitive).
  void SetHeader(std::string_view name, std::string value);
  // Returns the value of header `name`, or nullptr.
  const std::string* FindHeader(std::string_view name) const;

  int status = 200;
  std::vector<HttpHeader> headers;
  std::string body;
};

// Returns the standard reason phrase for `status` ("OK", "Not Found"...).
std::string_view HttpReasonPhrase(int status);

// Returns the bytes of `response` as HTTP/1.1, with Content-Length and
// "Connection: keep-alive" or "Connection: close".
std::string SerializeHttpResponse(const HttpResponse& response,
                                  bool keep_alive);

// Returns whether `a` and `b` are equal ignoring ASCII case.
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

}  // namespace blue::mcp

#endif  // BLUE_MCP_HTTP_H_
