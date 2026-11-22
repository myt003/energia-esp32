// Minimal host-side stubs/helpers for the pure C++ model tests.
// No ESP32, Arduino, WiFi or display headers are pulled in here.

#ifndef ENERGIA_TEST_STUBS_H_
#define ENERGIA_TEST_STUBS_H_

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

namespace teststubs {

inline int& checkCount() {
  static int count = 0;
  return count;
}

inline int& failureCount() {
  static int count = 0;
  return count;
}

inline void recordFailure(const char* file, int line, const char* expression) {
  ++failureCount();
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expression);
}

// Fake monotonic clock. The model takes explicit timestamps, but this stub
// mirrors the millis() source used by src/main.cpp on the device.
class FakeClock {
 public:
  explicit FakeClock(std::uint32_t startMs = 0) : nowMs_(startMs) {}
  std::uint32_t millis() const { return nowMs_; }
  void advance(std::uint32_t deltaMs) { nowMs_ += deltaMs; }
  void set(std::uint32_t nowMs) { nowMs_ = nowMs; }
 private:
  std::uint32_t nowMs_;
};

} // namespace teststubs

#define CHECK_TRUE(expr)                                                     \
  do {                                                                       \
    ++teststubs::checkCount();                                                \
    if (!(expr)) teststubs::recordFailure(__FILE__, __LINE__, #expr);         \
  } while (0)

#define CHECK_FALSE(expr) CHECK_TRUE(!(expr))

#define CHECK_EQ(expected, actual)                                           \
  do {                                                                       \
    ++teststubs::checkCount();                                                \
    if (!((expected) == (actual)))                                           \
      teststubs::recordFailure(__FILE__, __LINE__, #expected " == " #actual); \
  } while (0)

#define CHECK_STREQ(expected, actual)                                        \
  do {                                                                       \
    ++teststubs::checkCount();                                                \
    if (std::strcmp((expected), (actual)) != 0)                              \
      teststubs::recordFailure(__FILE__, __LINE__,                           \
                               #expected " streq " #actual);                 \
  } while (0)

#endif // ENERGIA_TEST_STUBS_H_
