#include <doctest/doctest.h>

#include <string>

#include "mcp/http.h"

namespace blue::mcp {
namespace {

using Status = HttpParseResult::Status;

HttpParseResult Parse(const std::string& bytes) {
  return ParseHttpRequest(bytes, HttpLimits());
}

TEST_CASE("HTTP parser reads a POST with Content-Length") {
  const std::string bytes =
      "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:8765\r\n"
      "content-type: application/json\r\nContent-Length: 2\r\n\r\n{}";
  const HttpParseResult result = Parse(bytes + "GET / HTTP/1.1\r\n");
  REQUIRE(result.status == Status::kComplete);
  CHECK(result.consumed == bytes.size());
  CHECK(result.request.method == "POST");
  CHECK(result.request.target == "/mcp");
  CHECK(result.request.body == "{}");
  CHECK(result.request.keep_alive);
  REQUIRE(result.request.FindHeader("Content-Type") != nullptr);
  CHECK(*result.request.FindHeader("CONTENT-TYPE") == "application/json");
}

TEST_CASE("HTTP parser waits for the whole head and body") {
  CHECK(Parse("POST /mcp HTTP/1.1\r\nHost: x\r\n").status ==
        Status::kIncomplete);
  const HttpParseResult partial = Parse(
      "POST /mcp HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n"
      "Expect: 100-continue\r\n\r\n{}");
  CHECK(partial.status == Status::kIncomplete);
  CHECK(partial.headers_complete);
  CHECK(partial.expects_continue);
}

TEST_CASE("HTTP parser decodes chunked bodies") {
  const std::string bytes =
      "POST /mcp HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\n{\"a\r\n4;ext=1\r\n\":1}\r\n0\r\nTrailer: y\r\n\r\n";
  const HttpParseResult result = Parse(bytes);
  REQUIRE(result.status == Status::kComplete);
  CHECK(result.request.body == "{\"a\":1}");
  CHECK(result.consumed == bytes.size());
  CHECK(Parse(bytes.substr(0, bytes.size() - 2)).status == Status::kIncomplete);
}

TEST_CASE("HTTP parser applies connection semantics") {
  CHECK_FALSE(
      Parse("GET / HTTP/1.1\r\nConnection: close\r\n\r\n").request.keep_alive);
  CHECK_FALSE(Parse("GET / HTTP/1.0\r\n\r\n").request.keep_alive);
  CHECK(Parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n")
            .request.keep_alive);
}

TEST_CASE("HTTP parser rejects malformed requests") {
  struct Case {
    const char* bytes;
    int status;
  };
  const Case cases[] = {
      {"GARBAGE\r\n\r\n", 400},
      {"GET /mcp HTTP/2.0\r\n\r\n", 400},
      {"GET /mcp HTTP/1.1\r\nBad Header\r\n\r\n", 400},
      {"GET /mcp HTTP/1.1\r\nX: a\r\n folded\r\n\r\n", 400},
      {"GET /mcp HTTP/1.1\r\nBad Name: a\r\n\r\n", 400},
      {"POST /mcp HTTP/1.1\r\nContent-Length: abc\r\n\r\n", 400},
      {"POST /mcp HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
       400},
      {"POST /mcp HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n", 501},
      {"POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
       "Content-Length: 3\r\n\r\n",
       400},
      {"POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400},
  };
  for (const Case& c : cases) {
    CAPTURE(c.bytes);
    const HttpParseResult result = Parse(c.bytes);
    CHECK(result.status == Status::kError);
    CHECK(result.error_status == c.status);
  }
}

TEST_CASE("HTTP parser enforces size limits") {
  HttpLimits limits;
  limits.max_body_bytes = 16;
  limits.max_header_bytes = 64;
  const HttpParseResult body = ParseHttpRequest(
      "POST /mcp HTTP/1.1\r\nContent-Length: 17\r\n\r\n", limits);
  CHECK(body.status == Status::kError);
  CHECK(body.error_status == 413);
  const HttpParseResult chunked = ParseHttpRequest(
      "POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n20\r\n", limits);
  CHECK(chunked.error_status == 413);
  const HttpParseResult head = ParseHttpRequest(
      "GET /mcp HTTP/1.1\r\nX: " + std::string(100, 'a'), limits);
  CHECK(head.status == Status::kError);
  CHECK(head.error_status == 431);
}

TEST_CASE("HTTP responses carry length and connection headers") {
  HttpResponse response;
  response.status = 202;
  response.SetHeader("X-Test", "1");
  response.SetHeader("x-test", "2");
  CHECK(*response.FindHeader("X-TEST") == "2");
  CHECK(SerializeHttpResponse(response, true) ==
        "HTTP/1.1 202 Accepted\r\nX-Test: 2\r\nContent-Length: 0\r\n"
        "Connection: keep-alive\r\n\r\n");
  response.body = "abc";
  CHECK(SerializeHttpResponse(response, false)
            .ends_with("Content-Length: 3\r\nConnection: close\r\n\r\nabc"));
}

}  // namespace
}  // namespace blue::mcp
