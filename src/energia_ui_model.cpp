// energia_ui_model.cpp -- see energia_ui_model.h for story mapping.
// Pure C++: compiles with g++ and requires no ESP32/Arduino headers.

#include "energia_ui_model.h"

#include <cstdio>
#include <cstring>

namespace energia {
namespace {

unsigned clampedScore(std::uint8_t score, bool hasScore) {
  if (!hasScore) return 0u;
  unsigned value = static_cast<unsigned>(score);
  return value > 100u ? 100u : value;
}

void ensureNotBlank(char* out, std::size_t capacity) {
  if (capacity == 0) return;
  bool blank = true;
  for (std::size_t i = 0; i < capacity; ++i) {
    if (out[i] != '\0') {
      blank = false;
      break;
    }
  }
  if (blank) {
    out[0] = '-';
    if (capacity > 1) out[1] = '\0';
  }
}

} // namespace

// ---------------------------------------------------------------------------
// DebouncedButton -- EN-ESP-002 (navigation) and EN-ESP-005 (power long press)
// ---------------------------------------------------------------------------

DebouncedButton::DebouncedButton(std::uint16_t debounceMs,
                                 std::uint16_t longPressMs)
    : debounceMs_(debounceMs),
      longPressMs_(longPressMs),
      rawPressed_(false),
      stablePressed_(false),
      longFired_(false),
      lastChangeMs_(0),
      pressStartMs_(0) {}

PressEvent DebouncedButton::update(bool rawPressed, std::uint32_t nowMs) {
  if (rawPressed != rawPressed_) {
    rawPressed_ = rawPressed;
    lastChangeMs_ = nowMs;
  }

  // Ignore changes that have not been stable for the debounce window.
  const std::uint32_t stableFor = nowMs - lastChangeMs_;

  if (!stablePressed_ && rawPressed_ && stableFor >= debounceMs_) {
    stablePressed_ = true;
    longFired_ = false;
    pressStartMs_ = nowMs;
    return PressEvent::None;
  }

  if (stablePressed_ && !rawPressed_ && stableFor >= debounceMs_) {
    stablePressed_ = false;
    const PressEvent result = longFired_ ? PressEvent::None : PressEvent::Short;
    longFired_ = false;
    return result;
  }

  // Long press fires once, while held, and suppresses the release short press.
  if (stablePressed_ && !longFired_ &&
      (nowMs - pressStartMs_) >= longPressMs_) {
    longFired_ = true;
    return PressEvent::Long;
  }

  return PressEvent::None;
}

void DebouncedButton::reset(bool rawPressed, std::uint32_t nowMs) {
  rawPressed_ = rawPressed;
  stablePressed_ = rawPressed;
  longFired_ = false;
  lastChangeMs_ = nowMs;
  pressStartMs_ = nowMs;
}

// ---------------------------------------------------------------------------
// UiModel -- EN-ESP-002 / EN-ESP-003 / EN-ESP-004 / EN-ESP-005
// ---------------------------------------------------------------------------

UiModel::UiModel() { reset(); }

void UiModel::reset() {
  for (std::size_t i = 0; i < kMaxDepartments; ++i) items_[i] = DepartmentSummary();
  count_ = 0;
  selectedIndex_ = 0;
  windowStart_ = 0;
  page_ = UiPage::List;
  displayOn_ = true;
  stale_ = true; // No fresh data until the first successful poll succeeds.
  recognitionEnabled_ = true;
  generatedAt_ = 0;
}

bool UiModel::setDepartments(const DepartmentSummary* items, std::size_t count) {
  if (count > kMaxDepartments) return false;
  if (count > 0 && items == nullptr) return false;

  for (std::size_t i = 0; i < count; ++i) items_[i] = items[i];
  for (std::size_t i = count; i < count_; ++i) items_[i] = DepartmentSummary();
  count_ = count;

  if (selectedIndex_ >= count_) selectedIndex_ = 0;
  recomputeWindow();
  return true;
}

const DepartmentSummary* UiModel::item(std::size_t index) const {
  return index < count_ ? &items_[index] : nullptr;
}

void UiModel::recomputeWindow() {
  if (count_ == 0) {
    selectedIndex_ = 0;
    windowStart_ = 0;
    return;
  }
  if (selectedIndex_ < windowStart_) {
    windowStart_ = selectedIndex_;
  } else if (selectedIndex_ >= windowStart_ + kVisibleRows) {
    windowStart_ = selectedIndex_ - (kVisibleRows - 1);
  }
  const std::size_t maxStart = count_ - 1;
  if (windowStart_ > maxStart) windowStart_ = maxStart;
}

UiEvent UiModel::handleButton(Button button, PressKind kind) {
  if (button == Button::Nav) {
    if (kind == PressKind::Short) {
      if (page_ != UiPage::List || count_ == 0) return UiEvent::None;
      selectedIndex_ = (selectedIndex_ + 1) % count_;
      recomputeWindow();
      return UiEvent::SelectionChanged;
    }
    if (page_ == UiPage::List) {
      if (count_ == 0) return UiEvent::None;
      page_ = UiPage::Detail;
      return UiEvent::DetailOpened;
    }
    page_ = UiPage::List;
    return UiEvent::BackToList;
  }

  if (button == Button::Power) {
    if (kind == PressKind::Short) {
      displayOn_ = !displayOn_;
      return UiEvent::DisplayToggled;
    }
    // The caller waits for physical release before entering deep sleep so the
    // active-low wake pin is not already asserted (EN-ESP-005).
    return UiEvent::SleepRequested;
  }

  return UiEvent::None;
}

// ---------------------------------------------------------------------------
// Safe truncation
// ---------------------------------------------------------------------------

std::size_t UiModel::copyTruncated(char* dst, std::size_t capacity,
                                   const char* src) {
  if (capacity == 0) return 0;
  if (dst == nullptr) return 0;
  if (src == nullptr) {
    dst[0] = '\0';
    return 0;
  }
  std::size_t n = 0;
  const std::size_t limit = capacity - 1;
  while (n < limit && src[n] != '\0') {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
  return n;
}

std::size_t UiModel::copyTruncatedEllipsis(char* dst, std::size_t capacity,
                                           const char* src, char marker) {
  if (capacity == 0) return 0;
  if (dst == nullptr) return 0;
  if (src == nullptr) {
    dst[0] = '\0';
    return 0;
  }

  const std::size_t n = copyTruncated(dst, capacity, src);
  if (src[n] != '\0' && n > 0) dst[n - 1] = marker;
  return n;
}

// ---------------------------------------------------------------------------
// Formatters (all output is bounded and ASCII-only for the SSD1306 font)
// ---------------------------------------------------------------------------

void UiModel::formatListRow(const DepartmentSummary& item, char* out,
                            std::size_t capacity) {
  copyTruncatedEllipsis(out, capacity, item.name);
  ensureNotBlank(out, capacity);
}

void UiModel::formatScoreLine(const DepartmentSummary& item, char* out,
                              std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  const unsigned value = clampedScore(item.score, item.hasScore);
  switch (item.scoreState) {
    case ScoreState::Final:
      if (item.hasScore) {
        std::snprintf(out, capacity, "SCORE %u/100", value);
      } else {
        std::snprintf(out, capacity, "SCORE UNAVAILABLE");
      }
      break;
    case ScoreState::Provisional:
      if (item.hasScore) {
        std::snprintf(out, capacity, "PROV ~%u/100", value);
      } else {
        std::snprintf(out, capacity, "SCORE PROVISIONAL");
      }
      break;
    case ScoreState::Unavailable:
    default:
      std::snprintf(out, capacity, "SCORE UNAVAILABLE");
      break;
  }
}

void UiModel::formatStatusLine(const DepartmentSummary& item, bool stale,
                               char* out, std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  const char* icon = "[?]";
  if (item.scoreState == ScoreState::Final) icon = "[i]";
  if (item.scoreState == ScoreState::Provisional) icon = "[~]";

  char label[kStatusCapacity];
  copyTruncatedEllipsis(label, sizeof(label), item.statusLabel);
  ensureNotBlank(label, sizeof(label));

  if (stale) {
    std::snprintf(out, capacity, "! %s %s", icon, label);
  } else {
    std::snprintf(out, capacity, "%s %s", icon, label);
  }
}

void UiModel::formatClock(std::uint32_t epoch, std::int32_t tzOffsetSeconds,
                          char* out, std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  if (epoch == 0) {
    std::snprintf(out, capacity, "--:--");
    return;
  }
  std::int64_t adjusted = static_cast<std::int64_t>(epoch) +
                         static_cast<std::int64_t>(tzOffsetSeconds);
  adjusted %= 86400;
  if (adjusted < 0) adjusted += 86400;
  const unsigned totalMinutes = static_cast<unsigned>(adjusted / 60);
  std::snprintf(out, capacity, "%02u:%02u", totalMinutes / 60u,
                totalMinutes % 60u);
}

void UiModel::formatAge(std::uint32_t epoch, std::uint32_t nowEpoch,
                        char* out, std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  if (epoch == 0) {
    std::snprintf(out, capacity, "--");
    return;
  }
  if (nowEpoch <= epoch) {
    std::snprintf(out, capacity, "now");
    return;
  }
  const std::uint32_t delta = nowEpoch - epoch;
  if (delta < 60u) {
    std::snprintf(out, capacity, "now");
  } else if (delta < 3600u) {
    std::snprintf(out, capacity, "%lum", static_cast<unsigned long>(delta / 60u));
  } else if (delta < 86400u) {
    std::snprintf(out, capacity, "%luh", static_cast<unsigned long>(delta / 3600u));
  } else {
    std::snprintf(out, capacity, "%lud", static_cast<unsigned long>(delta / 86400u));
  }
}

void UiModel::formatUpdatedLine(const DepartmentSummary& item,
                                std::uint32_t nowEpoch,
                                std::int32_t tzOffsetSeconds, bool stale,
                                char* out, std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  char clock[8];
  char age[8];
  formatClock(item.lastUpdateEpoch, tzOffsetSeconds, clock, sizeof(clock));
  formatAge(item.lastUpdateEpoch, nowEpoch, age, sizeof(age));
  if (stale) {
    std::snprintf(out, capacity, "! STALE %s (%s)", clock, age);
  } else {
    std::snprintf(out, capacity, "UPD %s (%s)", clock, age);
  }
}

void UiModel::formatTopLine(const DepartmentSummary& item,
                            bool recognitionEnabled, char* out,
                            std::size_t capacity) {
  if (capacity == 0 || out == nullptr) return;
  if (!recognitionEnabled) {
    std::snprintf(out, capacity, "TOP: policy off");
    return;
  }
  if (!item.top.eligible || item.top.displayName[0] == '\0') {
    std::snprintf(out, capacity, "TOP: none this period");
    return;
  }

  // Only the approved alias and score are ever present in this struct.
  char alias[13];
  copyTruncatedEllipsis(alias, sizeof(alias), item.top.displayName);
  const unsigned value = clampedScore(item.top.score, true);
  std::snprintf(out, capacity, "TOP: %s %u", alias, value);
}

// ---------------------------------------------------------------------------
// Content signature for flicker-free redraws (EN-ESP-003)
// ---------------------------------------------------------------------------

std::uint32_t UiModel::signature(std::uint32_t nowEpoch,
                                 std::int32_t tzOffsetSeconds) const {
  std::uint32_t hash = 2166136261u; // FNV-1a
  auto mixByte = [&hash](std::uint8_t byte) {
    hash ^= static_cast<std::uint32_t>(byte);
    hash *= 16777619u;
  };
  auto mixU32 = [&mixByte](std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
      mixByte(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  };
  auto mixStr = [&mixByte](const char* text) {
    for (const char* p = text; *p != '\0'; ++p)
      mixByte(static_cast<std::uint8_t>(*p));
    mixByte(0);
  };

  mixByte(static_cast<std::uint8_t>(page_));
  mixU32(static_cast<std::uint32_t>(selectedIndex_));
  mixU32(static_cast<std::uint32_t>(windowStart_));
  mixU32(static_cast<std::uint32_t>(count_));
  mixByte(displayOn_ ? 1u : 0u);
  mixByte(stale_ ? 1u : 0u);
  mixByte(recognitionEnabled_ ? 1u : 0u);
  mixU32(generatedAt_);
  mixU32(static_cast<std::uint32_t>(tzOffsetSeconds));

  if (page_ == UiPage::Detail) {
    // Minute granularity: the last-update age changes at most once per minute.
    mixU32(nowEpoch / 60u);
  }

  for (std::size_t i = 0; i < count_; ++i) {
    const DepartmentSummary& item = items_[i];
    mixStr(item.name);
    mixStr(item.statusLabel);
    mixU32(item.lastUpdateEpoch);
    mixByte(static_cast<std::uint8_t>(item.scoreState));
    mixByte(item.hasScore ? 1u : 0u);
    mixByte(item.score);
    mixStr(item.top.displayName);
    mixByte(item.top.eligible ? 1u : 0u);
    mixByte(item.top.score);
  }

  return hash;
}

} // namespace energia
