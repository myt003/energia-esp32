// Host-only tests for src/energia_ui_model.{h,cpp}.
// Run: make -C tests test

#include "../src/energia_ui_model.h"
#include "stubs/energia_test_stubs.h"

#include <cstring>

using namespace energia;

namespace {

DepartmentSummary makeItem(const char* name, ScoreState state,
                           std::uint8_t score, bool hasScore) {
  DepartmentSummary item;
  UiModel::copyTruncated(item.name, sizeof(item.name), name);
  item.scoreState = state;
  item.score = score;
  item.hasScore = hasScore;
  item.lastUpdateEpoch = 1000;
  return item;
}

void testSafeTruncation() {
  char out[8];

  CHECK_EQ(4u, UiModel::copyTruncated(out, sizeof(out), "abcd"));
  CHECK_STREQ("abcd", out);

  CHECK_EQ(7u, UiModel::copyTruncated(out, sizeof(out), "0123456789"));
  CHECK_STREQ("0123456", out);

  CHECK_EQ(7u, UiModel::copyTruncatedEllipsis(out, sizeof(out), "0123456789"));
  CHECK_STREQ("012345~", out);

  CHECK_EQ(0u, UiModel::copyTruncated(nullptr, 0, "abc"));
  CHECK_EQ(0u, UiModel::copyTruncated(out, 0, "abc"));
  CHECK_EQ(0u, UiModel::copyTruncated(out, sizeof(out), nullptr));
  CHECK_STREQ("", out);

  char one[1] = {'x'};
  CHECK_EQ(0u, UiModel::copyTruncated(one, sizeof(one), "abc"));
  CHECK_STREQ("", one);
}

void testDebouncedButton() {
  teststubs::FakeClock clock;
  DebouncedButton button(40, 1200);

  CHECK_EQ(PressEvent::None, button.update(true, clock.millis()));       // 0
  CHECK_EQ(PressEvent::None, button.update(true, 39));                  // bounce window
  CHECK_EQ(PressEvent::None, button.update(true, 40));                  // press accepted
  CHECK_EQ(PressEvent::None, button.update(true, 1239));                // 1199ms held
  CHECK_EQ(PressEvent::Long, button.update(true, 1240));               // long fires once
  CHECK_EQ(PressEvent::None, button.update(true, 2000));
  CHECK_EQ(PressEvent::None, button.update(false, 2010));              // release debounce
  CHECK_EQ(PressEvent::None, button.update(false, 2050));              // no short after long
  CHECK_FALSE(button.isPressed());

  DebouncedButton shortButton(40, 1200);
  CHECK_EQ(PressEvent::None, shortButton.update(true, 100));
  CHECK_EQ(PressEvent::None, shortButton.update(true, 140));
  CHECK_EQ(PressEvent::None, shortButton.update(false, 150));
  CHECK_EQ(PressEvent::Short, shortButton.update(false, 190));
  CHECK_EQ(PressEvent::None, shortButton.update(false, 500));

  DebouncedButton bouncy(40, 1200);
  CHECK_EQ(PressEvent::None, bouncy.update(true, 0));
  CHECK_EQ(PressEvent::None, bouncy.update(true, 40));
  CHECK_EQ(PressEvent::None, bouncy.update(false, 50));
  CHECK_EQ(PressEvent::None, bouncy.update(true, 60));
  CHECK_EQ(PressEvent::None, bouncy.update(true, 100));
  CHECK_EQ(PressEvent::None, bouncy.update(false, 160));
  CHECK_EQ(PressEvent::Short, bouncy.update(false, 200));

  DebouncedButton resetButton(40, 1200);
  resetButton.update(true, 0);
  resetButton.update(true, 40);
  resetButton.reset(false, 100);
  CHECK_FALSE(resetButton.isPressed());
  CHECK_EQ(PressEvent::None, resetButton.update(false, 200));
}

void testStateMachine() {
  UiModel model;
  CHECK_EQ(0u, model.count());
  CHECK_EQ(UiPage::List, model.page());
  CHECK_TRUE(model.displayOn());
  CHECK_TRUE(model.stale()); // EN-ESP-005: never assume data is fresh on boot.
  CHECK_EQ(UiEvent::None, model.handleButton(Button::Nav, PressKind::Short));

  DepartmentSummary items[6];
  for (std::size_t i = 0; i < 6; ++i)
    items[i] = makeItem("Dept", ScoreState::Final, static_cast<std::uint8_t>(i), true);

  CHECK_TRUE(model.setDepartments(items, 6));
  CHECK_EQ(6u, model.count());
  CHECK_STREQ("Dept", model.selected()->name); // copied, not aliased
  items[0].name[0] = 'X';
  CHECK_STREQ("Dept", model.selected()->name);

  // EN-ESP-002: short press advances, window scrolls with the selection.
  for (std::size_t i = 1; i < 6; ++i) {
    CHECK_EQ(UiEvent::SelectionChanged,
             model.handleButton(Button::Nav, PressKind::Short));
    CHECK_EQ(i, model.selectedIndex());
    const std::size_t expectedStart = i < 4 ? 0 : i - 3;
    CHECK_EQ(expectedStart, model.windowStart());
  }
  CHECK_EQ(UiEvent::SelectionChanged,
           model.handleButton(Button::Nav, PressKind::Short));
  CHECK_EQ(0u, model.selectedIndex()); // wraps
  CHECK_EQ(0u, model.windowStart());

  // EN-ESP-002: long press selects; short press is ignored on detail; long returns.
  CHECK_EQ(UiEvent::DetailOpened, model.handleButton(Button::Nav, PressKind::Long));
  CHECK_EQ(UiPage::Detail, model.page());
  CHECK_EQ(UiEvent::None, model.handleButton(Button::Nav, PressKind::Short));
  CHECK_EQ(0u, model.selectedIndex()); // short presses are ignored on detail
  CHECK_EQ(UiEvent::BackToList, model.handleButton(Button::Nav, PressKind::Long));
  CHECK_EQ(UiPage::List, model.page());

  // EN-ESP-005: power short toggles the display, long only requests sleep.
  CHECK_EQ(UiEvent::DisplayToggled,
           model.handleButton(Button::Power, PressKind::Short));
  CHECK_FALSE(model.displayOn());
  CHECK_EQ(UiEvent::DisplayToggled,
           model.handleButton(Button::Power, PressKind::Short));
  CHECK_TRUE(model.displayOn());
  CHECK_EQ(UiEvent::SleepRequested,
           model.handleButton(Button::Power, PressKind::Long));
  CHECK_TRUE(model.displayOn());
  CHECK_EQ(UiPage::List, model.page());

  // Bounds: overlong lists are rejected without changing state.
  DepartmentSummary tooMany[kMaxDepartments + 1];
  CHECK_FALSE(model.setDepartments(tooMany, kMaxDepartments + 1));
  CHECK_EQ(6u, model.count());
  CHECK_FALSE(model.setDepartments(nullptr, 1));
  CHECK_TRUE(model.setDepartments(nullptr, 0));
  CHECK_EQ(0u, model.count());
  CHECK_EQ(0u, model.selectedIndex());
}

void testFormatters() {
  char line[32];

  DepartmentSummary final = makeItem("Fabrication", ScoreState::Final, 87, true);
  UiModel::formatScoreLine(final, line, sizeof(line));
  CHECK_STREQ("SCORE 87/100", line);

  std::strcpy(final.statusLabel, "On track");
  UiModel::formatStatusLine(final, false, line, sizeof(line));
  CHECK_STREQ("[i] On track", line);
  UiModel::formatStatusLine(final, true, line, sizeof(line));
  CHECK_STREQ("! [i] On track", line);

  final.top.eligible = true;
  std::strcpy(final.top.displayName, "A. Silva");
  final.top.score = 95;
  UiModel::formatTopLine(final, true, line, sizeof(line));
  CHECK_STREQ("TOP: A. Silva 95", line);
  UiModel::formatTopLine(final, false, line, sizeof(line));
  CHECK_STREQ("TOP: policy off", line);
  final.top.eligible = false;
  UiModel::formatTopLine(final, true, line, sizeof(line));
  CHECK_STREQ("TOP: none this period", line);

  std::strcpy(final.top.displayName, "Abigail Longname");
  final.top.eligible = true;
  final.top.score = 99;
  UiModel::formatTopLine(final, true, line, sizeof(line));
  CHECK_STREQ("TOP: Abigail Lon~ 99", line);

  DepartmentSummary provisional =
      makeItem("Lab", ScoreState::Provisional, 74, true);
  UiModel::formatScoreLine(provisional, line, sizeof(line));
  CHECK_STREQ("PROV ~74/100", line);
  std::strcpy(provisional.statusLabel, "Pending");
  UiModel::formatStatusLine(provisional, false, line, sizeof(line));
  CHECK_STREQ("[~] Pending", line);

  DepartmentSummary provisionalNoScore =
      makeItem("Lab", ScoreState::Provisional, 0, false);
  UiModel::formatScoreLine(provisionalNoScore, line, sizeof(line));
  CHECK_STREQ("SCORE PROVISIONAL", line);

  DepartmentSummary unavailable =
      makeItem("Warehouse", ScoreState::Unavailable, 0, false);
  UiModel::formatScoreLine(unavailable, line, sizeof(line));
  CHECK_STREQ("SCORE UNAVAILABLE", line);
  std::strcpy(unavailable.statusLabel, "No data");
  UiModel::formatStatusLine(unavailable, false, line, sizeof(line));
  CHECK_STREQ("[?] No data", line);

  // EN-ESP-003: server values only; out-of-range values are clamped, never recomputed.
  DepartmentSummary bogus = makeItem("Bad", ScoreState::Final, 200, true);
  UiModel::formatScoreLine(bogus, line, sizeof(line));
  CHECK_STREQ("SCORE 100/100", line);

  // Last update formatting: epoch 1000 == 00:16 UTC; 3m later -> 00:19.
  UiModel::formatClock(1000, 0, line, sizeof(line));
  CHECK_STREQ("00:16", line);
  UiModel::formatClock(1000, 3600, line, sizeof(line));
  CHECK_STREQ("01:16", line);
  UiModel::formatClock(1000, -3600, line, sizeof(line));
  CHECK_STREQ("23:16", line);
  UiModel::formatClock(0, 0, line, sizeof(line));
  CHECK_STREQ("--:--", line);

  UiModel::formatAge(1000, 1180, line, sizeof(line));
  CHECK_STREQ("3m", line);
  UiModel::formatAge(1000, 4700, line, sizeof(line));
  CHECK_STREQ("1h", line);
  UiModel::formatAge(1000, 90000, line, sizeof(line));
  CHECK_STREQ("1d", line);

  UiModel::formatUpdatedLine(final, 1180, 0, false, line, sizeof(line));
  CHECK_STREQ("UPD 00:16 (3m)", line);
  UiModel::formatUpdatedLine(final, 1180, 0, true, line, sizeof(line));
  CHECK_STREQ("! STALE 00:16 (3m)", line);

  DepartmentSummary longName =
      makeItem("Very Long Department Name", ScoreState::Final, 50, true);
  char row[kListRowChars];
  UiModel::formatListRow(longName, row, sizeof(row));
  CHECK_EQ(kListRowChars - 1, std::strlen(row));
  CHECK_TRUE(std::strchr(row, '~') != nullptr);
}

void testSignature() {
  UiModel model;
  DepartmentSummary items[2] = {
      makeItem("A", ScoreState::Final, 10, true),
      makeItem("B", ScoreState::Final, 20, true)};
  CHECK_TRUE(model.setDepartments(items, 2));
  model.setStale(false); // boot default is stale; make the transition observable.

  const std::uint32_t base = model.signature(1000, 0);
  CHECK_EQ(base, model.signature(1000, 0));
  model.handleButton(Button::Nav, PressKind::Short);
  CHECK_TRUE(base != model.signature(1000, 0));

  const std::uint32_t afterNav = model.signature(1000, 0);
  model.handleButton(Button::Nav, PressKind::Long); // detail page
  CHECK_TRUE(afterNav != model.signature(1000, 0));
  const std::uint32_t detailSig = model.signature(1000, 0);
  CHECK_TRUE(detailSig != model.signature(1660, 0)); // minute tick on detail
  model.setStale(true);
  CHECK_TRUE(detailSig != model.signature(1000, 0));
  model.setDisplayOn(false);
  CHECK_TRUE(detailSig != model.signature(1000, 0));
}

} // namespace

int main() {
  testSafeTruncation();
  testDebouncedButton();
  testStateMachine();
  testFormatters();
  testSignature();

  std::printf("energia_ui_model tests: %d checks, %d failures\n",
              teststubs::checkCount(), teststubs::failureCount());
  return teststubs::failureCount() == 0 ? 0 : 1;
}
