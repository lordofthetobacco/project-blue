#include "mcp/protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"
#include "mcp/http.h"
#include "mcp/json.h"
#include "mcp/server.h"
#include "mcp/tool.h"

namespace blue::mcp {

namespace {

constexpr std::string_view kMetaProtocolVersion =
    "io.modelcontextprotocol/protocolVersion";
constexpr std::string_view kMetaClientCapabilities =
    "io.modelcontextprotocol/clientCapabilities";
constexpr std::string_view kMetaServerInfo =
    "io.modelcontextprotocol/serverInfo";

// The revision that introduced batches and (only it) allows them.
constexpr std::string_view kBatchProtocolVersion = "2025-03-26";

enum class MessageKind { kRequest, kNotification, kResponse, kInvalid };

bool IsLegacyVersion(std::string_view version) {
  for (std::string_view known : kLegacyProtocolVersions) {
    if (known == version) {
      return true;
    }
  }
  return false;
}

// Returns the kind of a single JSON-RPC message (MCP rules: ids are strings
// or integers, never null; params is an object when present).
MessageKind Classify(const Json& message) {
  if (!message.is_object()) {
    return MessageKind::kInvalid;
  }
  const std::string* jsonrpc = message.GetString("jsonrpc");
  if (jsonrpc == nullptr || *jsonrpc != "2.0") {
    return MessageKind::kInvalid;
  }
  const Json* id = message.Find("id");
  const bool id_valid = id != nullptr && (id->is_string() || id->is_integer());
  if (const Json* method = message.Find("method")) {
    if (!method->is_string()) {
      return MessageKind::kInvalid;
    }
    const Json* params = message.Find("params");
    if (params != nullptr && !params->is_object()) {
      return MessageKind::kInvalid;
    }
    if (id == nullptr) {
      return MessageKind::kNotification;
    }
    return id_valid ? MessageKind::kRequest : MessageKind::kInvalid;
  }
  if ((message.Find("result") != nullptr) !=
          (message.Find("error") != nullptr) &&
      id != nullptr) {
    return MessageKind::kResponse;
  }
  return MessageKind::kInvalid;
}

// Returns the request's id when it is usable in a response, else null.
Json IdOf(const Json& message) {
  const Json* id = message.Find("id");
  if (id != nullptr && (id->is_string() || id->is_integer())) {
    return *id;
  }
  return Json::Null();
}

// Builds a JSON-RPC error response; `id` null means "unknown" (omitted).
Json MakeError(const Json& id, int code, std::string_view message,
               std::optional<Json> data = std::nullopt) {
  Json error = Json::Object();
  error.Set("code", code).Set("message", message);
  if (data) {
    error.Set("data", std::move(*data));
  }
  Json response = Json::Object();
  response.Set("jsonrpc", "2.0");
  if (!id.is_null()) {
    response.Set("id", id);
  }
  response.Set("error", std::move(error));
  return response;
}

Json MakeResult(const Json& id, Json result) {
  Json response = Json::Object();
  response.Set("jsonrpc", "2.0").Set("id", id).Set("result", std::move(result));
  return response;
}

HttpResponse JsonResponse(int status, const Json& body) {
  HttpResponse response;
  response.status = status;
  response.SetHeader("Content-Type", "application/json");
  response.SetHeader("Cache-Control", "no-store");
  response.body = body.Dump();
  return response;
}

HttpResponse ErrorResponse(int status, const Json& id, int code,
                           std::string_view message,
                           std::optional<Json> data = std::nullopt) {
  return JsonResponse(status, MakeError(id, code, message, std::move(data)));
}

HttpResponse Accepted() {
  HttpResponse response;
  response.status = 202;
  return response;
}

HttpResponse PlainResponse(int status, std::string_view text) {
  HttpResponse response;
  response.status = status;
  response.SetHeader("Content-Type", "text/plain; charset=utf-8");
  response.body = std::string(text);
  return response;
}

std::string_view TrimSpaces(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

// Returns the host of an authority "host[:port]" (IPv6 kept in brackets).
std::string_view HostOfAuthority(std::string_view authority) {
  if (!authority.empty() && authority.front() == '[') {
    const size_t close = authority.find(']');
    return close == std::string_view::npos ? std::string_view()
                                           : authority.substr(0, close + 1);
  }
  return authority.substr(0, authority.find(':'));
}

bool IsLoopbackName(std::string_view host) {
  return EqualsIgnoreCase(host, "localhost") || host == "127.0.0.1" ||
         host == "[::1]";
}

std::string NewSessionId() {
  std::random_device device;
  std::string id;
  for (int i = 0; i < 4; ++i) {
    id += std::format("{:08x}", static_cast<uint32_t>(device()));
  }
  return id;
}

int Base64Value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}

bool DecodeBase64(std::string_view text, std::string& out) {
  out.clear();
  if (text.size() % 4 != 0) {
    return false;
  }
  uint32_t buffer = 0;
  int bits = 0;
  size_t padding = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '=') {
      if (i + 2 < text.size()) {
        return false;
      }
      ++padding;
      continue;
    }
    if (padding > 0) {
      return false;
    }
    const int value = Base64Value(c);
    if (value < 0) {
      return false;
    }
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
    }
  }
  return true;
}

// Returns the first line of `text`, shortened, for the activity log.
std::string Summary(std::string_view text) {
  std::string_view line = text.substr(0, text.find('\n'));
  if (line.size() > 80) {
    return std::string(line.substr(0, 77)) + "...";
  }
  return std::string(line);
}

Json ContentToJson(const ContentItem& item) {
  Json json = Json::Object();
  switch (item.kind) {
    case ContentItem::Kind::kText:
      json.Set("type", "text").Set("text", item.text);
      break;
    case ContentItem::Kind::kImage:
      json.Set("type", "image")
          .Set("data", item.data)
          .Set("mimeType", item.mime_type);
      break;
  }
  return json;
}

}  // namespace

// What the current request may use, from its protocol revision.
struct Endpoint::CallContext {
  bool modern = false;
  std::string version;
  // structuredContent, outputSchema and title fields (2025-06-18+).
  bool structured = false;
  // Tool annotations (2025-03-26+).
  bool annotations = false;

  static CallContext For(std::string_view version) {
    CallContext context;
    context.modern = version == kModernProtocolVersion;
    context.version = std::string(version);
    // Revision names are dates, so they order as strings.
    context.structured = version >= std::string_view("2025-06-18");
    context.annotations = version >= std::string_view("2025-03-26");
    return context;
  }
};

// A method's result or JSON-RPC error.
struct Endpoint::Outcome {
  static Outcome Success(Json result) {
    Outcome outcome;
    outcome.result = std::move(result);
    return outcome;
  }
  static Outcome Failure(int code, std::string message,
                         std::optional<Json> data = std::nullopt) {
    Outcome outcome;
    outcome.error_code = code;
    outcome.error_message = std::move(message);
    outcome.error_data = std::move(data);
    return outcome;
  }

  // Returns the JSON-RPC response for request `id`.
  Json ToResponse(const Json& id) const {
    if (result) {
      return MakeResult(id, *result);
    }
    return MakeError(id, error_code, error_message, error_data);
  }

  std::optional<Json> result;
  int error_code = 0;
  std::string error_message;
  std::optional<Json> error_data;
};

void Registry::AddTool(ToolDefinition tool) {
  auto shared = std::make_shared<const ToolDefinition>(std::move(tool));
  std::lock_guard lock(mutex_);
  tools_[shared->name] = std::move(shared);
}

void Registry::AddResource(ResourceDefinition resource) {
  auto shared = std::make_shared<const ResourceDefinition>(std::move(resource));
  std::lock_guard lock(mutex_);
  resources_[shared->uri] = std::move(shared);
}

std::vector<std::shared_ptr<const ToolDefinition>> Registry::Tools() const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.reserve(tools_.size());
  for (const auto& [name, tool] : tools_) {
    tools.push_back(tool);
  }
  return tools;
}

std::vector<std::shared_ptr<const ResourceDefinition>> Registry::Resources()
    const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<const ResourceDefinition>> resources;
  resources.reserve(resources_.size());
  for (const auto& [uri, resource] : resources_) {
    resources.push_back(resource);
  }
  return resources;
}

std::shared_ptr<const ToolDefinition> Registry::FindTool(
    std::string_view name) const {
  std::lock_guard lock(mutex_);
  const auto it = tools_.find(name);
  return it == tools_.end() ? nullptr : it->second;
}

std::shared_ptr<const ResourceDefinition> Registry::FindResource(
    std::string_view uri) const {
  std::lock_guard lock(mutex_);
  const auto it = resources_.find(uri);
  return it == resources_.end() ? nullptr : it->second;
}

void ActivityLog::Add(std::string text) {
  std::lock_guard lock(mutex_);
  if (entries_.size() == kCapacity) {
    entries_.pop_front();
  }
  entries_.push_back(
      ActivityEntry{std::chrono::system_clock::now(), std::move(text)});
}

std::vector<ActivityEntry> ActivityLog::Snapshot() const {
  std::lock_guard lock(mutex_);
  return std::vector<ActivityEntry>(entries_.begin(), entries_.end());
}

void ActivityLog::Clear() {
  std::lock_guard lock(mutex_);
  entries_.clear();
}

bool IsLocalOrigin(std::string_view origin) {
  size_t authority_start = 0;
  if (origin.starts_with("http://")) {
    authority_start = 7;
  } else if (origin.starts_with("https://")) {
    authority_start = 8;
  } else {
    return false;
  }
  std::string_view authority = origin.substr(authority_start);
  authority = authority.substr(0, authority.find('/'));
  return IsLoopbackName(HostOfAuthority(authority));
}

bool IsLocalHost(std::string_view host) {
  return IsLoopbackName(HostOfAuthority(TrimSpaces(host)));
}

bool DecodeHeaderValue(std::string_view value, std::string& decoded) {
  constexpr std::string_view kPrefix = "=?base64?";
  constexpr std::string_view kSuffix = "?=";
  if (value.size() >= kPrefix.size() + kSuffix.size() &&
      value.starts_with(kPrefix) && value.ends_with(kSuffix)) {
    return DecodeBase64(
        value.substr(kPrefix.size(),
                     value.size() - kPrefix.size() - kSuffix.size()),
        decoded);
  }
  decoded = std::string(value);
  return true;
}

Endpoint::Endpoint(const ServerOptions* options, const Registry* registry,
                   ActivityLog* activity)
    : options_(options), registry_(registry), activity_(activity) {}

HttpResponse Endpoint::Handle(const HttpRequest& request, HostBridge& host) {
  const std::string_view target = request.target;
  const std::string_view path = target.substr(0, target.find('?'));
  if (path != "/mcp") {
    return PlainResponse(404, "Not found. The MCP endpoint is /mcp.");
  }
  // DNS rebinding protection: only pages and clients on this machine.
  if (const std::string* origin = request.FindHeader("Origin")) {
    if (!IsLocalOrigin(*origin)) {
      activity_->Add(std::format("rejected Origin {}", Summary(*origin)));
      return ErrorResponse(403, Json::Null(), kInvalidRequest,
                           "Forbidden: Origin not allowed");
    }
  }
  const std::string* host_header = request.FindHeader("Host");
  if (host_header == nullptr || !IsLocalHost(*host_header)) {
    activity_->Add(std::format("rejected Host {}",
                               host_header ? Summary(*host_header) : "(none)"));
    return ErrorResponse(403, Json::Null(), kInvalidRequest,
                         "Forbidden: Host not allowed");
  }

  if (request.method == "POST") {
    return HandlePost(request, host);
  }
  if (request.method == "DELETE") {
    return HandleDelete(request);
  }
  if (request.method == "OPTIONS") {
    HttpResponse response;
    response.status = 204;
    response.SetHeader("Allow", "POST, DELETE, OPTIONS");
    return response;
  }
  // GET would open the optional standalone SSE stream (legacy) and is
  // removed in 2026-07-28; neither is offered.
  HttpResponse response = PlainResponse(405, "Method not allowed. Use POST.");
  response.SetHeader("Allow", "POST, DELETE, OPTIONS");
  return response;
}

HttpResponse Endpoint::HandlePost(const HttpRequest& request,
                                  HostBridge& host) {
  const std::string* content_type = request.FindHeader("Content-Type");
  if (content_type == nullptr ||
      !EqualsIgnoreCase(TrimSpaces(std::string_view(*content_type)
                                       .substr(0, content_type->find(';'))),
                        "application/json")) {
    return PlainResponse(415, "Content-Type must be application/json.");
  }
  core::Result<Json, JsonError> parsed = ParseJson(request.body);
  if (!parsed.ok()) {
    requests_.fetch_add(1);
    return ErrorResponse(
        400, Json::Null(), kParseError,
        std::format("Parse error: {} at byte {}", parsed.error().message,
                    parsed.error().offset));
  }
  const Json& message = parsed.value();

  if (message.is_object()) {
    const Json* params = message.GetObject("params");
    const Json* meta = params ? params->GetObject("_meta") : nullptr;
    if (meta != nullptr && meta->Find(kMetaProtocolVersion) != nullptr) {
      return HandleModern(request, message, host);
    }
    const std::string* method = message.GetString("method");
    if (method != nullptr && *method == "initialize") {
      return HandleInitialize(message);
    }
    // No session and a modern (or unknown) version header: a modern client
    // that left out _meta. Answer in modern terms so it can correct itself.
    const std::string* version = request.FindHeader("MCP-Protocol-Version");
    if (request.FindHeader("Mcp-Session-Id") == nullptr && version != nullptr &&
        !IsLegacyVersion(*version)) {
      return HandleModern(request, message, host);
    }
  }
  return HandleLegacy(request, message, host);
}

HttpResponse Endpoint::HandleDelete(const HttpRequest& request) {
  const std::string* id = request.FindHeader("Mcp-Session-Id");
  if (id == nullptr) {
    HttpResponse response =
        PlainResponse(405, "DELETE ends a session; no Mcp-Session-Id given.");
    response.SetHeader("Allow", "POST, DELETE, OPTIONS");
    return response;
  }
  std::lock_guard lock(sessions_mutex_);
  const auto it = sessions_.find(*id);
  if (it == sessions_.end()) {
    return PlainResponse(404, "Session not found.");
  }
  activity_->Add(std::format("session ended: {}", it->second.client_name));
  sessions_.erase(it);
  HttpResponse response;
  response.status = 204;
  return response;
}

HttpResponse Endpoint::HandleModern(const HttpRequest& request,
                                    const Json& message, HostBridge& host) {
  requests_.fetch_add(1);
  const MessageKind kind = Classify(message);
  if (kind == MessageKind::kInvalid || kind == MessageKind::kResponse) {
    return ErrorResponse(400, IdOf(message), kInvalidRequest,
                         "Invalid Request: expected one JSON-RPC request "
                         "or notification");
  }
  if (kind == MessageKind::kNotification) {
    return Accepted();
  }
  const Json id = IdOf(message);
  // Classify guarantees a string method for requests.
  const std::string method = *message.GetString("method");
  const Json* params_value = message.GetObject("params");
  const Json params = params_value ? *params_value : Json::Object();
  const Json* meta = params.GetObject("_meta");
  const std::string* version =
      meta ? meta->GetString(kMetaProtocolVersion) : nullptr;
  if (version == nullptr) {
    return ErrorResponse(400, id, kInvalidParams,
                         std::format("Invalid params: _meta[\"{}\"] is "
                                     "required",
                                     kMetaProtocolVersion));
  }
  if (*version != kModernProtocolVersion) {
    Json supported = Json::Array();
    supported.Push(Json::String(std::string(kModernProtocolVersion)));
    for (std::string_view legacy : kLegacyProtocolVersions) {
      supported.Push(Json::String(std::string(legacy)));
    }
    Json data = Json::Object();
    data.Set("supported", std::move(supported)).Set("requested", *version);
    activity_->Add(
        std::format("unsupported protocol version {}", Summary(*version)));
    return ErrorResponse(400, id, kUnsupportedProtocolVersion,
                         "Unsupported protocol version", std::move(data));
  }
  if (meta->GetObject(kMetaClientCapabilities) == nullptr) {
    return ErrorResponse(400, id, kInvalidParams,
                         std::format("Invalid params: _meta[\"{}\"] is "
                                     "required",
                                     kMetaClientCapabilities));
  }

  // The transport's mirrored headers must match the body.
  const std::string* version_header =
      request.FindHeader("MCP-Protocol-Version");
  if (version_header == nullptr || *version_header != *version) {
    return ErrorResponse(400, id, kHeaderMismatch,
                         "Header mismatch: MCP-Protocol-Version header "
                         "missing or different from the body");
  }
  const std::string* method_header = request.FindHeader("Mcp-Method");
  if (method_header == nullptr || *method_header != method) {
    return ErrorResponse(400, id, kHeaderMismatch,
                         "Header mismatch: Mcp-Method header missing or "
                         "different from the body");
  }
  if (method == "tools/call" || method == "resources/read" ||
      method == "prompts/get") {
    const std::string* body_name =
        params.GetString(method == "resources/read" ? std::string_view("uri")
                                                    : std::string_view("name"));
    const std::string* name_header = request.FindHeader("Mcp-Name");
    std::string decoded;
    if (name_header == nullptr || body_name == nullptr ||
        !DecodeHeaderValue(*name_header, decoded) || decoded != *body_name) {
      return ErrorResponse(400, id, kHeaderMismatch,
                           "Header mismatch: Mcp-Name header missing or "
                           "different from the body");
    }
  }

  const CallContext context = CallContext::For(*version);
  Outcome outcome = Dispatch(method, params, context, host);
  if (!outcome.result) {
    const int status = outcome.error_code == kMethodNotFound ? 404 : 200;
    return JsonResponse(status, outcome.ToResponse(id));
  }
  Json& result = *outcome.result;
  result.Set("resultType", "complete");
  Json result_meta =
      result.GetObject("_meta") ? *result.GetObject("_meta") : Json::Object();
  result_meta.Set(kMetaServerInfo, ServerInfo());
  result.Set("_meta", std::move(result_meta));
  return JsonResponse(200, outcome.ToResponse(id));
}

HttpResponse Endpoint::HandleInitialize(const Json& message) {
  requests_.fetch_add(1);
  if (Classify(message) != MessageKind::kRequest) {
    return ErrorResponse(400, IdOf(message), kInvalidRequest,
                         "Invalid Request: initialize must be a request");
  }
  const Json id = IdOf(message);
  const Json* params = message.GetObject("params");
  const std::string* requested =
      params ? params->GetString("protocolVersion") : nullptr;
  if (requested == nullptr) {
    return ErrorResponse(400, id, kInvalidParams,
                         "Invalid params: protocolVersion is required");
  }
  const std::string version = IsLegacyVersion(*requested)
                                  ? *requested
                                  : std::string(kLegacyProtocolVersions[0]);
  std::string client = "unknown client";
  if (const Json* info = params->GetObject("clientInfo")) {
    const std::string* name = info->GetString("name");
    const std::string* client_version = info->GetString("version");
    client = name ? Summary(*name) : client;
    if (client_version) {
      client += " " + Summary(*client_version);
    }
  }
  const std::string session_id = CreateSession(version, client);
  activity_->Add(std::format("initialize: {} (protocol {})", client, version));

  Json result = Json::Object();
  result.Set("protocolVersion", version)
      .Set("capabilities", Capabilities(/*legacy=*/true))
      .Set("serverInfo", ServerInfo());
  if (!options_->instructions.empty()) {
    result.Set("instructions", options_->instructions);
  }
  HttpResponse response = JsonResponse(200, MakeResult(id, std::move(result)));
  response.SetHeader("Mcp-Session-Id", session_id);
  return response;
}

HttpResponse Endpoint::HandleLegacy(const HttpRequest& request,
                                    const Json& message, HostBridge& host) {
  const std::string* session_id = request.FindHeader("Mcp-Session-Id");
  if (session_id == nullptr) {
    requests_.fetch_add(1);
    return ErrorResponse(400, IdOf(message), kInvalidRequest,
                         "Bad Request: no Mcp-Session-Id header. Send "
                         "initialize first, or use protocol 2026-07-28 "
                         "with params._meta.");
  }
  const std::string version = TouchSession(*session_id);
  if (version.empty()) {
    requests_.fetch_add(1);
    return ErrorResponse(404, IdOf(message), kInvalidRequest,
                         "Session not found. Send initialize again.");
  }
  if (const std::string* header = request.FindHeader("MCP-Protocol-Version")) {
    if (!IsLegacyVersion(*header)) {
      requests_.fetch_add(1);
      return ErrorResponse(400, IdOf(message), kInvalidRequest,
                           "Bad Request: unsupported MCP-Protocol-Version");
    }
  }
  const CallContext context = CallContext::For(version);

  if (message.is_array()) {
    if (version != kBatchProtocolVersion || message.size() == 0) {
      requests_.fetch_add(1);
      return ErrorResponse(400, Json::Null(), kInvalidRequest,
                           "Invalid Request: batches are not supported in "
                           "this protocol version");
    }
    Json responses = Json::Array();
    for (const Json& item : message.array_items()) {
      Json response = HandleLegacyMessage(item, context, host);
      if (!response.is_null()) {
        responses.Push(std::move(response));
      }
    }
    return responses.size() == 0 ? Accepted() : JsonResponse(200, responses);
  }
  Json response = HandleLegacyMessage(message, context, host);
  if (response.is_null()) {
    return Accepted();
  }
  const bool rejected = response.Find("error") != nullptr &&
                        Classify(message) == MessageKind::kInvalid;
  return JsonResponse(rejected ? 400 : 200, response);
}

Json Endpoint::HandleLegacyMessage(const Json& message,
                                   const CallContext& context,
                                   HostBridge& host) {
  requests_.fetch_add(1);
  switch (Classify(message)) {
    case MessageKind::kInvalid:
      return MakeError(IdOf(message), kInvalidRequest, "Invalid Request");
    case MessageKind::kNotification:
    case MessageKind::kResponse:
      // notifications/initialized, notifications/cancelled, responses to
      // server requests (none are sent): nothing to answer.
      return Json::Null();
    case MessageKind::kRequest:
      break;
  }
  const Json id = IdOf(message);
  const std::string& method = *message.GetString("method");
  if (method == "initialize") {
    return MakeError(id, kInvalidRequest,
                     "Invalid Request: session already initialized");
  }
  const Json* params = message.GetObject("params");
  return Dispatch(method, params ? *params : Json::Object(), context, host)
      .ToResponse(id);
}

Endpoint::Outcome Endpoint::Dispatch(std::string_view method,
                                     const Json& params,
                                     const CallContext& context,
                                     HostBridge& host) {
  if (method == "tools/call") {
    return CallTool(params, context, host);
  }
  if (method == "tools/list") {
    return Outcome::Success(ListTools(context));
  }
  if (method == "resources/read") {
    return ReadResource(params, context, host);
  }
  if (method == "resources/list") {
    return Outcome::Success(ListResources(context));
  }
  if (method == "resources/templates/list") {
    Json result = Json::Object();
    result.Set("resourceTemplates", Json::Array());
    if (context.modern) {
      result.Set("ttlMs", 0).Set("cacheScope", "private");
    }
    return Outcome::Success(std::move(result));
  }
  if (method == "server/discover" && context.modern) {
    Json versions = Json::Array();
    versions.Push(Json::String(std::string(kModernProtocolVersion)));
    for (std::string_view legacy : kLegacyProtocolVersions) {
      versions.Push(Json::String(std::string(legacy)));
    }
    Json result = Json::Object();
    result.Set("supportedVersions", std::move(versions))
        .Set("capabilities", Capabilities(/*legacy=*/false));
    if (!options_->instructions.empty()) {
      result.Set("instructions", options_->instructions);
    }
    result.Set("ttlMs", 0).Set("cacheScope", "private");
    return Outcome::Success(std::move(result));
  }
  if (method == "ping" && !context.modern) {
    return Outcome::Success(Json::Object());
  }
  return Outcome::Failure(kMethodNotFound,
                          std::format("Method not found: {}", Summary(method)));
}

Endpoint::Outcome Endpoint::CallTool(const Json& params,
                                     const CallContext& context,
                                     HostBridge& host) {
  const std::string* name = params.GetString("name");
  if (name == nullptr) {
    return Outcome::Failure(kInvalidParams,
                            "Invalid params: tools/call needs params.name");
  }
  std::shared_ptr<const ToolDefinition> tool = registry_->FindTool(*name);
  if (tool == nullptr) {
    return Outcome::Failure(kInvalidParams,
                            std::format("Unknown tool: {}", Summary(*name)));
  }
  const Json* arguments_value = params.Find("arguments");
  if (arguments_value != nullptr && !arguments_value->is_null() &&
      !arguments_value->is_object()) {
    return Outcome::Failure(kInvalidParams,
                            "Invalid params: arguments must be an object");
  }
  Json arguments = arguments_value != nullptr && arguments_value->is_object()
                       ? *arguments_value
                       : Json::Object();
  tool_calls_.fetch_add(1);

  ToolResult result;
  const std::optional<std::string> invalid =
      ValidateAgainstSchema(tool->input_schema, arguments);
  const auto start = std::chrono::steady_clock::now();
  if (invalid) {
    // Reported as a tool error (not a protocol error) so the model can fix
    // its arguments, as the 2025-11-25 revision recommends.
    result = ToolResult::Error(
        std::format("Invalid arguments for {}: {}", tool->name, *invalid));
  } else {
    result = host.CallTool(tool, std::move(arguments));
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::string first_text;
  for (const ContentItem& item : result.content) {
    if (item.kind == ContentItem::Kind::kText) {
      first_text = Summary(item.text);
      break;
    }
  }
  activity_->Add(
      result.is_error
          ? std::format("tools/call {}: error: {}", tool->name, first_text)
          : std::format("tools/call {} ({} ms)", tool->name, elapsed.count()));

  Json content = Json::Array();
  for (const ContentItem& item : result.content) {
    content.Push(ContentToJson(item));
  }
  if (content.size() == 0 && result.structured) {
    ContentItem text;
    text.text = result.structured->DumpPretty();
    content.Push(ContentToJson(text));
  }
  Json json = Json::Object();
  json.Set("content", std::move(content));
  if (context.structured && result.structured) {
    json.Set("structuredContent", *result.structured);
  }
  json.Set("isError", result.is_error);
  return Outcome::Success(std::move(json));
}

Endpoint::Outcome Endpoint::ReadResource(const Json& params,
                                         const CallContext& context,
                                         HostBridge& host) {
  const std::string* uri = params.GetString("uri");
  if (uri == nullptr) {
    return Outcome::Failure(kInvalidParams,
                            "Invalid params: resources/read needs params.uri");
  }
  std::shared_ptr<const ResourceDefinition> resource =
      registry_->FindResource(*uri);
  if (resource == nullptr) {
    Json data = Json::Object();
    data.Set("uri", *uri);
    return Outcome::Failure(
        context.modern ? kInvalidParams : kLegacyResourceNotFound,
        "Resource not found", std::move(data));
  }
  core::Result<ResourceContents, std::string> read =
      host.ReadResource(resource);
  if (!read.ok()) {
    activity_->Add(std::format("resources/read {}: error: {}", resource->uri,
                               Summary(read.error())));
    return Outcome::Failure(kInternalError, read.error());
  }
  activity_->Add(std::format("resources/read {}", resource->uri));
  const ResourceContents& contents = read.value();
  Json item = Json::Object();
  item.Set("uri", resource->uri)
      .Set("mimeType", contents.mime_type.empty() ? resource->mime_type
                                                  : contents.mime_type)
      .Set("text", contents.text);
  Json list = Json::Array();
  list.Push(std::move(item));
  Json result = Json::Object();
  result.Set("contents", std::move(list));
  if (context.modern) {
    result.Set("ttlMs", 0).Set("cacheScope", "private");
  }
  return Outcome::Success(std::move(result));
}

Json Endpoint::ListTools(const CallContext& context) const {
  Json tools = Json::Array();
  for (const std::shared_ptr<const ToolDefinition>& tool : registry_->Tools()) {
    Json json = Json::Object();
    json.Set("name", tool->name);
    if (context.structured && !tool->title.empty()) {
      json.Set("title", tool->title);
    }
    json.Set("description", tool->description)
        .Set("inputSchema", tool->input_schema.is_object()
                                ? tool->input_schema
                                : Json::Object().Set("type", "object"));
    if (context.structured && tool->output_schema) {
      json.Set("outputSchema", *tool->output_schema);
    }
    if (context.annotations) {
      Json annotations = Json::Object();
      if (!tool->title.empty()) {
        annotations.Set("title", tool->title);
      }
      annotations.Set("readOnlyHint", tool->read_only)
          .Set("destructiveHint", tool->destructive)
          .Set("openWorldHint", false);
      json.Set("annotations", std::move(annotations));
    }
    tools.Push(std::move(json));
  }
  Json result = Json::Object();
  result.Set("tools", std::move(tools));
  if (context.modern) {
    result.Set("ttlMs", 0).Set("cacheScope", "private");
  }
  return result;
}

Json Endpoint::ListResources(const CallContext& context) const {
  Json resources = Json::Array();
  for (const std::shared_ptr<const ResourceDefinition>& resource :
       registry_->Resources()) {
    Json json = Json::Object();
    json.Set("uri", resource->uri).Set("name", resource->name);
    if (context.structured && !resource->title.empty()) {
      json.Set("title", resource->title);
    }
    if (!resource->description.empty()) {
      json.Set("description", resource->description);
    }
    if (!resource->mime_type.empty()) {
      json.Set("mimeType", resource->mime_type);
    }
    resources.Push(std::move(json));
  }
  Json result = Json::Object();
  result.Set("resources", std::move(resources));
  if (context.modern) {
    result.Set("ttlMs", 0).Set("cacheScope", "private");
  }
  return result;
}

Json Endpoint::ServerInfo() const {
  Json info = Json::Object();
  info.Set("name", options_->name);
  if (!options_->title.empty()) {
    info.Set("title", options_->title);
  }
  info.Set("version", options_->version);
  return info;
}

Json Endpoint::Capabilities(bool legacy) const {
  Json tools = Json::Object();
  Json resources = Json::Object();
  if (legacy) {
    tools.Set("listChanged", false);
    resources.Set("subscribe", false).Set("listChanged", false);
  }
  Json capabilities = Json::Object();
  capabilities.Set("tools", std::move(tools))
      .Set("resources", std::move(resources));
  return capabilities;
}

void Endpoint::ClearSessions() {
  std::lock_guard lock(sessions_mutex_);
  sessions_.clear();
}

int Endpoint::session_count() const {
  std::lock_guard lock(sessions_mutex_);
  return static_cast<int>(sessions_.size());
}

std::string Endpoint::TouchSession(std::string_view id) {
  std::lock_guard lock(sessions_mutex_);
  const auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    return std::string();
  }
  it->second.last_used = std::chrono::steady_clock::now();
  return it->second.protocol_version;
}

std::string Endpoint::CreateSession(std::string protocol_version,
                                    std::string client_name) {
  std::string id = NewSessionId();
  std::lock_guard lock(sessions_mutex_);
  if (sessions_.size() >= kMaxSessions) {
    auto oldest = std::min_element(
        sessions_.begin(), sessions_.end(), [](const auto& a, const auto& b) {
          return a.second.last_used < b.second.last_used;
        });
    sessions_.erase(oldest);
  }
  sessions_[id] = Session{std::move(protocol_version), std::move(client_name),
                          std::chrono::steady_clock::now()};
  return id;
}

}  // namespace blue::mcp
