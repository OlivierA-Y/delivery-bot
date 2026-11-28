#include "protocol.h"

#include <cJSON.h>

#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

namespace hotelbot {
namespace {
bool digit(char c) {
  return c >= '0' && c <= '9';
}

const char* numberEnd(const char* cursor) {
  if (*cursor == '-') ++cursor;
  if (*cursor == '0') ++cursor;
  else {
    if (!digit(*cursor)) return nullptr;
    while (digit(*cursor)) ++cursor;
  }
  if (*cursor == '.') {
    if (!digit(*++cursor)) return nullptr;
    while (digit(*cursor)) ++cursor;
  }
  if (*cursor == 'e' || *cursor == 'E') {
    ++cursor;
    if (*cursor == '+' || *cursor == '-') ++cursor;
    if (!digit(*cursor)) return nullptr;
    while (digit(*cursor)) ++cursor;
  }
  return std::strchr(",} \t\r", *cursor) ? cursor : nullptr;
}

bool flatObject(const char* text) {
  while (*text == ' ' || *text == '\r' || *text == '\t') ++text;
  if (*text++ != '{') return false;
  bool quoted = false;
  for (; *text; ++text) {
    if (*text == '"') quoted = !quoted;
    if (*text == '\\') return false;  // Fields and IDs in v1 use unescaped ASCII.
    if (quoted) {
      if (static_cast<unsigned char>(*text) < 32) return false;
      continue;
    }
    if (*text == '{' || *text == '[' || *text == ']') return false;
    if (digit(*text) || *text == '-' || *text == '+') {
      const char* end = numberEnd(text);
      if (!end) return false;
      if (!*end) break;
      text = end - 1;
    }
  }
  return !quoted;
}

const cJSON* field(const cJSON* object, const char* name) {
  return cJSON_GetObjectItemCaseSensitive(object, name);
}

bool readId(const cJSON* item, Id& result) {
  if (!cJSON_IsString(item)) return false;
  const char* value = item->valuestring;
  const size_t length = std::strlen(value);
  if (!length || length >= result.size()) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!digit(c) && !(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') && c != '-' && c != '_') {
      return false;
    }
  }
  std::memcpy(result.data(), value, length + 1);
  return true;
}

bool readUnsigned(const cJSON* item, uint32_t& result, uint32_t minimum = 1) {
  if (!cJSON_IsNumber(item)) return false;
  const double value = item->valuedouble;
  if (!(value >= minimum && value <= UINT32_MAX) || value != std::floor(value)) return false;
  result = static_cast<uint32_t>(value);
  return true;
}

bool readNumber(const cJSON* item, float& result) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      std::abs(item->valuedouble) > 1'000'000) return false;
  result = static_cast<float>(item->valuedouble);
  return true;
}

bool uniqueFields(const cJSON* object) {
  for (const cJSON* item = object->child; item; item = item->next) {
    for (const cJSON* other = item->next; other; other = other->next) {
      if (!std::strcmp(item->string, other->string)) return false;
    }
  }
  return true;
}

void quote(char* output, size_t capacity, const char* text) {
  if (text) std::snprintf(output, capacity, "\"%s\"", text);
  else std::snprintf(output, capacity, "null");
}

const char* modeName(Mode mode) {
  switch (mode) {
    case Mode::Idle: return "idle";
    case Mode::Speed: return "speed";
    case Mode::Position: return "position";
  }
  return "idle";
}

const char* moveName(MoveStatus status) {
  switch (status) {
    case MoveStatus::Accepted: return "accepted";
    case MoveStatus::Moving: return "moving";
    case MoveStatus::Done: return "done";
    case MoveStatus::Canceled: return "canceled";
    case MoveStatus::Fault: return "fault";
  }
  return "fault";
}

void formatFaults(char (&output)[192], uint16_t faults) {
  struct Name { uint16_t bit; const char* text; };
  constexpr Name names[] = {
    {Uncalibrated, "uncalibrated"}, {LeftHall, "left_hall"}, {RightHall, "right_hall"},
    {LeftStall, "left_stall"}, {RightStall, "right_stall"}, {Pwm, "pwm"},
    {LinkTimeout, "link_timeout"}, {MoveTimeout, "move_timeout"},
    {LeftDirection, "left_direction"}, {RightDirection, "right_direction"},
  };
  size_t length = 1;
  output[0] = '[';
  for (const auto& entry : names) {
    if (!(faults & entry.bit)) continue;
    length += std::snprintf(output + length, sizeof(output) - length, "%s\"%s\"",
                            length > 1 ? "," : "", entry.text);
  }
  output[length++] = ']';
  output[length] = '\0';
}
}  // namespace

bool parseFrame(const char* line, Frame& frame) {
  if (std::strlen(line) > kMaxFrame || !flatObject(line)) return false;
  std::unique_ptr<cJSON, decltype(&cJSON_Delete)> document(cJSON_ParseWithOpts(line, nullptr, true), cJSON_Delete);
  const cJSON* object = document.get();
  if (!cJSON_IsObject(object) || !uniqueFields(object)) return false;
  const cJSON* type = field(object, "type");
  Frame parsed;
  if (!cJSON_IsString(type) || !readId(field(object, "session"), parsed.session)) return false;
  const char* name = type->valuestring;
  if (!std::strcmp(name, "hello")) {
    parsed.kind = Kind::Hello;
    if (!readUnsigned(field(object, "v"), parsed.version, 0)) return false;
  } else {
    if (!readUnsigned(field(object, "seq"), parsed.seq)) return false;
    if (!std::strcmp(name, "arm")) parsed.kind = Kind::Arm;
    else if (!std::strcmp(name, "stop")) parsed.kind = Kind::Stop;
    else if (!std::strcmp(name, "set_wheel_speeds")) {
      parsed.kind = Kind::Speed;
      if (!readNumber(field(object, "left_deg_s"), parsed.left) ||
          !readNumber(field(object, "right_deg_s"), parsed.right)) return false;
    } else if (!std::strcmp(name, "move_wheels")) {
      parsed.kind = Kind::Move;
      if (!readId(field(object, "move_id"), parsed.moveId) ||
          !readNumber(field(object, "left_deg"), parsed.left) ||
          !readNumber(field(object, "right_deg"), parsed.right) ||
          !readNumber(field(object, "max_deg_s"), parsed.maxSpeed)) return false;
    } else if (!std::strcmp(name, "keepalive")) {
      parsed.kind = Kind::KeepAlive;
      if (!readId(field(object, "move_id"), parsed.moveId)) return false;
    } else return false;
  }
  frame = parsed;
  return true;
}

int formatState(char* output, size_t capacity, const Protocol& protocol, const Feedback& feedback,
                const char* bootId, uint64_t now, const MoveReport* move) {
  if (!move) move = protocol.lastMove();
  char moveId[kIdBytes + 2], moveStatus[16], lastSeq[16], faults[192];
  quote(moveId, sizeof(moveId), move ? move->id.data() : nullptr);
  quote(moveStatus, sizeof(moveStatus), move ? moveName(move->status) : nullptr);
  if (protocol.lastSequence()) std::snprintf(lastSeq, sizeof(lastSeq), "%" PRIu32, protocol.lastSequence());
  else std::strcpy(lastSeq, "null");
  formatFaults(faults, protocol.faults());
  return std::snprintf(output, capacity,
      "{\"type\":\"state\",\"session\":\"%s\",\"boot\":\"%s\",\"sample_ms\":%" PRIu64
      ",\"left_count\":%" PRId64 ",\"right_count\":%" PRId64
      ",\"left_deg_s\":%.2f,\"right_deg_s\":%.2f,\"armed\":%s,\"ready\":%s,\"mode\":\"%s\""
      ",\"last_seq\":%s,\"move_id\":%s,\"move_status\":%s,\"faults\":%s}\n",
      protocol.session().data(), bootId, now, feedback.left.count, feedback.right.count,
      feedback.left.speed, feedback.right.speed, protocol.armed() ? "true" : "false",
      protocol.ready() ? "true" : "false", modeName(protocol.mode()), lastSeq, moveId, moveStatus, faults);
}

int formatReply(char* output, size_t capacity, const Reply& reply, const Frame& frame,
                const char* bootId, bool benchMoves, float maxWheelDegPerSecond) {
  if (reply.type == Reply::Type::HelloAck) {
    return std::snprintf(output, capacity,
        "{\"type\":\"hello_ack\",\"v\":1,\"session\":\"%s\",\"boot\":\"%s\","
        "\"capabilities\":[\"set_wheel_speeds\"%s],\"max_wheel_deg_s\":%.0f}\n",
        frame.session.data(), bootId, benchMoves ? ",\"move_wheels\"" : "", maxWheelDegPerSecond);
  }
  return std::snprintf(output, capacity,
      "{\"type\":\"error\",\"session\":\"%s\",\"boot\":\"%s\",\"seq\":%" PRIu32
      ",\"code\":\"%s\"}\n", frame.session.data(), bootId, frame.seq, reply.error);
}

}  // namespace hotelbot
