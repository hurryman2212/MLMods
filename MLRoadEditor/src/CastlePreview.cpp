#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "CastlePreview.hpp"
#include "Geometry.hpp"

// Physics/water dependencies require recurring refreshes. Wait after a complete
// warmup/sweep, then service a fixed pending set across parent-loop passes.
static constexpr std::uint64_t preview_lifetime_ms = 250;
static constexpr std::size_t preview_periodic_per_pass = 2;
static constexpr std::size_t preview_cache_limit = 128;
static constexpr std::int32_t preview_point_limit = 2048;
static constexpr std::int32_t preview_component_limit = 4096;
static constexpr std::int32_t preview_world_road_limit = 4096;
static constexpr std::int32_t preview_material_limit = 256;

static CastlePreviewNative native_preview{};
static CastlePreviewRead preview_read{};
static CastlePreviewClock preview_clock{};
static CastlePreviewButtons preview_buttons{};
static CastlePreviewAssignWeak preview_weak{};
static std::atomic<std::uint64_t> preview_generation{1};
static std::atomic<std::uint64_t> preview_calls{}, preview_reused{},
    preview_native_calls{}, preview_flushed{};
static std::atomic<std::uint64_t> preview_ineligible{}, preview_changed{},
    preview_expired{}, preview_overlap{}, preview_button_calls{};
static std::atomic<std::uint64_t> preview_periodic{}, preview_deferred{},
    preview_max_periodic_per_pass{};

struct PreviewBox {
  double min_x{}, min_y{}, max_x{}, max_y{};
};

struct PreviewCommon {
  std::vector<std::byte> guard;
  std::vector<std::byte> key;
  std::uintptr_t roads_begin{};
  std::vector<std::uintptr_t> roads;
  std::vector<std::uintptr_t> preview_roads;
  std::vector<std::size_t> preview_indices;
  std::map<std::uintptr_t, std::size_t> road_counts;
};

struct PreviewSnapshot {
  std::vector<std::byte> key;
  std::array<std::uint64_t, 8> session{};
  PreviewBox own_box{}, active_box{};
  std::uint64_t road_weak{};
  std::shared_ptr<const PreviewCommon> common;
  bool disjoint{};
};

struct PreviewEntry {
  void *road{};
  std::vector<std::byte> key;
  std::uint64_t road_weak{};
  bool previously_disjoint{};
  bool reused{};
  bool periodic_pending{};
  std::shared_ptr<const PreviewCommon> common;
};

struct PreviewBatch {
  std::vector<std::uintptr_t> roads;
  std::vector<std::uintptr_t> preview_roads;
  std::uintptr_t begin{}, end{}, last_cursor{};
  std::uint64_t refreshed{}, last_return{};
  std::size_t periodic_count{};
  bool valid{}, has_refresh{}, force_refresh{}, sweeping{};
  std::shared_ptr<const PreviewCommon> common;
};

struct PreviewThreadCache {
  std::vector<PreviewEntry> entries;
  std::array<std::uint64_t, 8> session{};
  std::uint64_t generation{}, last_time{};
  PreviewBatch batch;
  std::shared_ptr<const PreviewCommon> common;
  std::uintptr_t common_next_cursor{}, common_end{};
  bool busy{};
};

static thread_local PreviewThreadCache preview_cache;

static bool default_preview_read(std::uintptr_t address, void *output,
                                 std::size_t size) {
#ifdef _WIN32
  SIZE_T copied{};
  return ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void *>(address), output,
                           size, &copied) != 0 &&
         copied == size;
#else
  // Non-Windows builds exist only for the injected offline fixtures.
  (void)address;
  (void)output;
  (void)size;
  return false;
#endif
}

static std::uint64_t default_preview_clock() {
#ifdef _WIN32
  return GetTickCount64();
#else
  return 0;
#endif
}

static bool default_preview_buttons() {
#ifdef _WIN32
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
         (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
#else
  return true;
#endif
}

static bool read_preview_memory(std::uintptr_t base, std::size_t offset,
                                void *output, std::size_t size) {
  return base != 0 &&
         offset <= std::numeric_limits<std::uintptr_t>::max() - base &&
         size <= std::numeric_limits<std::uintptr_t>::max() - base - offset &&
         preview_read(base + offset, output, size);
}

template <std::size_t Size> struct PreviewBytes {
  std::array<std::byte, Size> bytes{};

  template <class T> T at(std::size_t offset) const {
    T value{};
    if (offset <= Size && sizeof(T) <= Size - offset)
      std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
  }
};

static void append_preview_bytes(std::vector<std::byte> &key, const void *bytes,
                                 std::size_t size) {
  const auto *begin = static_cast<const std::byte *>(bytes);
  key.insert(key.end(), begin, begin + size);
}

template <class T>
static void append_preview_value(std::vector<std::byte> &key, const T &value) {
  append_preview_bytes(key, &value, sizeof(value));
}

static bool preview_object_tag(std::uintptr_t address,
                               std::vector<std::byte> &key,
                               std::uint64_t *weak_result = nullptr) {
  PreviewBytes<0x28> header;
  if (!preview_weak ||
      !read_preview_memory(address, 0, header.bytes.data(),
                           header.bytes.size()) ||
      (header.at<std::uint32_t>(8) & 0x40018000U) != 0 ||
      header.at<std::int32_t>(0xc) < 0 || header.at<std::uintptr_t>(0x10) == 0)
    return false;
  std::uint64_t weak{};
  preview_weak(&weak, reinterpret_cast<void *>(address));
  if ((weak >> 32) == 0 ||
      static_cast<std::uint32_t>(weak) != header.at<std::uint32_t>(0xc))
    return false;
  append_preview_value(key, address);
  append_preview_bytes(key, header.bytes.data(), header.bytes.size());
  append_preview_value(key, weak);
  if (weak_result)
    *weak_result = weak;
  return true;
}

template <class T>
static bool preview_array(const NativeArray<T> &array, std::int32_t limit,
                          std::vector<T> &values, std::vector<std::byte> &key) {
  if (array.count < 0 || array.count > limit || array.capacity < array.count ||
      array.capacity < 0 || (array.count != 0 && array.data == nullptr))
    return false;
  append_preview_value(key, array);
  values.resize(static_cast<std::size_t>(array.count));
  if (!values.empty()) {
    const auto size = values.size() * sizeof(T);
    if (!read_preview_memory(reinterpret_cast<std::uintptr_t>(array.data), 0,
                             values.data(), size))
      return false;
    append_preview_bytes(key, values.data(), size);
  }
  return true;
}

static bool preview_road_box(const PreviewBytes<0x4c0> &road,
                             std::vector<std::byte> *key, PreviewBox &box) {
  const auto source = road.at<NativeArray<Vec3>>(0x318);
  std::vector<Vec3> points;
  std::vector<std::byte> ignored;
  if (source.count < 2 ||
      !preview_array(source, preview_point_limit, points,
                     key ? *key : ignored) ||
      road.at<std::uint8_t>(0x390) != 1)
    return false;
  box = road.at<PreviewBox>(0x370);
  if (!std::isfinite(box.min_x) || !std::isfinite(box.min_y) ||
      !std::isfinite(box.max_x) || !std::isfinite(box.max_y) ||
      box.min_x > box.max_x || box.min_y > box.max_y)
    return false;
  bool nondegenerate{};
  for (const auto &point : points) {
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z))
      return false;
    nondegenerate |= point.x != points.front().x || point.y != points.front().y;
    box.min_x = std::min(box.min_x, point.x);
    box.min_y = std::min(box.min_y, point.y);
    box.max_x = std::max(box.max_x, point.x);
    box.max_y = std::max(box.max_y, point.y);
  }
  return nondegenerate;
}

static bool
capture_preview(void *road_pointer, void *pawn_pointer,
                PreviewSnapshot &snapshot,
                std::shared_ptr<const PreviewCommon> *cached_common = nullptr,
                bool allow_common_reuse = false) {
  const auto road = reinterpret_cast<std::uintptr_t>(road_pointer);
  const auto pawn = reinterpret_cast<std::uintptr_t>(pawn_pointer);
  PreviewBytes<0xd00> pawn_bytes;
  PreviewBytes<0x4c0> road_bytes, active_bytes;
  PreviewBytes<0x1080> master_bytes;
  if (!read_preview_memory(pawn, 0, pawn_bytes.bytes.data(),
                           pawn_bytes.bytes.size()) ||
      pawn_bytes.at<std::uint8_t>(0x34c) != 1 ||
      pawn_bytes.at<std::uint8_t>(0x848) != 1 ||
      pawn_bytes.at<std::uint8_t>(0x7a0) != 1 ||
      pawn_bytes.at<std::uint8_t>(0xc58) != 0 ||
      !read_preview_memory(road, 0, road_bytes.bytes.data(),
                           road_bytes.bytes.size()) ||
      road_bytes.at<std::uint8_t>(0x2b3) != 1 ||
      road_bytes.at<std::uint8_t>(0x2b4) != 3 ||
      road_bytes.at<std::uint8_t>(0x2b5) == 0)
    return false;
  const auto master = pawn_bytes.at<std::uintptr_t>(0x340);
  const auto parent = pawn_bytes.at<std::uintptr_t>(0x840);
  const auto active = pawn_bytes.at<std::uintptr_t>(0x778);
  std::uintptr_t parent_master{};
  if (master == 0 || parent == 0 || active == 0 || active == road ||
      master != road_bytes.at<std::uintptr_t>(0x2a8) ||
      !read_preview_memory(parent, 0x2e8, &parent_master,
                           sizeof(parent_master)) ||
      parent_master != master ||
      !read_preview_memory(master, 0, master_bytes.bytes.data(),
                           master_bytes.bytes.size()) ||
      master_bytes.at<std::uintptr_t>(0x5b0) != pawn ||
      !read_preview_memory(active, 0, active_bytes.bytes.data(),
                           active_bytes.bytes.size()) ||
      (active_bytes.at<std::uint8_t>(0x2b4) != 0 &&
       active_bytes.at<std::uint8_t>(0x2b4) != 3) ||
      active_bytes.at<std::uintptr_t>(0x2a8) != master)
    return false;

  snapshot.key.reserve(4096);
  std::uint64_t pawn_weak{}, master_weak{}, parent_weak{};
  if (!preview_object_tag(road, snapshot.key, &snapshot.road_weak) ||
      !preview_object_tag(pawn, snapshot.key, &pawn_weak) ||
      !preview_object_tag(master, snapshot.key, &master_weak) ||
      !preview_object_tag(parent, snapshot.key, &parent_weak) ||
      !preview_object_tag(active, snapshot.key))
    return false;
  snapshot.session = {pawn,
                      pawn_weak,
                      master,
                      master_weak,
                      parent,
                      parent_weak,
                      master_bytes.at<std::uintptr_t>(0x20),
                      pawn_bytes.at<std::uintptr_t>(0x20)};

  NativeArray<std::uintptr_t> parent_roads{};
  std::vector<std::uintptr_t> pointers;
  if (!read_preview_memory(parent, 0xef0, &parent_roads, sizeof(parent_roads)))
    return false;
  const auto road_class = road_bytes.at<std::uintptr_t>(0x10);
  std::vector<std::byte> common_guard;
  append_preview_value(common_guard, snapshot.session);
  append_preview_value(common_guard, parent_roads);
  append_preview_value(common_guard, road_class);
  append_preview_value(common_guard, master_bytes.at<std::uint8_t>(0x301));
  append_preview_value(common_guard, master_bytes.at<std::uint8_t>(0x318));
  append_preview_value(common_guard, master_bytes.at<std::uintptr_t>(0x5b8));
  for (const auto offset : {0xd58, 0xe40, 0x1068})
    append_preview_value(common_guard, master_bytes.at<std::uintptr_t>(offset));
  for (const auto offset : {0xd68, 0xfe0, 0x590})
    append_preview_value(common_guard,
                         master_bytes.at<NativeArray<std::uintptr_t>>(offset));
  if (allow_common_reuse && cached_common && *cached_common &&
      (*cached_common)->guard == common_guard) {
    snapshot.common = *cached_common;
  } else {
    auto common = std::make_shared<PreviewCommon>();
    common->guard = std::move(common_guard);
    common->key = common->guard;
    if (!preview_array(parent_roads, preview_world_road_limit, common->roads,
                       common->key))
      return false;
    common->roads_begin = reinterpret_cast<std::uintptr_t>(parent_roads.data);
    // A changed callable subset can move the first cursor forward between
    // passes. Verify the same road class before reading the native preview
    // flag.
    for (std::size_t index{}; index < common->roads.size(); ++index) {
      const auto pointer = common->roads[index];
      ++common->road_counts[pointer];
      if (pointer == 0)
        continue;
      std::uintptr_t sibling_class{};
      std::uint8_t sibling_preview{};
      if (!read_preview_memory(pointer, 0x10, &sibling_class,
                               sizeof(sibling_class)) ||
          sibling_class == 0)
        return false;
      // Manor source arrays normally contain both Roads and StaticMeshActors.
      // Do not read road fields in smaller non-road objects. If different Road
      // subclasses are actually called, their different filtered lists start
      // separate native cohorts instead of inheriting this class's deadline.
      if (sibling_class != road_class)
        continue;
      if (!read_preview_memory(pointer, 0x2b3, &sibling_preview,
                               sizeof(sibling_preview)))
        return false;
      if (sibling_preview != 0) {
        common->preview_roads.push_back(pointer);
        common->preview_indices.push_back(index);
      }
    }
    append_preview_bytes(common->key, common->preview_roads.data(),
                         common->preview_roads.size() * sizeof(std::uintptr_t));
    for (const auto offset : {0xd68, 0xfe0})
      if (!preview_array(master_bytes.at<NativeArray<std::uintptr_t>>(offset),
                         preview_material_limit, pointers, common->key))
        return false;
    if (!preview_array(master_bytes.at<NativeArray<std::uintptr_t>>(0x590),
                       preview_world_road_limit, pointers, common->key))
      return false;
    if (cached_common && *cached_common && (*cached_common)->key == common->key)
      snapshot.common = *cached_common;
    else
      snapshot.common = std::move(common);
    if (cached_common)
      *cached_common = snapshot.common;
  }
  const auto membership = snapshot.common->road_counts.find(road);
  if (membership == snapshot.common->road_counts.end() ||
      membership->second != 1)
    return false;

  const auto root = road_bytes.at<std::uintptr_t>(0x1b8);
  const auto spline = road_bytes.at<std::uintptr_t>(0x2c0);
  std::array<double, 12> transform{};
  if (!preview_object_tag(root, snapshot.key) ||
      !preview_object_tag(spline, snapshot.key) ||
      !read_preview_memory(root, 0x1d0, transform.data(), sizeof(transform)) ||
      !std::all_of(transform.begin(), transform.end(),
                   [](double value) { return std::isfinite(value); }))
    return false;
  append_preview_value(snapshot.key, transform);
  const auto selected = pawn_bytes.at<std::uintptr_t>(0x858);
  append_preview_value(snapshot.key, selected);
  if (selected && !preview_object_tag(selected, snapshot.key))
    return false;

  const auto tangent = road_bytes.at<float>(0x2b8);
  const auto width = road_bytes.at<float>(0x2e8);
  const auto active_width = active_bytes.at<float>(0x2e8);
  if (!std::isfinite(tangent) || !std::isfinite(width) || width < 0 ||
      !std::isfinite(active_width) || active_width < 0)
    return false;
  // Native parameters, persistent source points, retained generated identities,
  // and every material selector are copied; no game-owned input is written.
  append_preview_bytes(snapshot.key, road_bytes.bytes.data() + 0x2b0, 12);
  append_preview_value(snapshot.key, width);
  append_preview_value(snapshot.key, road_bytes.at<std::uintptr_t>(0x2f8));
  append_preview_bytes(snapshot.key, road_bytes.bytes.data() + 0x330,
                       7 * sizeof(void *));
  append_preview_bytes(snapshot.key, road_bytes.bytes.data() + 0x370,
                       sizeof(PreviewBox) + 1);
  append_preview_value(snapshot.key, road_bytes.at<std::uintptr_t>(0x420));
  append_preview_value(snapshot.key, road_bytes.at<std::uint8_t>(0x4b8));
  append_preview_bytes(snapshot.key, active_bytes.bytes.data() + 0x2b0, 12);
  append_preview_value(snapshot.key, active_width);
  append_preview_value(snapshot.key, active_bytes.at<std::uint8_t>(0x4b8));
  if (!preview_road_box(road_bytes, &snapshot.key, snapshot.own_box) ||
      !preview_road_box(active_bytes, nullptr, snapshot.active_box))
    return false;

  const auto components = road_bytes.at<NativeArray<std::uintptr_t>>(0x2d8);
  if (components.count == 0 ||
      !preview_array(components, preview_component_limit, pointers,
                     snapshot.key))
    return false;
  for (const auto component : pointers)
    if (!preview_object_tag(component, snapshot.key))
      return false;
  // Native 0x4ccc630 rejects candidates whose RoadType is not 0, so an active
  // castle wall (type 3) cannot cut these sibling walls even when overlapping.
  // Switching types changes the key and requires a native cleanup rebuild.
  // Ordinary-road touching boxes remain overlap, matching the native inclusive
  // broad phase. Cached boxes already include native expansion.
  snapshot.disjoint = active_bytes.at<std::uint8_t>(0x2b4) == 3 ||
                      snapshot.active_box.max_x < snapshot.own_box.min_x ||
                      snapshot.active_box.min_x > snapshot.own_box.max_x ||
                      snapshot.active_box.max_y < snapshot.own_box.min_y ||
                      snapshot.active_box.min_y > snapshot.own_box.max_y;
  return true;
}

void castle_preview_reset() noexcept {
  preview_generation.fetch_add(1, std::memory_order_relaxed);
}

CastlePreviewMetrics castle_preview_metrics() noexcept {
  return {preview_calls.load(std::memory_order_relaxed),
          preview_reused.load(std::memory_order_relaxed),
          preview_native_calls.load(std::memory_order_relaxed),
          preview_flushed.load(std::memory_order_relaxed),
          preview_ineligible.load(std::memory_order_relaxed),
          preview_changed.load(std::memory_order_relaxed),
          preview_expired.load(std::memory_order_relaxed),
          preview_overlap.load(std::memory_order_relaxed),
          preview_button_calls.load(std::memory_order_relaxed),
          preview_periodic.load(std::memory_order_relaxed),
          preview_deferred.load(std::memory_order_relaxed),
          preview_max_periodic_per_pass.load(std::memory_order_relaxed)};
}

void configure_castle_preview_functions(CastlePreviewNative native,
                                        CastlePreviewClock clock,
                                        CastlePreviewButtons buttons,
                                        CastlePreviewAssignWeak weak,
                                        CastlePreviewRead read) noexcept {
  native_preview = native;
  preview_clock = clock ? clock : default_preview_clock;
  preview_buttons = buttons ? buttons : default_preview_buttons;
  preview_weak = weak;
  preview_read = read ? read : default_preview_read;
  castle_preview_reset();
}

void configure_castle_preview(std::uintptr_t image) noexcept {
  configure_castle_preview_functions(
      reinterpret_cast<CastlePreviewNative>(image + 0x4cd6430), nullptr,
      nullptr, reinterpret_cast<CastlePreviewAssignWeak>(image + 0x14938f0));
}

struct PreviewBusyScope {
  PreviewThreadCache &cache;
  explicit PreviewBusyScope(PreviewThreadCache &value) : cache(value) {
    cache.busy = true;
  }
  ~PreviewBusyScope() { cache.busy = false; }
};

void castle_preview_flush(void *pawn) noexcept {
  auto &cache = preview_cache;
  const auto generation = preview_generation.load(std::memory_order_relaxed);
  castle_preview_reset();
  cache.common_next_cursor = 0;
  if (cache.busy || cache.generation != generation)
    return;
  const PreviewBusyScope busy(cache);
  // Remove ownership before callbacks: a native rebuild may reenter or trigger
  // reset, and must never reuse an entry currently being finalized.
  auto entries = std::move(cache.entries);
  cache.entries.clear();
  for (const auto &entry : entries) {
    if (preview_generation.load(std::memory_order_relaxed) != generation + 1)
      break;
    if (!entry.reused && !entry.periodic_pending)
      continue;
    try {
      PreviewSnapshot current;
      if (capture_preview(entry.road, pawn, current) &&
          current.session == cache.session &&
          current.road_weak == entry.road_weak) {
        preview_flushed.fetch_add(1, std::memory_order_relaxed);
        native_preview(entry.road, 1);
      }
    } catch (...) {
      // FinishRoad still executes through its own wrapper. Invalid stale
      // previews are never dereferenced through an unvalidated cached pointer.
    }
  }
}

void castle_preview_regenerate(void *road, std::uint8_t force, void *pawn,
                               std::uintptr_t iterator, std::uintptr_t end) {
  preview_calls.fetch_add(1, std::memory_order_relaxed);
  auto &cache = preview_cache;
  if (cache.busy) {
    // Never leave a reusable snapshot across a nested native rebuild.
    castle_preview_reset();
    preview_native_calls.fetch_add(1, std::memory_order_relaxed);
    native_preview(road, force);
    return;
  }
  const PreviewBusyScope busy(cache);
  PreviewSnapshot before;
  bool eligible{};
  const auto generation = preview_generation.load(std::memory_order_relaxed);
  try {
    const auto now = preview_clock();
    if (cache.generation != generation || now < cache.last_time) {
      cache.entries.clear();
      cache.session = {};
      cache.batch = {};
      cache.common.reset();
      cache.common_next_cursor = 0;
      cache.generation = generation;
    }
    cache.last_time = now;
    const bool buttons = preview_buttons();
    // The hooked native loop advances monotonically through one parent array.
    // Reuse common world inputs only for its exact next callable sibling.
    // A native callback, reset, changed guard or nonsequential cursor forces a
    // new capture; no common snapshot survives an unobserved loop boundary.
    const bool common_reusable =
        cache.common_next_cursor != 0 && iterator == cache.common_next_cursor &&
        end == cache.common_end && cache.common &&
        iterator >= cache.common->roads_begin &&
        (iterator - cache.common->roads_begin) / sizeof(std::uintptr_t) <
            cache.common->roads.size() &&
        cache.common->roads[(iterator - cache.common->roads_begin) /
                            sizeof(std::uintptr_t)] ==
            reinterpret_cast<std::uintptr_t>(road);
    cache.common_next_cursor = 0;
    eligible =
        force == 1 && !buttons &&
        capture_preview(road, pawn, before, &cache.common, common_reusable);
    std::uintptr_t current_road{};
    if (eligible) {
      const auto &common = *before.common;
      const auto bytes = common.roads.size() * sizeof(std::uintptr_t);
      eligible =
          common.roads_begin != 0 &&
          bytes <=
              std::numeric_limits<std::uintptr_t>::max() - common.roads_begin &&
          end == common.roads_begin + bytes && iterator >= common.roads_begin &&
          iterator < end && iterator % alignof(std::uintptr_t) == 0 &&
          (iterator - common.roads_begin) % sizeof(std::uintptr_t) == 0 &&
          read_preview_memory(iterator, 0, &current_road,
                              sizeof(current_road)) &&
          current_road == reinterpret_cast<std::uintptr_t>(road);
    }
    if (!eligible) {
      if (buttons)
        preview_button_calls.fetch_add(1, std::memory_order_relaxed);
      else
        preview_ineligible.fetch_add(1, std::memory_order_relaxed);
      cache.entries.clear();
      cache.batch = {};
      cache.common.reset();
    } else {
      auto &batch = cache.batch;
      const auto &common = *before.common;
      if (cache.session != before.session ||
          batch.begin != common.roads_begin || batch.end != end ||
          (batch.common != before.common &&
           (batch.roads != common.roads ||
            batch.preview_roads != common.preview_roads))) {
        cache.entries.clear();
        cache.session = before.session;
        batch = {};
        batch.begin = common.roads_begin;
        batch.end = end;
        batch.roads = common.roads;
        batch.preview_roads = common.preview_roads;
      }
      batch.common = before.common;
      if (!batch.valid || iterator <= batch.last_cursor) {
        // A pending road leaves the sweep only after a native call. Never
        // requeue the early roads while later siblings are still pending.
        if (batch.valid &&
            (batch.force_refresh ||
             (batch.sweeping &&
              std::none_of(cache.entries.begin(), cache.entries.end(),
                           [](const PreviewEntry &entry) {
                             return entry.periodic_pending;
                           })))) {
          batch.refreshed = batch.last_return;
          batch.has_refresh = true;
          batch.sweeping = false;
        }
        batch.periodic_count = 0;
        batch.force_refresh = !batch.has_refresh;
        if (batch.force_refresh)
          cache.entries.clear();
        else if (!batch.sweeping &&
                 now - batch.refreshed >= preview_lifetime_ms) {
          for (auto &entry : cache.entries)
            entry.periodic_pending = true;
          batch.sweeping = true;
        }
        batch.valid = true;
      }
      batch.last_cursor = iterator;
      const auto found = std::find_if(
          cache.entries.begin(), cache.entries.end(),
          [road](const PreviewEntry &entry) { return entry.road == road; });
      const bool reusable =
          !batch.force_refresh && found != cache.entries.end() &&
          found->previously_disjoint && before.disjoint &&
          found->key == before.key && found->common == before.common &&
          generation == preview_generation.load(std::memory_order_relaxed);
      const bool periodic = reusable && found->periodic_pending &&
                            batch.periodic_count < preview_periodic_per_pass;
      if (reusable && !periodic) {
        const auto completed = preview_clock();
        if (completed >= cache.last_time) {
          batch.last_return = completed;
          cache.last_time = completed;
          found->reused = true;
          preview_reused.fetch_add(1, std::memory_order_relaxed);
          if (found->periodic_pending)
            preview_deferred.fetch_add(1, std::memory_order_relaxed);
          const auto index =
              (iterator - common.roads_begin) / sizeof(std::uintptr_t);
          const auto next =
              std::upper_bound(common.preview_indices.begin(),
                               common.preview_indices.end(), index);
          cache.common_next_cursor =
              next == common.preview_indices.end()
                  ? 0
                  : common.roads_begin + *next * sizeof(std::uintptr_t);
          cache.common_end = end;
          return;
        }
        cache.entries.clear();
        batch = {};
        eligible = false;
      } else {
        if (batch.force_refresh || periodic)
          preview_expired.fetch_add(1, std::memory_order_relaxed);
        else if (!before.disjoint ||
                 (found != cache.entries.end() && !found->previously_disjoint))
          preview_overlap.fetch_add(1, std::memory_order_relaxed);
        else if (found != cache.entries.end() &&
                 (found->key != before.key || found->common != before.common))
          preview_changed.fetch_add(1, std::memory_order_relaxed);
        if (periodic) {
          ++batch.periodic_count;
          preview_periodic.fetch_add(1, std::memory_order_relaxed);
          auto maximum =
              preview_max_periodic_per_pass.load(std::memory_order_relaxed);
          while (
              maximum < batch.periodic_count &&
              !preview_max_periodic_per_pass.compare_exchange_weak(
                  maximum, batch.periodic_count, std::memory_order_relaxed)) {
          }
        }
        // Mandatory key/overlap rebuilds also satisfy this road's pending
        // refresh; they are never held back by the optional-work budget.
        if (found != cache.entries.end())
          cache.entries.erase(found);
      }
    }
  } catch (...) {
    cache.entries.clear();
    cache.batch = {};
    eligible = false;
  }

  // Exactly one native call on every fallback; native exceptions retain their
  // original behavior and cannot accidentally trigger a second invocation.
  preview_native_calls.fetch_add(1, std::memory_order_relaxed);
  try {
    native_preview(road, force);
  } catch (...) {
    cache.entries.clear();
    cache.batch = {};
    throw;
  }
  try {
    const auto completed = preview_clock();
    if (completed < cache.last_time ||
        generation != preview_generation.load(std::memory_order_relaxed)) {
      cache.entries.clear();
      cache.batch = {};
      return;
    }
    cache.last_time = completed;
    cache.batch.last_return = completed;
    if (!eligible)
      return;
    PreviewSnapshot after;
    if (cache.entries.size() >= preview_cache_limit || preview_buttons() ||
        !capture_preview(road, pawn, after, &cache.common) ||
        after.session != before.session)
      return;
    const auto captured = preview_clock();
    if (captured < completed ||
        generation != preview_generation.load(std::memory_order_relaxed)) {
      cache.entries.clear();
      cache.batch = {};
      return;
    }
    cache.last_time = captured;
    cache.batch.last_return = captured;
    cache.entries.push_back({road, std::move(after.key), after.road_weak,
                             before.disjoint && after.disjoint, false, false,
                             after.common});
  } catch (...) {
    cache.entries.clear();
    cache.batch = {};
  }
}
