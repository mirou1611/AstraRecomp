#include "ps2vita/gif.hpp"
#include "ps2vita/gs.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Diagnostic contract tests for Astra's logical linear VRAM, not native GS
// swizzled memory or a hardware/reference-renderer rasterization golden.
namespace {

unsigned checks = 0, failures = 0;
constexpr std::uint32_t kVramSize = 0x400000u, kVramMask = kVramSize - 1u;
using Events = std::vector<ps2vita::GsPixelWrite>;

void check(bool condition, const std::string& label) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

std::uint64_t frame(unsigned base, unsigned width = 10u, unsigned format = 0u,
                    std::uint32_t mask = 0u) {
  return base | (std::uint64_t{width} << 16u) |
         (std::uint64_t{format} << 24u) | (std::uint64_t{mask} << 32u);
}

std::uint32_t address(unsigned base, unsigned width, unsigned native_x,
                      unsigned native_y) {
  return (base * 8192u + (native_y * width + native_x) * 4u) & kVramMask;
}

void store32(std::vector<std::uint8_t>& memory, std::uint32_t at,
             std::uint32_t value) {
  for (unsigned byte = 0; byte < 4u; ++byte)
    memory[(at + byte) & kVramMask] =
        static_cast<std::uint8_t>(value >> (byte * 8u));
}

std::uint32_t load32(const std::vector<std::uint8_t>& memory, std::uint32_t at) {
  std::uint32_t value = 0;
  for (unsigned byte = 0; byte < 4u; ++byte)
    value |= std::uint32_t{memory[(at + byte) & kVramMask]} << (byte * 8u);
  return value;
}

ps2vita::Gs::PixelObserver observer(Events& events) {
  return [&events](const ps2vita::GsPixelWrite& event) { events.push_back(event); };
}

struct Packet {
  std::vector<std::uint8_t> bytes;
  void word(std::uint64_t value) {
    for (unsigned byte = 0; byte < 8u; ++byte)
      bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8u)));
  }
  void ad(std::uint64_t value, unsigned reg) {
    word(0x1000000000008001ull); word(0xEull);
    word(value); word(reg);
  }
  void image(std::uint32_t base, unsigned format, unsigned u, unsigned v,
             std::uint32_t texel) {
    ad((std::uint64_t{base / 256u} << 32u) | (1ull << 48u) |
       (std::uint64_t{format} << 56u), 0x50u);
    ad((std::uint64_t{u} << 32u) | (std::uint64_t{v} << 48u), 0x51u);
    ad(1ull | (1ull << 32u), 0x52u); ad(0u, 0x53u);
    word(0x0800000000008001ull); word(0u);
    word(texel); word(0u); // One 16-byte IMAGE payload; transfer is one texel.
  }
  bool submit(ps2vita::Gif& gif) const {
    return gif.submit(bytes.data(), bytes.size());
  }
};

std::uint64_t xyz(unsigned host_x, unsigned host_y, std::uint32_t z = 7u) {
  // Guest coordinates are 12.4 fixed point; each host sample is 4 native pixels.
  return host_x * 64u | (std::uint64_t{host_y * 64u} << 16u) |
         (std::uint64_t{z} << 32u);
}

std::uint64_t uv(unsigned u, unsigned v) {
  return u * 16u | (std::uint64_t{v * 16u} << 16u);
}

void test_configuration() {
  Events events;
  std::vector<std::uint8_t> memory(kVramSize);
  ps2vita::Gs gs;
  check(!gs.pixel_watch_enabled(), "watch starts disabled");
  check(!gs.set_pixel_watch({0u}), "nonempty watch requires an observer");
  check(!gs.set_pixel_watch({1u}, observer(events)), "unaligned watch rejected");
  check(!gs.set_pixel_watch({kVramSize}, observer(events)), "VRAM-end watch rejected");
  check(!gs.set_pixel_watch({0u, 0u}, observer(events)), "duplicate watches rejected");
  std::vector<std::uint32_t> addresses;
  for (unsigned i = 0; i < 17u; ++i) addresses.push_back(i * 4u);
  check(!gs.set_pixel_watch(addresses, observer(events)), "seventeen watches rejected");
  check(!gs.pixel_watch_enabled(), "invalid setup does not enable a watch");
  addresses.pop_back();
  check(gs.set_pixel_watch(addresses, observer(events)) && gs.pixel_watch_enabled(),
        "sixteen distinct aligned watches accepted");
  check(!gs.set_pixel_watch({3u}, observer(events)) && gs.pixel_watch_enabled(),
        "invalid replacement retains the existing watch");
  gs.set_color_target(&memory, frame(0u, 1u));
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.point({0, 0, 1u, 0x12345678u});
  check(events.size() == 4u, "failed replacement retained original tile watches");
  events.clear();
  check(gs.set_pixel_watch({}, observer(events)) && !gs.pixel_watch_enabled(),
        "empty address list disables even with an observer");
  gs.point({0, 0, 1u, 0x87654321u});
  check(events.empty(), "disabled observer is not called");
  check(gs.set_pixel_watch({kVramSize - 4u}, observer(events)),
        "last aligned word in VRAM is a valid watch");
  check(gs.set_pixel_watch({}) && !gs.pixel_watch_enabled(),
        "empty default setup disables");
}

void test_alias_and_wrapped_tiles() {
  Events events;
  std::vector<std::uint8_t> memory(kVramSize);
  ps2vita::Gs gs;
  const auto watched = address(0u, 640u, 80u * 4u + 3u, 65u * 4u + 2u);
  check(watched == address(0x50u, 640u, 80u * 4u + 3u, 1u * 4u + 2u),
        "FRAME 0 row 65 aliases FRAME 0x50 row 1 in the linear model");
  store32(memory, watched, 0x10203040u);
  check(gs.set_pixel_watch({watched}, observer(events)), "physical alias watch accepted");
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_color_target(&memory, frame(0u));
  gs.set_draw_trace({ps2vita::GsTracePrimitive::Point, 11u, 23u, 0u, frame(0u)});
  gs.point({80, 65, 9u, 0xA1B2C3D4u});
  gs.set_color_target(&memory, frame(0x50u));
  gs.set_draw_trace({ps2vita::GsTracePrimitive::Point, 12u, 24u, 0u, frame(0x50u)});
  gs.point({80, 1, 10u, 0x55667788u});
  check(events.size() == 2u, "both aliased target draws notify the same physical watch");
  if (events.size() == 2u) {
    const auto& first = events[0];
    const auto& second = events[1];
    check(first.address == watched && first.before == 0x10203040u &&
              first.after == 0xA1B2C3D4u && first.input_color == 0xA1B2C3D4u &&
              first.z == 9u && first.x == 80 && first.y == 65,
          "alias event records the exact interior native word and host sample");
    check(second.address == watched && second.before == first.after &&
              second.after == 0x55667788u && second.x == 80 && second.y == 1 &&
              second.frame == frame(0x50u) && second.draw.frame == second.frame &&
              second.draw.sequence == 12u && second.draw.packet == 24u,
          "later alias event records the new FRAME, coordinates, and prior value");
  }
  events.clear();
  std::vector<std::uint32_t> watches;
  // FRAME 511 with a 192-pixel pitch puts dy=3 of this tile beyond 4 MiB.
  // Every dx/dy is watched, including non-origin words and wrapped addresses.
  for (unsigned dy = 0; dy < 4u; ++dy)
    for (unsigned dx = 0; dx < 4u; ++dx) {
      const auto at = address(511u, 192u, 8u + dx, 8u + dy);
      watches.push_back(at);
      store32(memory, at, 0x11000000u + dy * 4u + dx);
    }
  check(watches.front() > 0x3FF000u && watches.back() < 0x1000u,
        "wrap fixture crosses the end of logical VRAM inside one tile");
  check(gs.set_pixel_watch(watches, observer(events)), "all sixteen tile words accepted");
  gs.set_color_target(&memory, frame(511u, 3u));
  gs.point({2, 2, 0xDEADBEEFu, 0xCAFEBABEu});
  check(events.size() == 16u, "one tile produces one event per matching physical watch");
  for (std::size_t i = 0; i < events.size(); ++i)
    check(events[i].address == watches[i] && events[i].before == 0x11000000u + i &&
              events[i].after == 0xCAFEBABEu && events[i].x == 2 && events[i].y == 2 &&
              events[i].z == 0xDEADBEEFu && load32(memory, watches[i]) == events[i].after,
          "wrapped tile event has exact per-word before/after " + std::to_string(i));
  events.clear();
  gs.point({3, 2, 1u, 0u});
  check(events.empty(), "neighboring tile does not notify watched words");
}

void test_masks_rejections_and_fallback() {
  Events events;
  std::vector<std::uint8_t> memory(kVramSize);
  ps2vita::Gs gs;
  const auto watched = address(0u, 640u, 9u, 10u);
  check(gs.set_pixel_watch({watched}, observer(events)), "mask test watch accepted");
  gs.set_color_target(&memory, frame(0u));
  gs.clear(0x12345678u, 10u);
  check(events.empty() && load32(memory, watched) == 0x12345678u,
        "clear updates backing memory without raster events");
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_color_target(&memory, frame(0u, 10u, 0u, 0x00FF00FFu));
  gs.point({2, 2, 5u, 0xA1B2C3D4u});
  check(events.size() == 1u && events[0].before == 0x12345678u &&
            events[0].after == 0xA134C378u && events[0].input_color == 0xA1B2C3D4u,
        "partial FBMSK preserves watched destination bits and records unmasked input");
  events.clear();
  gs.set_color_target(&memory, frame(0u, 10u, 0u, 0xFFFFFFFFu));
  gs.point({2, 2, 5u, 0u});
  check(events.size() == 1u && events[0].before == 0xA134C378u &&
            events[0].after == events[0].before && events[0].input_color == 0u,
        "fully masked accepted operation is reported even though no bits change");
  events.clear();
  gs.set_color_target(&memory, frame(0u, 10u, 1u));
  gs.point({2, 2, 5u, 0xEE010203u});
  check(events.size() == 1u && events[0].after == 0xA1010203u,
        "PSMCT24 accepted write preserves each watched word's alpha");
  events.clear();
  gs.set_color_target(&memory, frame(0u));
  gs.set_alpha_test(1u); // NEVER, KEEP.
  gs.point({2, 2, 1u, 0x87654321u});
  gs.set_alpha_test(1u | (2u << 12u)); // NEVER, ZB_ONLY.
  gs.point({2, 2, 1u, 0x87654321u});
  check(events.empty() && load32(memory, watched) == 0xA1010203u,
        "alpha KEEP and ZB_ONLY do not report color operations");
  gs.set_alpha_test(1u | (3u << 12u)); // NEVER, RGB_ONLY.
  gs.point({2, 2, 1u, 0xEEABCDEFu});
  check(events.size() == 1u && events[0].after == 0xA1ABCDEFu &&
            events[0].input_color == 0xEEABCDEFu,
        "alpha RGB_ONLY accepted operation records preserved destination alpha");
  events.clear();
  gs.set_alpha_test(1u | (1u << 12u)); // NEVER, FB_ONLY.
  gs.point({2, 2, 1u, 0x55443322u});
  check(events.size() == 1u && events[0].after == 0x55443322u,
        "alpha FB_ONLY still reports its accepted color operation");
  events.clear();
  gs.set_alpha_test(0u);
  gs.set_depth_state(ps2vita::Gs::DepthTest::Never, true);
  gs.point({2, 2, 0u, 0u});
  gs.set_depth_state(ps2vita::Gs::DepthTest::LessEqual, false);
  gs.point({2, 2, 11u, 0u}); // Initial depth is still 10 (depth writes disabled above).
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_scissor(0, 0, 1, 1);
  gs.point({2, 2, 1u, 0u});
  gs.set_scissor(0, 0, ps2vita::Gs::kWidth - 1, ps2vita::Gs::kHeight - 1);
  gs.point({-1, 2, 1u, 0u}); gs.point({2, ps2vita::Gs::kHeight, 1u, 0u});
  check(events.empty() && load32(memory, watched) == 0x55443322u,
        "depth, scissor, and host bounds rejection do not report writes");
  // The observer is only for bound logical VRAM, never the host fallback buffer.
  std::vector<std::uint8_t> small_memory(1024u);
  const std::array<std::uint64_t, 3> invalid_frames{{frame(0u, 0u), frame(0u, 10u, 2u),
                                                 frame(0u)}};
  for (std::size_t i = 0; i < invalid_frames.size(); ++i) {
    gs.set_color_target(i == 2u ? &small_memory : &memory, invalid_frames[i]);
    gs.point({2, 2, 1u, 0x13579BDFu});
    check(events.empty() && gs.pixel(2, 2) == 0x13579BDFu,
          "unsupported binding uses host fallback without watch callbacks " +
              std::to_string(i));
  }
  gs.set_color_target(nullptr, frame(0u));
  gs.point({2, 2, 1u, 0x2468ACE0u});
  check(events.empty() && gs.pixel(2, 2) == 0x2468ACE0u,
        "null backing memory uses standalone host buffer without watch callbacks");
  gs.set_color_target(&memory, frame(0u, 1u));
  const auto narrow_watch = address(0u, 64u, 0u, 8u);
  check(gs.set_pixel_watch({narrow_watch}, observer(events)), "narrow target watch accepted");
  gs.point({16, 2, 1u, 0u}); // Outside 64 native pixels, not outside the host preview.
  check(events.empty(), "sample outside FRAME pitch cannot alias a watched next row");
}

void draw_observational_scene(ps2vita::Gs& gs, std::vector<std::uint8_t>& memory) {
  gs.set_color_target(&memory, frame(32u));
  gs.set_depth_state(ps2vita::Gs::DepthTest::Always, false);
  gs.set_draw_trace({ps2vita::GsTracePrimitive::Point, 0u, 1u, 0u, frame(32u)});
  gs.point({4, 4, 8u, 0x80402010u});
  gs.set_blend_state(true, 0x44u, false, true); // (Cs-Cd)*As/128+Cd.
  gs.point({4, 4, 7u, 0x40806040u});
  gs.set_blend_state(false, 0u, false, true);
  gs.set_draw_trace({ps2vita::GsTracePrimitive::Line, 0u, 2u, 1u, frame(32u)});
  gs.line({2, 3, 5u, 0x80112233u}, {8, 5, 3u, 0x80AABBCCu});
  gs.set_draw_trace({ps2vita::GsTracePrimitive::Triangle, 0u, 3u, 3u, frame(32u)});
  gs.triangle({3, 3, 2u, 0x80FF0000u}, {9, 3, 2u, 0x8000FF00u},
              {3, 9, 2u, 0x800000FFu});
  gs.set_color_target(&memory, frame(32u, 10u, 0u, 0x00FF00FFu));
  gs.point({4, 4, 1u, 0xA1B2C3D4u});
  gs.set_alpha_test(1u); gs.point({4, 4, 0u, 0u});
  gs.set_alpha_test(0u);
}

void test_observational_equality_and_blend() {
  std::vector<std::uint8_t> off_memory(kVramSize);
  for (std::size_t i = 0; i < off_memory.size(); ++i)
    off_memory[i] = static_cast<std::uint8_t>(i * 17u + 5u);
  auto on_memory = off_memory;
  Events events;
  ps2vita::Gs off, on;
  const auto watched = address(32u, 640u, 16u, 16u);
  check(on.set_pixel_watch({watched}, observer(events)), "observational watch accepted");
  draw_observational_scene(off, off_memory);
  draw_observational_scene(on, on_memory);
  check(off_memory == on_memory, "watch off/on leaves all four MiB of rendered VRAM identical");
  bool equal = true;
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
      equal = equal && off.pixel(x, y) == on.pixel(x, y);
  check(equal, "watch off/on leaves the complete draw preview identical");
  check(events.size() >= 2u && events[1].before == 0x80402010u &&
            events[1].input_color == 0x40806040u && events[1].after == 0x40604028u,
        "blend event records pre-blend source, old destination, and accepted result");
  events.clear();
  const auto current = load32(on_memory, watched);
  on.set_color_target(&on_memory, frame(32u));
  on.point({4, 4, 1u, current});
  check(events.size() == 1u && events[0].before == current && events[0].after == current,
        "same-color accepted operation updates provenance rather than filtering by RGB delta");
}

void test_gif_texture32_and_draw_metadata() {
  Events events;
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  const auto target_frame = frame(32u);
  const auto watched = address(32u, 640u, 5u * 4u + 1u, 5u * 4u + 2u);
  check(gs.set_pixel_watch({watched}, observer(events)), "GIF PSMCT32 watch accepted");
  Packet upload;
  upload.image(0x1000u, 0u, 5u, 2u, 0x80402010u);
  check(upload.submit(gif) && events.empty() && gif.read_local32(0x1214u) == 0x80402010u,
        "IMAGE upload supplies texture but is not a watched raster operation");
  const auto tex0 = 0x10ull | (1ull << 14u) | (3ull << 26u) | (2ull << 30u) |
                    (1ull << 34u) | (1ull << 35u); // 8x4 RGBA DECAL.
  const auto clamp = 2ull | (3ull << 2u) | (2ull << 4u) | (5ull << 14u) |
                     (1ull << 24u) | (2ull << 34u); // U region clamp; V region repeat.
  Packet draw;
  draw.ad(target_frame, 0x4Cu); draw.ad(tex0, 6u); draw.ad(clamp, 8u);
  draw.ad(0x42u, 0x42u); draw.ad(0u, 0x47u); draw.ad(0x7700000033ull, 0x3Bu);
  draw.ad(0x113u, 0u); draw.ad(0x70123456u, 1u); draw.ad(uv(11u, 14u), 3u);
  draw.ad(xyz(4u, 4u), 5u); draw.ad(xyz(8u, 4u), 5u); draw.ad(xyz(4u, 8u), 5u);
  check(draw.submit(gif) && events.size() == 1u, "textured triangle covers the watched tile once");
  if (!events.empty()) {
    const auto& event = events.back();
    check(event.draw.kind == ps2vita::GsTracePrimitive::Triangle &&
              event.draw.sequence == 0u && event.draw.packet == 2u &&
              event.draw.prim == 0x113u && event.draw.frame == target_frame &&
              event.frame == target_frame && event.draw.tex0 == tex0 &&
              event.draw.clamp == clamp && event.draw.alpha == 0x42u &&
              event.draw.test == 0u && event.draw.texa == 0x7700000033ull,
          "GIF event snapshots primitive sequence, submission ordinal, and GS state");
    check(event.texture.valid && event.texture.raw_u == 11u && event.texture.raw_v == 14u &&
              event.texture.u == 5u && event.texture.v == 2u && event.texture.format == 0u &&
              event.texture.address == 0x1214u && event.texture.raw_texel == 0x80402010u &&
              event.texture.expanded_texel == 0x80402010u &&
              event.texture.vertex_color == 0x70123456u &&
              event.texture.output_color == 0x80402010u && event.input_color == 0x80402010u &&
              event.before == 0u && event.after == 0x80402010u && event.x == 5 && event.y == 5,
          "PSMCT32 trace retains raw/clamped UV, exact texel address, and texture function output");
  }
  events.clear();
  Packet point;
  point.ad(0u, 0u); point.ad(0x88776655u, 1u); point.ad(xyz(5u, 5u), 5u);
  check(point.submit(gif) && events.size() == 1u &&
            events[0].draw.kind == ps2vita::GsTracePrimitive::Point &&
            events[0].draw.sequence == 0u && events[0].draw.packet == 3u &&
            !events[0].texture.valid && events[0].before == 0x80402010u,
        "new untextured draw clears texture provenance and has its own kind counter");
  events.clear();
  check(point.submit(gif) && events.size() == 1u && events[0].draw.sequence == 1u &&
            events[0].draw.packet == 4u,
        "point sequence increments independently of triangle sequence");
  events.clear();
  Packet suppressed;
  suppressed.ad(xyz(5u, 5u), 0xDu); // XYZ3 advances without a drawing kick.
  check(suppressed.submit(gif) && events.empty() && gif.points_emitted() == 2u,
        "suppressed XYZ3 kick reports no write and does not increment point draws");
  Packet line;
  line.ad(1u, 0u); line.ad(0x80ABCDEFu, 1u);
  line.ad(xyz(5u, 5u), 5u); line.ad(xyz(6u, 5u), 5u);
  check(line.submit(gif) && events.size() == 1u &&
            events[0].draw.kind == ps2vita::GsTracePrimitive::Line &&
            events[0].draw.sequence == 0u && events[0].draw.packet == 6u &&
            !events[0].texture.valid,
        "line reports its own draw kind and sequence without stale texture metadata");
  events.clear();
  Packet collapsed;
  collapsed.ad(3u, 0u); collapsed.ad(xyz(5u, 5u), 5u);
  collapsed.ad(xyz(5u, 5u), 5u); collapsed.ad(xyz(5u, 5u), 5u);
  check(collapsed.submit(gif) && events.empty() && gif.triangles_emitted() == 2u,
        "collapsed triangle advances draw count but cannot claim a watched color write");
  check(gs.set_pixel_watch({}), "GIF watch can be disabled");
  check(draw.submit(gif) && events.empty(), "disabled GIF instrumentation makes no callback");
  check(gs.set_pixel_watch({watched}, observer(events)), "GIF watch can be re-enabled");
  gif.reset();
  check(events.empty() && gs.pixel_watch_enabled(), "GIF reset clears memory without raster callbacks");
  check(draw.submit(gif) && events.size() == 1u && events[0].draw.sequence == 0u &&
            events[0].draw.packet == 1u && events[0].texture.raw_texel == 0u,
        "GIF reset restarts sequence and submission provenance and clears texture memory");
}

void test_gif_texture16() {
  Events events;
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  const auto watched = address(40u, 640u, 8u, 12u);
  check(gs.set_pixel_watch({watched}, observer(events)), "GIF PSMCT16 watch accepted");
  const auto tex0 = 0x20ull | (1ull << 14u) | (2ull << 20u) |
                    (2ull << 26u) | (2ull << 30u); // 4x4 RGB MODULATE.
  constexpr std::uint64_t texa = 0xA500008033ull; // TA0=51, AEM=1, TA1=165.
  struct Case { std::uint32_t raw, expanded, output; };
  const std::array<Case, 3> cases{{{0xFC1Fu, 0xA5F800F8u, 0x40F800F8u},
                                  {0x03E0u, 0x3300F800u, 0x4000F800u},
                                  {0u, 0u, 0x40000000u}}};
  std::uint32_t prior = 0u;
  for (std::size_t i = 0; i < cases.size(); ++i) {
    events.clear();
    Packet upload;
    upload.image(0x2000u, 2u, 1u, 3u, cases[i].raw);
    check(upload.submit(gif) && events.empty(),
          "PSMCT16 IMAGE is excluded from raster events " + std::to_string(i));
    Packet draw;
    draw.ad(frame(40u), 0x4Cu); draw.ad(tex0, 6u); draw.ad(0u, 8u);
    draw.ad(texa, 0x3Bu); draw.ad(0x116u, 0u); draw.ad(0x40808080u, 1u);
    draw.ad(uv(9u, 7u), 3u); draw.ad(xyz(2u, 3u), 5u);
    draw.ad(uv(9u, 7u), 3u); draw.ad(xyz(3u, 4u), 5u);
    check(draw.submit(gif) && events.size() == 1u,
          "PSMCT16 textured sprite reports one covered tile " + std::to_string(i));
    if (!events.empty()) {
      const auto& event = events[0];
      check(event.draw.kind == ps2vita::GsTracePrimitive::Sprite &&
                event.draw.sequence == i && event.draw.packet == 2u * (i + 1u) &&
                event.draw.tex0 == tex0 && event.draw.texa == texa &&
                event.texture.valid && event.texture.format == 2u &&
                event.texture.raw_u == 9u && event.texture.raw_v == 7u &&
                event.texture.u == 1u && event.texture.v == 3u &&
                event.texture.address == 0x2182u && event.texture.raw_texel == cases[i].raw &&
                event.texture.expanded_texel == cases[i].expanded &&
                event.texture.vertex_color == 0x40808080u &&
                event.texture.output_color == cases[i].output &&
                event.input_color == cases[i].output && event.before == prior &&
                event.after == cases[i].output && gif.read_local32(watched) == cases[i].output,
            "PSMCT16 raw halfword, TEXA expansion, repeat UV, and RGB alpha provenance " +
                std::to_string(i));
    }
    prior = cases[i].output;
  }
}

void test_gif_watched_image_and_feedback() {
  Events events;
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  const auto watched = address(32u, 64u, 4u, 4u);
  check(gs.set_pixel_watch({watched}, observer(events)), "feedback watch accepted");
  Packet upload;
  upload.image(0x40000u, 0u, 4u, 4u, 0x80402010u);
  check(upload.submit(gif) && events.empty() && gif.read_local32(watched) == 0x80402010u,
        "IMAGE touching the exact watched physical address is not a raster event");
  Packet draw;
  // Texture and framebuffer share both base and pitch. MODULATE retains RGB
  // (vertex=128), halves alpha (vertex=64), then ALPHA adds source RGB to Cd.
  const auto tex0 = 0x400ull | (1ull << 14u) | (3ull << 26u) |
                    (3ull << 30u) | (1ull << 34u);
  draw.ad(frame(32u, 1u), 0x4Cu); draw.ad(tex0, 6u); draw.ad(0u, 8u);
  draw.ad(0x8000000068ull, 0x42u); // (Cs-0)*FIX/128+Cd, FIX=128.
  draw.ad(1u, 0x46u); draw.ad(0x156u, 0u); draw.ad(0x40808080u, 1u);
  draw.ad(uv(4u, 4u), 3u); draw.ad(xyz(1u, 1u), 5u);
  draw.ad(uv(4u, 4u), 3u); draw.ad(xyz(2u, 2u), 5u);
  check(draw.submit(gif) && events.size() == 1u,
        "one aliased texture/target sprite emits one watched operation");
  if (!events.empty()) {
    const auto& event = events[0];
    check(event.before == 0x80402010u && event.input_color == 0x40402010u &&
              event.after == 0x40804020u && gif.read_local32(watched) == event.after,
          "feedback blend has independently calculated input and doubled RGB result");
    check(event.texture.address == watched && event.texture.raw_texel == event.before &&
              event.texture.expanded_texel == event.before &&
              event.texture.output_color == event.input_color &&
              event.texture.raw_texel != event.after,
          "feedback provenance preserves sampled pre-write texel instead of resampling destination");
  }
}

void test_gif_observational_equality() {
  Events events;
  ps2vita::Gs off, on;
  ps2vita::Gif off_gif(off), on_gif(on);
  check(on.set_pixel_watch({address(32u, 640u, 16u, 16u)}, observer(events)),
        "GIF observational watch accepted");
  Packet packet;
  packet.image(0x1000u, 0u, 1u, 1u, 0x80604020u);
  packet.ad(frame(32u), 0x4Cu);
  packet.ad(0x10ull | (1ull << 14u) | (2ull << 26u) | (2ull << 30u) |
            (1ull << 34u) | (1ull << 35u), 6u);
  packet.ad(0x116u, 0u); packet.ad(0x80808080u, 1u);
  packet.ad(uv(1u, 1u), 3u); packet.ad(xyz(2u, 2u), 5u);
  packet.ad(uv(1u, 1u), 3u); packet.ad(xyz(7u, 7u), 5u);
  packet.ad(0x113u, 0u); packet.ad(xyz(2u, 2u), 5u);
  packet.ad(xyz(8u, 2u), 5u); packet.ad(xyz(2u, 8u), 5u);
  packet.ad(frame(32u, 10u, 1u, 0x0000FF00u), 0x4Cu);
  packet.ad(0u, 0u); packet.ad(0xA1B2C3D4u, 1u); packet.ad(xyz(4u, 4u), 5u);
  check(packet.submit(off_gif) && packet.submit(on_gif) && !events.empty(),
        "GIF equality fixture executes texture, triangle, sprite, and masked point writes");
  bool equal_memory = true;
  for (std::uint32_t at = 0u; at < kVramSize; at += 4u)
    equal_memory = (off_gif.read_local32(at) == on_gif.read_local32(at)) && equal_memory;
  check(equal_memory, "GIF watch off/on leaves every logical VRAM word identical");
  bool equal_preview = true;
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x)
      equal_preview = (off.pixel(x, y) == on.pixel(x, y)) && equal_preview;
  check(equal_preview && off_gif.triangles_emitted() == on_gif.triangles_emitted() &&
            off_gif.sprites_emitted() == on_gif.sprites_emitted() &&
            off_gif.points_emitted() == on_gif.points_emitted() &&
            off_gif.packets_submitted() == on_gif.packets_submitted() &&
            off_gif.pending_bytes() == on_gif.pending_bytes(),
        "GIF watch off/on preserves preview, draw counters, and packet consumption");
}

} // namespace

int main() {
  test_configuration();
  test_alias_and_wrapped_tiles();
  test_masks_rejections_and_fallback();
  test_observational_equality_and_blend();
  test_gif_texture32_and_draw_metadata();
  test_gif_texture16();
  test_gif_watched_image_and_feedback();
  test_gif_observational_equality();
  std::printf("GS pixel-watch: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
