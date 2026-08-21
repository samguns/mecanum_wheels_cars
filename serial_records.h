#pragma once

// Structured, versioned, single-line serial records.
//
// Why this exists (research D1): the existing console output is prose written for humans. Values
// arrive split across lines, refusals are free text, and internal spacing varies, so a tool that
// scraped it would eventually misreport a motor's electrical parameters after an ordinary firmware
// reword. This module adds a machine-readable format alongside the prose, which stays unchanged.
//
// Contract: specs/003-v13-configurator/contracts/serial-protocol.md
//
// Every line here begins with "#V13 " and no prose line ever does, so a human and a tool can share
// one session. Records are emitted only from the communications context, never from the
// deterministic control path (FR-035).

#include <Arduino.h>
#include <stdint.h>

// Bump only on a breaking change: removing or renaming a key, changing a unit, or changing a
// value's meaning. Adding a key or a record type does NOT bump this.
static const uint8_t SERIAL_RECORD_VERSION = 1;

static const char SERIAL_RECORD_PREFIX[] = "#V13";

// Raw byte that aborts a calibration stage. Chosen because it must be recognisable without a line
// terminator and without the Commander's line assembly, which is not running during a blocked
// stage. 0x18 is ASCII CAN, "cancel".
static const uint8_t SERIAL_ABORT_BYTE = 0x18;

// ---------------------------------------------------------------------------
// Record framing
// ---------------------------------------------------------------------------

// Begins a record: emits "#V13 v=<version> t=<type>". Follow with field emitters, then endRecord().
void recordBegin(const char *type);

// Ends the record with a newline. A record is never split across lines.
void recordEnd();

void recordKeyUint(const char *key, uint32_t value);
void recordKeyInt(const char *key, int32_t value);
void recordKeyBool(const char *key, bool value);
void recordKeyHex(const char *key, uint32_t value);

// Emits `nan` when `available` is false, so an unpopulated field is explicitly absent rather than
// silently reported as zero or false.
void recordKeyFloat(const char *key, float value, uint8_t decimals, bool available = true);
void recordKeyOptionalBool(const char *key, bool value, bool available);

// Emits a bare token with no encoding. Use only for values known to contain no spaces.
void recordKeyToken(const char *key, const char *token);

// Percent-encodes anything that is not an unreserved character, so free text such as a refusal
// reason survives intact inside a space-separated record.
void recordKeyText(const char *key, const char *text);
void recordKeyText(const char *key, const __FlashStringHelper *text);

// Contract-level records that do not depend on sketch-owned types. Keeping these here makes their
// exact field names and units testable and prevents calibration call sites from hand-assembling
// subtly different wire formats.
void emitCalibrationProgress(uint8_t motor, const char *stage, uint8_t percent, bool energised);
void emitAlignmentPending(uint8_t motor, uint16_t polePairs, int8_t direction,
                          float electricalOffset);
void emitCharacteristicsPending(uint8_t motor, float resistance, float inductanceD,
                                float inductanceQ);
void emitFaultRecord(const char *kind, const __FlashStringHelper *reason);
