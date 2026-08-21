#include "serial_records.h"

namespace {

bool isUnreserved(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == '.' || c == '~';
}

void emitEncodedChar(char c) {
  if (isUnreserved(c)) {
    Serial.write(c);
    return;
  }
  static const char hex[] = "0123456789ABCDEF";
  Serial.write('%');
  Serial.write(hex[((uint8_t)c >> 4) & 0x0F]);
  Serial.write(hex[(uint8_t)c & 0x0F]);
}

void emitKey(const char *key) {
  Serial.write(' ');
  Serial.print(key);
  Serial.write('=');
}

}  // namespace

void recordBegin(const char *type) {
  Serial.print(SERIAL_RECORD_PREFIX);
  Serial.print(F(" v="));
  Serial.print(SERIAL_RECORD_VERSION);
  Serial.print(F(" t="));
  Serial.print(type);
}

void recordEnd() { Serial.println(); }

void recordKeyUint(const char *key, uint32_t value) {
  emitKey(key);
  Serial.print(value);
}

void recordKeyInt(const char *key, int32_t value) {
  emitKey(key);
  Serial.print(value);
}

void recordKeyBool(const char *key, bool value) {
  emitKey(key);
  Serial.print(value ? '1' : '0');
}

void recordKeyHex(const char *key, uint32_t value) {
  emitKey(key);
  Serial.print(F("0x"));
  Serial.print(value, HEX);
}

void recordKeyFloat(const char *key, float value, uint8_t decimals, bool available) {
  emitKey(key);
  if (!available || isnan(value)) {
    Serial.print(F("nan"));
    return;
  }
  Serial.print(value, decimals);
}

void recordKeyOptionalBool(const char *key, bool value, bool available) {
  emitKey(key);
  if (!available) {
    // Reporting an unevaluated field as 0 would let a tool display a confident "no fault" for
    // something the firmware never checked, which is worse than saying nothing.
    Serial.print(F("nan"));
    return;
  }
  Serial.print(value ? '1' : '0');
}

void recordKeyToken(const char *key, const char *token) {
  emitKey(key);
  Serial.print(token);
}

void recordKeyText(const char *key, const char *text) {
  emitKey(key);
  if (text == nullptr) return;
  for (const char *p = text; *p != '\0'; ++p) emitEncodedChar(*p);
}

void recordKeyText(const char *key, const __FlashStringHelper *text) {
  emitKey(key);
  if (text == nullptr) return;
  PGM_P p = reinterpret_cast<PGM_P>(text);
  while (true) {
    const char c = pgm_read_byte(p++);
    if (c == '\0') break;
    emitEncodedChar(c);
  }
}

void emitCalibrationProgress(uint8_t motor, const char *stage, uint8_t percent, bool energised) {
  recordBegin("calprog");
  recordKeyUint("m", motor);
  recordKeyToken("stage", stage);
  recordKeyUint("pct", percent > 100 ? 100 : percent);
  recordKeyBool("energised", energised);
  recordEnd();
}

void emitAlignmentPending(uint8_t motor, uint16_t polePairs, int8_t direction,
                          float electricalOffset) {
  recordBegin("calpend");
  recordKeyUint("m", motor);
  recordKeyToken("stage", "align");
  recordKeyUint("pp", polePairs);
  recordKeyInt("dir", direction);
  recordKeyFloat("offset", electricalOffset, 6);
  recordEnd();
}

void emitCharacteristicsPending(uint8_t motor, float resistance, float inductanceD,
                                float inductanceQ) {
  recordBegin("calpend");
  recordKeyUint("m", motor);
  recordKeyToken("stage", "charac");
  recordKeyFloat("r", resistance, 6);
  recordKeyFloat("ld", inductanceD, 9);
  recordKeyFloat("lq", inductanceQ, 9);
  recordEnd();
}

void emitFaultRecord(const char *kind, const __FlashStringHelper *reason) {
  recordBegin("fault");
  recordKeyToken("kind", kind);
  recordKeyText("reason", reason);
  // Retained as a zero-valued v1 compatibility field; calibration has no retry timer.
  recordKeyUint("cooldown_ms", 0);
  recordEnd();
}
