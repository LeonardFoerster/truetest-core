// footprint.md §2.3: retained camera (pan/zoom/fit/detach/go-live). Pure
// logic, no ImGui - see src/ui/desk/footprint_camera.h.

#include <gtest/gtest.h>

#include "ui/desk/footprint_camera.h"

using truetest::ui::desk::FootprintCamera;
using truetest::ui::desk::FootprintCameraState;

TEST(FootprintCamera, StartsUninitializedAndFollowing)
{
    FootprintCamera cam;
    EXPECT_FALSE(cam.initialized());
    EXPECT_EQ(cam.state(), FootprintCameraState::following);
    EXPECT_EQ(cam.unseen_bars(), 0);
}

TEST(FootprintCamera, FitSetsExactDataBoundsAndFollows)
{
    FootprintCamera cam;
    cam.fit(/*latest_bar_index=*/10, 1000, 5000, 90.0, 110.0);
    EXPECT_TRUE(cam.initialized());
    EXPECT_EQ(cam.state(), FootprintCameraState::following);
    EXPECT_EQ(cam.time_min_ms(), 1000);
    EXPECT_EQ(cam.time_max_ms(), 5000);
    EXPECT_DOUBLE_EQ(cam.price_min(), 90.0);
    EXPECT_DOUBLE_EQ(cam.price_max(), 110.0);
    EXPECT_EQ(cam.unseen_bars(), 0);
}

TEST(FootprintCamera, UpdateLatestAutoInitializesOnFirstCall)
{
    FootprintCamera cam;
    cam.update_latest(3, 0, 4000, 10.0, 20.0);
    EXPECT_TRUE(cam.initialized());
    EXPECT_EQ(cam.time_min_ms(), 0);
    EXPECT_EQ(cam.time_max_ms(), 4000);
}

TEST(FootprintCamera, FollowingPreservesSpanAndSlidesWithNewData)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 10.0); // span = 1000ms
    cam.update_latest(1, 0, 2000, 0.0, 10.0); // new data extends to 2000
    // Span preserved (1000ms), window slides so max == new data max.
    EXPECT_EQ(cam.time_max_ms(), 2000);
    EXPECT_EQ(cam.time_min_ms(), 1000);
    EXPECT_EQ(cam.unseen_bars(), 0); // still following - nothing is "unseen"
}

TEST(FootprintCamera, FollowingAutoFitsPriceEachFrame)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 10.0);
    cam.update_latest(0, 0, 1000, 5.0, 25.0); // price range widened
    EXPECT_DOUBLE_EQ(cam.price_min(), 5.0);
    EXPECT_DOUBLE_EQ(cam.price_max(), 25.0);
}

TEST(FootprintCamera, ManualPanDetachesAndMovesWindow)
{
    FootprintCamera cam;
    cam.fit(5, 0, 1000, 0.0, 10.0); // span_t=1000, span_p=10
    cam.pan(/*time_frac=*/0.1, /*price_frac=*/-0.2);
    EXPECT_EQ(cam.state(), FootprintCameraState::detached);
    EXPECT_EQ(cam.time_min_ms(), 100);
    EXPECT_EQ(cam.time_max_ms(), 1100);
    EXPECT_DOUBLE_EQ(cam.price_min(), -2.0);
    EXPECT_DOUBLE_EQ(cam.price_max(), 8.0);
}

TEST(FootprintCamera, DetachingSnapshotsSeenBarIndexSoLaterArrivalsCountAsUnseen)
{
    FootprintCamera cam;
    cam.fit(5, 0, 1000, 0.0, 10.0); // latest=5 fully seen at detach time
    cam.pan(0.0, 0.0); // detach
    EXPECT_EQ(cam.unseen_bars(), 0); // nothing new has arrived yet

    cam.update_latest(8, 0, 1000, 0.0, 10.0); // 3 more bars closed while detached
    EXPECT_EQ(cam.state(), FootprintCameraState::detached); // never auto-resumes
    EXPECT_EQ(cam.unseen_bars(), 3);

    cam.update_latest(9, 0, 1000, 0.0, 10.0); // one more
    EXPECT_EQ(cam.unseen_bars(), 4);
}

TEST(FootprintCamera, DetachedViewportNeverMovesOnItsOwn)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 10.0);
    cam.pan(0.5, 0.0); // detach, window now [500,1500]
    const auto min_before = cam.time_min_ms();
    const auto max_before = cam.time_max_ms();

    // New data arrives repeatedly while detached - the plan is explicit:
    // "never snap back automatically".
    for (int i = 0; i < 5; ++i)
        cam.update_latest(i + 1, 0, 1000 + (i + 1) * 500, 0.0, 10.0);

    EXPECT_EQ(cam.time_min_ms(), min_before);
    EXPECT_EQ(cam.time_max_ms(), max_before);
}

TEST(FootprintCamera, GoLiveResumesFollowingAtCurrentZoomSpanNotFullFit)
{
    FootprintCamera cam;
    cam.fit(0, 0, 10000, 0.0, 10.0); // full span 10000
    cam.zoom_time(5.0, 0.5); // detach, span now 2000 (10000/5)
    ASSERT_EQ(cam.state(), FootprintCameraState::detached);
    const auto zoomed_span = cam.time_max_ms() - cam.time_min_ms();
    EXPECT_EQ(zoomed_span, 2000);

    cam.go_live(3);
    EXPECT_EQ(cam.state(), FootprintCameraState::following);
    EXPECT_EQ(cam.unseen_bars(), 0);

    // Next frame's update_latest must preserve the zoomed span, NOT reset
    // to the full data range - go_live is not fit().
    cam.update_latest(4, 0, 20000, 0.0, 10.0);
    EXPECT_EQ(cam.time_max_ms() - cam.time_min_ms(), zoomed_span);
    EXPECT_EQ(cam.time_max_ms(), 20000);
}

TEST(FootprintCamera, ZoomTimeKeepsAnchorFixed)
{
    FootprintCamera cam;
    // Span well above kMinTimeSpanMs (1000ms) so the 2x zoom-in below isn't
    // clamped by the minimum-span floor.
    cam.fit(0, 0, 100'000, 0.0, 10.0);
    // Anchor at 0.25 across [0,100000] -> anchor_time = 25000.
    cam.zoom_time(2.0, 0.25); // span halves to 50000
    EXPECT_EQ(cam.time_max_ms() - cam.time_min_ms(), 50'000);
    // anchor_time (25000) must remain at the same 0.25 fraction of the new window.
    const double frac = static_cast<double>(25'000 - cam.time_min_ms())
        / static_cast<double>(cam.time_max_ms() - cam.time_min_ms());
    EXPECT_NEAR(frac, 0.25, 1e-9);
}

TEST(FootprintCamera, ZoomPriceKeepsAnchorFixed)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 100.0);
    cam.zoom_price(4.0, 0.5); // span 100 -> 25, anchor at midpoint (50)
    EXPECT_DOUBLE_EQ(cam.price_max() - cam.price_min(), 25.0);
    EXPECT_DOUBLE_EQ(cam.price_min(), 37.5);
    EXPECT_DOUBLE_EQ(cam.price_max(), 62.5);
}

TEST(FootprintCamera, ZoomTimeCannotCollapseBelowMinimumSpan)
{
    FootprintCamera cam;
    // Start well above the floor so the clamp path is actually exercised
    // (starting AT the floor would trivially "pass" without proving anything).
    cam.fit(0, 0, 10'000'000, 0.0, 10.0);
    cam.zoom_time(1'000'000.0, 0.5); // absurd zoom-in factor
    EXPECT_EQ(cam.time_max_ms() - cam.time_min_ms(), 1'000); // clamped to kMinTimeSpanMs
}

TEST(FootprintCamera, ZoomOutWidensSpan)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 10.0);
    cam.zoom_time(0.5, 0.5); // zoom out -> span doubles
    EXPECT_EQ(cam.time_max_ms() - cam.time_min_ms(), 2000);
}

TEST(FootprintCamera, JumpToStartPreservesSpanAndDetaches)
{
    FootprintCamera cam;
    cam.fit(0, 1000, 2000, 0.0, 10.0); // span 1000
    cam.jump_to_start(0);
    EXPECT_EQ(cam.state(), FootprintCameraState::detached);
    EXPECT_EQ(cam.time_min_ms(), 0);
    EXPECT_EQ(cam.time_max_ms(), 1000);
}

TEST(FootprintCamera, NonPositiveZoomFactorIsIgnored)
{
    FootprintCamera cam;
    cam.fit(0, 0, 1000, 0.0, 10.0);
    cam.zoom_time(0.0, 0.5);
    cam.zoom_time(-2.0, 0.5);
    // Still following - a rejected zoom call must not spuriously detach.
    EXPECT_EQ(cam.state(), FootprintCameraState::following);
    EXPECT_EQ(cam.time_max_ms() - cam.time_min_ms(), 1000);
}
