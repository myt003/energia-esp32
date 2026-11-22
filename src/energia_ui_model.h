// energia_ui_model.h
// Pure C++ (no Arduino/ESP-IDF dependencies) UI model for the Energia ESP32 OLED.
// Stories:
//   EN-ESP-002: finite-state list/detail UI, one-button navigation, safe truncation.
//   EN-ESP-003: score/status/last-update formatting; device never recalculates scores.
//   EN-ESP-004: top-employee alias formatting with privacy-neutral fallbacks.
//   EN-ESP-005: stale/offline marker supplied by the caller and rendered in labels.
//
// This file is deliberately host-testable: tests/ compiles it with g++ only.

#ifndef ENERGIA_UI_MODEL_H_
#define ENERGIA_UI_MODEL_H_

#include <cstddef>
#include <cstdint>

namespace energia {

// Fixed capacities keep RAM predictable on ESP32 and bound every copy.
constexpr std::size_t kNameCapacity = 24;    // department display name, incl. NUL
constexpr std::size_t kStatusCapacity = 20;  // server-provided status label
constexpr std::size_t kAliasCapacity = 20;   // approved display name/alias only
constexpr std::size_t kMaxDepartments = 12;
constexpr std::size_t kCacheCapacity = 4096; // NVS cache size for sanitized JSON

constexpr std::size_t kListRowChars = 15;    // fits the 128px OLED at 6px/char
constexpr std::size_t kDetailLineChars = 21; // 128px / ~6px per default glyph

enum class ScoreState : std::uint8_t {
  Final = 0,       // server published a final 0..100 score
  Provisional = 1, // server published a provisional score (clearly flagged)
  Unavailable = 2  // no score available; device must not invent one
};

enum class UiPage : std::uint8_t {
  List = 0,
  Detail = 1
};

enum class Button : std::uint8_t {
  Nav = 0,
  Power = 1
};

enum class PressKind : std::uint8_t {
  Short = 0,
  Long = 1
};

enum class PressEvent : std::uint8_t {
  None = 0,
  Short = 1,
  Long = 2
};

enum class UiEvent : std::uint8_t {
  None = 0,
  SelectionChanged = 1,
  DetailOpened = 2,
  BackToList = 3,
  DisplayToggled = 4,
  SleepRequested = 5
};

struct EmployeePreview {
  char displayName[kAliasCapacity];
  std::uint8_t score;      // 0..100, only meaningful when eligible == true
  bool eligible;

  EmployeePreview() : displayName{}, score(0), eligible(false) {}
};

struct DepartmentSummary {
  char name[kNameCapacity];
  char statusLabel[kStatusCapacity];
  std::uint32_t lastUpdateEpoch; // UTC epoch seconds from the server
  ScoreState scoreState;
  std::uint8_t score;            // server value only; never recalculated here
  bool hasScore;                 // false when the server sent null/unavailable
  EmployeePreview top;

  DepartmentSummary()
      : name{},
        statusLabel{},
        lastUpdateEpoch(0),
        scoreState(ScoreState::Unavailable),
        score(0),
        hasScore(false),
        top() {}
};

// Debounced two-threshold button decoder (EN-ESP-002 / EN-ESP-005).
// Call update() from the main loop with the raw active-low level already
// inverted by the caller (true == physically pressed).
class DebouncedButton {
 public:
  DebouncedButton(std::uint16_t debounceMs = 40, std::uint16_t longPressMs = 1200);

  PressEvent update(bool rawPressed, std::uint32_t nowMs);
  void reset(bool rawPressed, std::uint32_t nowMs);

  bool isPressed() const { return stablePressed_; }
  std::uint16_t debounceMs() const { return debounceMs_; }
  std::uint16_t longPressMs() const { return longPressMs_; }

 private:
  std::uint16_t debounceMs_;
  std::uint16_t longPressMs_;
  bool rawPressed_;
  bool stablePressed_;
  bool longFired_;
  std::uint32_t lastChangeMs_;
  std::uint32_t pressStartMs_;
};

// Pure view state + formatters. No display, WiFi or time dependencies.
class UiModel {
 public:
  static constexpr std::uint8_t kVisibleRows = 4;

  UiModel();

  void reset();

  // Copies summaries into internal storage; rejects too-large lists.
  bool setDepartments(const DepartmentSummary* items, std::size_t count);

  std::size_t count() const { return count_; }
  const DepartmentSummary* item(std::size_t index) const;
  const DepartmentSummary* selected() const { return item(selectedIndex_); }

  std::size_t selectedIndex() const { return selectedIndex_; }
  std::size_t windowStart() const { return windowStart_; }
  UiPage page() const { return page_; }
  bool displayOn() const { return displayOn_; }
  bool stale() const { return stale_; }
  bool recognitionEnabled() const { return recognitionEnabled_; }
  std::uint32_t generatedAt() const { return generatedAt_; }

  void setDisplayOn(bool on) { displayOn_ = on; }
  void setStale(bool stale) { stale_ = stale; }
  void setRecognitionEnabled(bool enabled) { recognitionEnabled_ = enabled; }
  void setGeneratedAt(std::uint32_t epoch) { generatedAt_ = epoch; }

  // Single entry point for the FSM (EN-ESP-002 and EN-ESP-005).
  UiEvent handleButton(Button button, PressKind kind);

  // Content-based signature lets main.cpp redraw only on real changes
  // (EN-ESP-003). nowEpoch/60 makes the "updated" line tick once per minute.
  std::uint32_t signature(std::uint32_t nowEpoch,
                          std::int32_t tzOffsetSeconds) const;

  // --- Safe string helpers -------------------------------------------------
  // Always NUL-terminates when capacity > 0. Never reads past src's NUL.
  static std::size_t copyTruncated(char* dst, std::size_t capacity, const char* src);

  // Like copyTruncated(), but replaces the last kept char with 'marker' when
  // truncation happened so users can tell the value was shortened.
  static std::size_t copyTruncatedEllipsis(char* dst, std::size_t capacity,
                                           const char* src, char marker = '~');

  // --- Formatters ----------------------------------------------------------
  static void formatListRow(const DepartmentSummary& item, char* out,
                            std::size_t capacity);
  static void formatScoreLine(const DepartmentSummary& item, char* out,
                              std::size_t capacity);
  static void formatStatusLine(const DepartmentSummary& item, bool stale,
                               char* out, std::size_t capacity);
  static void formatUpdatedLine(const DepartmentSummary& item,
                                std::uint32_t nowEpoch,
                                std::int32_t tzOffsetSeconds, bool stale,
                                char* out, std::size_t capacity);
  static void formatTopLine(const DepartmentSummary& item,
                            bool recognitionEnabled, char* out,
                            std::size_t capacity);
  static void formatClock(std::uint32_t epoch, std::int32_t tzOffsetSeconds,
                          char* out, std::size_t capacity);
  static void formatAge(std::uint32_t epoch, std::uint32_t nowEpoch,
                        char* out, std::size_t capacity);

 private:
  void recomputeWindow();

  DepartmentSummary items_[kMaxDepartments];
  std::size_t count_;
  std::size_t selectedIndex_;
  std::size_t windowStart_;
  UiPage page_;
  bool displayOn_;
  bool stale_;
  bool recognitionEnabled_;
  std::uint32_t generatedAt_;
};

} // namespace energia

#endif // ENERGIA_UI_MODEL_H_
