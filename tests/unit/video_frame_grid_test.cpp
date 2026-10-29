#include <catch2/catch_test_macros.hpp>

#include "video/video_frame_grid.h"

using namespace rebelliocap;

TEST_CASE("video frame grid removes sub-frame acquisition jitter") {
  const VideoFrameGrid grid(123'456, 60, 10'000'000);
  for (std::uint64_t index = 0; index < 600; ++index) {
    const auto scheduled = grid.at(index);
    const auto acquired = scheduled + static_cast<QpcTicks>(index % 8) * 10'000;
    CHECK(grid.at_or_before(acquired) == scheduled);
  }
}

TEST_CASE("video frame grid retains elapsed time after startup waits and recovery") {
  const VideoFrameGrid grid(1'000'000, 60, 10'000'000);
  for (const auto elapsed : {999'999LL, 1'000'000LL, 10'000'001LL, 600'123'456LL}) {
    const auto captured = 1'000'000 + elapsed;
    const auto presented = grid.at_or_before(captured);
    CHECK(presented <= captured);
    CHECK(captured - presented <= 166'666);
    CHECK(presented >= grid.at(5));
  }
}

TEST_CASE("video frame grid inverts fractional frame boundaries exactly") {
  for (const auto fps : {30U, 60U, 120U, 144U}) {
    const VideoFrameGrid grid(975'321, fps, 10'000'003);
    for (std::uint64_t index = 1; index < 10'000; ++index) {
      const auto boundary = grid.at(index);
      REQUIRE(grid.index_at(boundary) == index);
      REQUIRE(grid.index_at(boundary - 1) == index - 1);
      REQUIRE(grid.index_at(boundary + 1) == index);
    }
  }
}

TEST_CASE("video frame grid does not accumulate rounding error over six hours") {
  constexpr QpcTicks origin = 321'000;
  constexpr QpcTicks frequency = 10'000'003;
  for (const auto fps : {30U, 60U, 120U, 144U}) {
    const VideoFrameGrid grid(origin, fps, frequency);
    for (std::uint64_t second = 0; second <= 6 * 60 * 60; ++second) {
      REQUIRE(grid.at(second * fps) == origin + static_cast<QpcTicks>(second) * frequency);
    }
  }
}

TEST_CASE("video frame grid does not invent slots before its QPC origin") {
  const VideoFrameGrid grid(5'000, 60, 60'000);
  CHECK(grid.index_at(0) == 0);
  CHECK(grid.index_at(4'999) == 0);
  CHECK(grid.at_or_before(5'000) == 5'000);
  CHECK(grid.at_or_before(5'100) == grid.at_or_before(5'900));
}
TEST_CASE("acquisition may finish within the following slot without rewriting presentation time") {
  rebelliocap::VideoFrameGrid grid(100'000,60,10'000'000);
  CHECK(grid.after_acquisition(grid.at(3),grid.at(3)+2'000)==grid.at(3));
  CHECK(grid.after_acquisition(grid.at(3),grid.at(4)+2'000)==grid.at(3));
  CHECK(grid.after_acquisition(grid.at(3),grid.at(5)+2'000)==grid.at(5));
}
