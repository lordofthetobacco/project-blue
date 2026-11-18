#include "mcp/http.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace blue::mcp {

namespace {

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// RFC 9110 tchar.
bool IsTokenChar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  constexpr std::string_view kSymbols = "!#$%&'*+-.^_`|~";
  return kSymbols.find(c) != std::string_view::npos;
}

bool IsToken(std::string_view text) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (!IsTokenChar(c)) {
      return false;
    }
  }
  return true;
}

// Field values: visible ASCII, obs-text, space and tab; no CR, LF or NUL.
bool IsFieldValue(std::string_view text) {
  for (char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte < 0x20 && c != '\t') || byte == 0x7F) {
      return false;
    }
  }
  return true;
}

std::string_view TrimOws(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Returns whether the comma-separated header `list` contains `token`
// (case-insensitive), e.g. "keep-alive, Upgrade" contains "upgrade".
bool ListContains(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const size_t comma = list.find(',');
    const std::string_view item = TrimOws(list.substr(0, comma));
    if (EqualsIgnoreCase(item, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    list.remove_prefix(comma + 1);
  }
  return false;
}

HttpParseResult Error(int status, std::string message) {
  HttpParseResult result;
  result.status = HttpParseResult::Status::kError;
  result.error_status = status;
  result.error_message = std::move(message);
  return result;
}

const std::string* FindIn(const std::vector<HttpHeader>& headers,
                          std::string_view name) {
  for (const HttpHeader& header : headers) {
    if (EqualsIgnoreCase(header.name, name)) {
      return &header.value;
    }
  }
  return nullptr;
}

// Parses the request line "METHOD SP target SP HTTP/1.x".
bool ParseRequestLine(std::string_view line, HttpRequest& request) {
  const size_t first_space = line.find(' ');
  if (first_space == std::string_view::npos) {
    return false;
  }
  const size_t second_space = line.find(' ', first_space + 1);
  if (second_space == std::string_view::npos ||
      line.find(' ', second_space + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view method = line.substr(0, first_space);
  const std::string_view target =
      line.substr(first_space + 1, second_space - first_space - 1);
  const std::string_view version = line.substr(second_space + 1);
  if (!IsToken(method) || target.empty() || !IsFieldValue(target) ||
      (version != "HTTP/1.1" && version != "HTTP/1.0")) {
    return false;
  }
  request.method = std::string(method);
  request.target = std::string(target);
  request.version = std::string(version);
  return true;
}

// Parses the head (request line + headers, without the final CRLF CRLF)
// into `result.request`. Returns false and fills the error on failure.
bool ParseHead(std::string_view head, const HttpLimits& limits,
               HttpParseResult& result) {
  size_t line_end = head.find("\r\n");
  const std::string_view request_line = head.substr(0, line_end);
  if (!ParseRequestLine(request_line, result.request)) {
    result = Error(400, "malformed request line");
    return false;
  }
  std::string_view rest = line_end == std::string_view::npos
                              ? std::string_view()
                              : head.substr(line_end + 2);
  int count = 0;
  while (!rest.empty()) {
    line_end = rest.find("\r\n");
    const std::string_view line = rest.substr(0, line_end);
    rest = line_end == std::string_view::npos ? std::string_view()
                                              : rest.substr(line_end + 2);
    if (line.empty() || line.front() == ' ' || line.front() == '\t') {
      result = Error(400, "obsolete line folding is not supported");
      return false;
    }
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      result = Error(400, "malformed header line");
      return false;
    }
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = TrimOws(line.substr(colon + 1));
    if (!IsToken(name) || !IsFieldValue(value)) {
      result = Error(400, "invalid header");
      return false;
    }
    if (++count > limits.max_header_count) {
      result = Error(431, "too many headers");
      return false;
    }
    result.request.headers.push_back(
        HttpHeader{std::string(name), std::string(value)});
  }
  return true;
}

// Parses a decimal Content-Length; false when invalid.
bool ParseLength(std::string_view text, size_t& length) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  uint64_t value = 0;
  const std::from_chars_result parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc() || value > SIZE_MAX / 2) {
    return false;
  }
  length = static_cast<size_t>(value);
  return true;
}

enum class ChunkStatus { kIncomplete, kComplete, kError, kTooLarge };

// Decodes a chunked body starting at `data`. On kComplete, `consumed` is
// the number of bytes including the last chunk and trailers.
ChunkStatus DecodeChunked(std::string_view data, size_t max_body,
                          std::string& body, size_t& consumed) {
  size_t pos = 0;
  body.clear();
  while (true) {
    const size_t line_end = data.find("\r\n", pos);
    if (line_end == std::string_view::npos) {
      return data.size() - pos > 1024 ? ChunkStatus::kError
                                      : ChunkStatus::kIncomplete;
    }
    std::string_view size_text = data.substr(pos, line_end - pos);
    const size_t extension = size_text.find(';');
    size_text = TrimOws(size_text.substr(0, extension));
    uint64_t chunk_size = 0;
    const std::from_chars_result parsed = std::from_chars(
        size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
    if (size_text.empty() || parsed.ec != std::errc() ||
        parsed.ptr != size_text.data() + size_text.size()) {
      return ChunkStatus::kError;
    }
    pos = line_end + 2;
    if (chunk_size == 0) {
      // Trailer section: header lines until an empty line.
      while (true) {
        const size_t trailer_end = data.find("\r\n", pos);
        if (trailer_end == std::string_view::npos) {
          return ChunkStatus::kIncomplete;
        }
        const bool empty = trailer_end == pos;
        pos = trailer_end + 2;
        if (empty) {
          consumed = pos;
          return ChunkStatus::kComplete;
        }
      }
    }
    if (chunk_size > max_body || body.size() + chunk_size > max_body) {
      return ChunkStatus::kTooLarge;
    }
    const auto size = static_cast<size_t>(chunk_size);
    if (data.size() - pos < size + 2) {
      return ChunkStatus::kIncomplete;
    }
    body.append(data.substr(pos, size));
    pos += size;
    if (data.substr(pos, 2) != "\r\n") {
      return ChunkStatus::kError;
    }
    pos += 2;
  }
}

}  // namespace

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
      return false;
    }
  }
  return true;
}

const std::string* HttpRequest::FindHeader(std::string_view name) const {
  return FindIn(headers, name);
}

HttpParseResult ParseHttpRequest(std::string_view buffer,
                                 const HttpLimits& limits) {
  // Tolerate empty lines before a request (RFC 9112 2.2).
  size_t start = 0;
  while (buffer.substr(start, 2) == "\r\n") {
    start += 2;
  }
  const size_t head_end = buffer.find("\r\n\r\n", start);
  if (head_end == std::string_view::npos) {
    if (buffer.size() - start > limits.max_header_bytes) {
      return Error(431, "request header too large");
    }
    return HttpParseResult();
  }
  if (head_end - start > limits.max_header_bytes) {
    return Error(431, "request header too large");
  }

  HttpParseResult result;
  if (!ParseHead(buffer.substr(start, head_end - start), limits, result)) {
    return result;
  }
  HttpRequest& request = result.request;
  result.headers_complete = true;

  const std::string* connection = request.FindHeader("Connection");
  if (request.version == "HTTP/1.0") {
    request.keep_alive =
        connection != nullptr && ListContains(*connection, "keep-alive");
  } else {
    request.keep_alive =
        connection == nullptr || !ListContains(*connection, "close");
  }
  const std::string* expect = request.FindHeader("Expect");
  result.expects_continue =
      expect != nullptr && EqualsIgnoreCase(*expect, "100-continue");

  const size_t body_start = head_end + 4;
  const std::string_view rest = buffer.substr(body_start);
  const std::string* transfer_encoding =
      request.FindHeader("Transfer-Encoding");
  const std::string* content_length = request.FindHeader("Content-Length");

  if (transfer_encoding != nullptr) {
    if (content_length != nullptr) {
      return Error(400, "both Transfer-Encoding and Content-Length");
    }
    if (!EqualsIgnoreCase(*transfer_encoding, "chunked")) {
      return Error(501, "unsupported transfer coding");
    }
    size_t consumed = 0;
    switch (
        DecodeChunked(rest, limits.max_body_bytes, request.body, consumed)) {
      case ChunkStatus::kIncomplete:
        request.body.clear();
        result.status = HttpParseResult::Status::kIncomplete;
        return result;
      case ChunkStatus::kError:
        return Error(400, "malformed chunked body");
      case ChunkStatus::kTooLarge:
        return Error(413, "request body too large");
      case ChunkStatus::kComplete:
        result.status = HttpParseResult::Status::kComplete;
        result.consumed = body_start + consumed;
        return result;
    }
  }

  size_t length = 0;
  if (content_length != nullptr) {
    // Repeated Content-Length headers must agree.
    for (const HttpHeader& header : request.headers) {
      if (EqualsIgnoreCase(header.name, "Content-Length") &&
          header.value != *content_length) {
        return Error(400, "conflicting Content-Length");
      }
    }
    if (!ParseLength(*content_length, length)) {
      return Error(400, "invalid Content-Length");
    }
    if (length > limits.max_body_bytes) {
      return Error(413, "request body too large");
    }
  }
  if (rest.size() < length) {
    result.status = HttpParseResult::Status::kIncomplete;
    return result;
  }
  request.body = std::string(rest.substr(0, length));
  result.status = HttpParseResult::Status::kComplete;
  result.consumed = body_start + length;
  return result;
}

void HttpResponse::SetHeader(std::string_view name, std::string value) {
  for (HttpHeader& header : headers) {
    if (EqualsIgnoreCase(header.name, name)) {
      header.value = std::move(value);
      return;
    }
  }
  headers.push_back(HttpHeader{std::string(name), std::move(value)});
}

const std::string* HttpResponse::FindHeader(std::string_view name) const {
  return FindIn(headers, name);
}

std::string_view HttpReasonPhrase(int status) {
  switch (status) {
    case 100:
      return "Continue";
    case 200:
      return "OK";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 408:
      return "Request Timeout";
    case 411:
      return "Length Required";
    case 413:
      return "Content Too Large";
    case 415:
      return "Unsupported Media Type";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 503:
      return "Service Unavailable";
    default:
      return "Unknown";
  }
}

std::string SerializeHttpResponse(const HttpResponse& response,
                                  bool keep_alive) {
  std::string out;
  out.reserve(256 + response.body.size());
  out += "HTTP/1.1 ";
  out += std::to_string(response.status);
  out += ' ';
  out += HttpReasonPhrase(response.status);
  out += "\r\n";
  for (const HttpHeader& header : response.headers) {
    out += header.name;
    out += ": ";
    out += header.value;
    out += "\r\n";
  }
  out += "Content-Length: ";
  out += std::to_string(response.body.size());
  out += "\r\n";
  out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  out += response.body;
  return out;
}

}  // namespace blue::mcp
