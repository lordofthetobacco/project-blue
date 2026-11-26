#include "net/wire_error.h"

#include <string_view>

namespace blue::net {

std::string_view WireErrorText(WireError error) {
  switch (error) {
    case WireError::kOverflow:
      return "write buffer or budget exhausted";
    case WireError::kTruncated:
      return "payload truncated";
    case WireError::kOutOfRange:
      return "value out of range";
    case WireError::kInvalidUtf8:
      return "string is not valid UTF-8";
    case WireError::kNonCanonical:
      return "non-canonical encoding";
    case WireError::kTrailingData:
      return "trailing data after the last field";
    case WireError::kUnknownPacketKind:
      return "unknown packet kind";
    case WireError::kUnknownMessage:
      return "unknown message type";
    case WireError::kUnexpectedMessage:
      return "message not allowed here";
    case WireError::kMissingBaseline:
      return "delta baseline not available";
    case WireError::kInconsistent:
      return "delta contradicts its baseline";
    case WireError::kUsage:
      return "API misuse";
    case WireError::kWindowFull:
      return "reliable send window full";
    case WireError::kMessageTooLarge:
      return "message exceeds the reliable budget";
  }
  return "unknown wire error";
}

}  // namespace blue::net
