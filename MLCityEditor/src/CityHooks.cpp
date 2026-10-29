#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

#include "CityHooks.hpp"
#include "CityRuntime.hpp"
#include "Construction.hpp"
#include "Demolition.hpp"
#include "Eligibility.hpp"
#include "ExtensionCancel.hpp"
#include "Maturity.hpp"
#include "Retinue.hpp"
#include "Upgrade.hpp"
#include "Windmill.hpp"

struct CityTrampolines {
  std::uintptr_t resource{}, wealth{}, begin{}, setup{}, type{}, extension{},
      artisan{}, manor_cost{}, process_commands{}, can_upgrade{},
      upgrade_options{}, artisan_callback{}, assembly_request{},
      assembly_callback{};
};
static CityTrampolines trampolines;
using ManorCost = CityArray *(*)(void *, CityArray *, bool);
static ManorCost native_manor_cost{};

auto city_fingerprints() noexcept -> std::span<const CityFingerprint> {
  static constexpr std::array checks{
      CityFingerprint{
          0x4d15250,
          "48895c24104889742418574883ec30418bf8488bda488bf14183f810752c448b"},
      CityFingerprint{
          0x4d13440,
          "48895c240848896c24104889742418574883ec400f297c24208bda488be985d2"},
      CityFingerprint{
          0x4d12e00,
          "488954241048894c2408555341544156488d6c24c14881ec88000000458bf048"},
      CityFingerprint{
          0x4cfc2f0,
          "4c8bdc55574156498dab28ffffff4881ecc0010000488b05f4ee44044833c448"},
      CityFingerprint{
          0x4d0aaa0,
          "488bc44889582055488d68a14881eca000000048897008488bd9488b89e80200"},
      CityFingerprint{
          0x4cf9560,
          "40534883ec40488b01488bd948896c2450488974245848897c24600fb6a8c103"},
      CityFingerprint{
          0x4d2cd80,
          "4c8bdc55535657498dab88fdffff4881ec58030000488b0564e441044833c448"},
      CityFingerprint{
          0x4d2c8f0,
          "4c8bdc53574881ecf8000000488b05fde841044833c448898424d000000083b9"},
      CityFingerprint{
          0x4cf9750,
          "48895c241048896c24184889742420574883ec30488b01488bea488bd9c680c1"},
      CityFingerprint{
          0x4ce11f0,
          "488bc44c8948204c894018488950104889480855535657415541564157488da8"},
      CityFingerprint{
          0x4cb56d0,
          "4055488dac2420dbffffb8e0250000e84c482902482be0488b05125b49044833"},
      CityFingerprint{
          0x4d3c5e0,
          "4c8bdc5553498dab18ffffff4881ecd8010000488b0506ec40044833c4488945"},
      CityFingerprint{0x14938f0, "40534883ec20488bd94885d2741a8b52"},
      CityFingerprint{0x1497740, "448b49044585c974448b0185c0783e3b"},
      CityFingerprint{0x4d45280, "488b91f80200000f57c0486381000300"},
      CityFingerprint{0x4d452c0, "488b91f80200000f57c0486381000300"},
      CityFingerprint{0x4d27780, "488b91f80200000f28d9486381000300"},
      CityFingerprint{0x4d21730, "f30f1189ec080000c681e80800000144"},
      CityFingerprint{0x4d0b370, "488bc448895810488970184889782055"},
      CityFingerprint{
          0x4c6cda0,
          "8954241048894c24085741574881ec980000004863c2488bf94c8d3c80488b81"},
      CityFingerprint{
          0x4d09180,
          "89542410534883ec30488bd94881c1c8140000488b014c6341084e8d0c80493b"},
      CityFingerprint{
          0x4cc34c0,
          "40534883ec208b150441890465488b0425580000008bd9b964110000488b04d0"},
      CityFingerprint{
          0x4b1ef90,
          "488954241048894c240841554883ec504c63690844896c24704585ed750933c0"},
      CityFingerprint{0x4b1f1e0, "488954241048894c240841554883ec50"},
      CityFingerprint{0x4d253a0, "48894c2408555356574154415541564157"},
      CityFingerprint{
          0x48999e0,
          "48895c2408488974241048897c24184c8974242055488bec4883ec70"},
      CityFingerprint{
          0x4d24b60,
          "48894c240857415541564883ec70488db9600400004c896424584c8be9488d91"},
      CityFingerprint{
          0x4c49700,
          "48895c241848894c24085556574154415541564157488bec4881ec8000000049"},
      // Native constructor defaults for industrial fueling, not inventory.
      CityFingerprint{
          0x4cf82d9,
          "66898f700e0000888f720e0000c787740e0000ffffffff888f780e0000"},
      CityFingerprint{
          0x4b029a0,
          "4c8bdc4883ec78488b051aad97044885c00f8592000000488d05c23cbefe4989"},
      CityFingerprint{
          0x34c3ac0,
          "48895c241048897424184c8974242055488d6c24a94881ec90000000488b1a48"},
      CityFingerprint{
          0x4ba94c0,
          "4c8bdc49895b20555657498d6ba14881ecb0000000488b05241d5a044833c448"},
      CityFingerprint{0x7f52290, "c03a4c4301000000"},
      CityFingerprint{0x1382c70, "e90b000000cccccccccccccccccccccc"},
      CityFingerprint{
          0x4d217d0,
          "405341554883ec380f297424204c8d91380500004533ed4c89642468450fb6e1"},
      CityFingerprint{
          0x4d55240,
          "4c8bdc49895b205556415541564157498dab48ffffff4881ec90010000488b05"},
      CityFingerprint{0x4b2b3d0, "48895c24184889742420555741544156"},
      CityFingerprint{0x4b343a0, "4c8bdc55574156498d6ba14881ecb000"},
      CityFingerprint{0x4d1a650, "4c8bdc55498dabe8fdffff4881ec1003"},
      CityFingerprint{0x4d32ef0, "8854241048894c240855535657415441"},
      CityFingerprint{0x4ca1bb0, "4053555657415641574881ece8000000"},
      CityFingerprint{0x4b244b0, "e81b6f0000"},
      CityFingerprint{0x4b2b990, "e80b8a0000"},
      CityFingerprint{0x4b35f59, "e8f2461e00"},
      CityFingerprint{0x4b366a8, "e8a33f1e00"},
      CityFingerprint{0x4b244f5, "e8f6e92000"},
      CityFingerprint{0x4d34068, "e843dbf6ff"},
      CityFingerprint{0x4d30626, "498b8424e8020000"},
      CityFingerprint{0x4d30530, "4c8bdc55498dab08fcffff4881ecf0040000"},
      CityFingerprint{0x4d30620, "32c088442461"},
      CityFingerprint{0x4ab3200, "c6814c03000001c3"},
      CityFingerprint{0x4ab3210, "c6814d03000001c3"},
      CityFingerprint{0x7f45500, "0100e00200000000"},
      CityFingerprint{
          0x4cf48b0,
          "40534883ec40488bd9488b89b00500004885c90f84de01000080b95011000000"},
      CityFingerprint{
          0x4d2b340,
          "4055535657415441564157488d6c24d94881ecf0000000418b400c4533e44489"},
      CityFingerprint{
          0x4d3a9a0,
          "48895c2418555657415641574883ec60488b05490841044833c4488944245048"},
      CityFingerprint{0x4b16870,
                      "4885d20f845b020000488bc4555657488da878fcffff4881"},
      CityFingerprint{0x4718f10,
                      "48895c24084889742410574883ec30486359088bf2488bf9"},
      CityFingerprint{0x4ccb350,
                      "48895c2410574883ec30448b056fc28804488bd965488b04"},
      CityFingerprint{0x10dd950,
                      "48895c24084889742410574883ec30486359088bf2488bf9"},
      CityFingerprint{0x1128900,
                      "4885c9742e534883ec20488bd9488b0dfc5f09084885c975"},
      // Master.ready's initialization context is independent of the
      // RoadEditor post-load hook at 0x4c84b94, in either mod load order.
      CityFingerprint{
          0x4c821ee,
          "897c24584088b918030000e8628a83fe488d4c2440ff151f794102e822f854fc"},
      CityFingerprint{0x4d0c84f,
                      "488b9ee80200004863bba00f00008d47018983a00f00003b"}};
  return checks;
}

static auto city_decode(std::string_view text) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> output;
  if (text.size() % 2 != 0)
    return output;
  for (std::size_t i = 0; i < text.size(); i += 2) {
    unsigned value{};
    for (std::size_t j = 0; j < 2; ++j) {
      const char c = text[i + j];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        return {};
      value =
          value * 16 + static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    output.push_back(static_cast<std::uint8_t>(value));
  }
  return output;
}

auto city_validate_image(const std::uint8_t *image, std::size_t size) -> bool {
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 nt{};
  if (size != 0x09dfd000 ||
      !city_read(reinterpret_cast<std::uintptr_t>(image), &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 0x1000 ||
      !city_read(reinterpret_cast<std::uintptr_t>(image) + dos.e_lfanew, &nt,
                 sizeof(nt)) ||
      nt.Signature != IMAGE_NT_SIGNATURE ||
      nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt.FileHeader.TimeDateStamp != 0x6836c1ba ||
      nt.OptionalHeader.SizeOfImage != size ||
      nt.OptionalHeader.CheckSum != 0x09a2ba15)
    return false;
  for (const auto &check : city_fingerprints()) {
    const auto expected = city_decode(check.bytes);
    std::vector<std::uint8_t> actual(expected.size());
    if (expected.empty() || check.rva >= size ||
        expected.size() > size - check.rva ||
        !city_read(reinterpret_cast<std::uintptr_t>(image) + check.rva,
                   actual.data(), actual.size()) ||
        actual != expected)
      return false;
  }
  return true;
}

static auto city_manor_cost(void *building, CityArray *result, bool include_all)
    -> CityArray * {
  if (result != nullptr && city_edited_manor(building)) {
    *result = {};
    return result;
  }
  return native_manor_cost ? native_manor_cost(building, result, include_all)
                           : result;
}

static auto city_find_windmill(void *building) -> void * {
  using GetClass = void *(*)();
  using MarkReachable = void (*)(void *);
  using FindComponent = void *(*)(void *, void **);
  auto *component_class = reinterpret_cast<GetClass>(city_image + 0x4b029a0)();
  std::uint8_t tracking{};
  if (!city_valid_object(component_class) ||
      !city_read(city_image + 0x92a4dd8, &tracking, sizeof(tracking))) {
    return nullptr;
  }
  // Mirror FinishConstruction's TObjectPtr class access, including its GC
  // reachability barrier, before the native actor component lookup.
  if (tracking != 0) {
    reinterpret_cast<MarkReachable>(city_image + 0x1382c70)(component_class);
  }
  return reinterpret_cast<FindComponent>(city_image +
                                         0x34c3ac0)(building, &component_class);
}

static auto city_process_commands(void *master) -> void {
  // The existing game-thread hook calls the original exactly once through
  // the retinue handler. UE4SS on_update only reports counters; it never
  // touches windmill components or timers from its separate update thread.
  city_process_retinue_requests(master);
  city_repair_windmills(master);
}

auto city_prepare_callbacks() noexcept -> void {
  ExtensionCancelApi extension_cancel;
  extension_cancel.release_family =
      reinterpret_cast<decltype(extension_cancel.release_family)>(city_image +
                                                                  0x4c6cda0);
  extension_cancel.disable_recipe =
      reinterpret_cast<decltype(extension_cancel.disable_recipe)>(city_image +
                                                                  0x4d09180);
  extension_cancel.remove_pointer =
      reinterpret_cast<decltype(extension_cancel.remove_pointer)>(city_image +
                                                                  0x4b1ef90);
  extension_cancel.refresh_resources =
      reinterpret_cast<decltype(extension_cancel.refresh_resources)>(
          city_image + 0x4d24b60);
  extension_cancel.refresh_region =
      reinterpret_cast<decltype(extension_cancel.refresh_region)>(city_image +
                                                                  0x4c49700);
  extension_cancel.building_stats =
      reinterpret_cast<decltype(extension_cancel.building_stats)>(city_image +
                                                                  0x4cc34c0);
  extension_cancel.grow_pointers =
      reinterpret_cast<decltype(extension_cancel.grow_pointers)>(city_image +
                                                                 0x4718f10);
  extension_cancel.remove_upgrade =
      reinterpret_cast<decltype(extension_cancel.remove_upgrade)>(city_image +
                                                                  0x4b1f1e0);
  extension_cancel.apply_upgrade_visuals =
      reinterpret_cast<decltype(extension_cancel.apply_upgrade_visuals)>(
          city_image + 0x4d253a0);
  extension_cancel.empty_production_entries =
      reinterpret_cast<decltype(extension_cancel.empty_production_entries)>(
          city_image + 0x48999e0);
  extension_cancel.free_memory =
      reinterpret_cast<decltype(extension_cancel.free_memory)>(city_image +
                                                               0x1128900);
  configure_extension_cancel(extension_cancel);
  WindmillApi windmill;
  windmill.find_component = &city_find_windmill;
  windmill.activate =
      reinterpret_cast<decltype(windmill.activate)>(city_image + 0x4ba94c0);
  configure_windmill(windmill);
  ConstructionApi construction;
  construction.add_building =
      reinterpret_cast<decltype(construction.add_building)>(city_image +
                                                            0x4b2b3d0);
  construction.change_silver =
      reinterpret_cast<decltype(construction.change_silver)>(city_image +
                                                             0x4b343a0);
  construction.initialize = reinterpret_cast<decltype(construction.initialize)>(
      city_image + 0x4d1a650);
  configure_construction(construction);
  DemolitionApi demolition;
  demolition.demolish =
      reinterpret_cast<decltype(demolition.demolish)>(city_image + 0x4d32ef0);
  demolition.spawn_building =
      reinterpret_cast<decltype(demolition.spawn_building)>(city_image +
                                                            0x4ca1bb0);
  configure_demolition(demolition);
  UpgradeApi upgrade;
  upgrade.begin = reinterpret_cast<decltype(upgrade.begin)>(trampolines.begin);
  upgrade.setup = reinterpret_cast<decltype(upgrade.setup)>(trampolines.setup);
  upgrade.type_callback =
      reinterpret_cast<decltype(upgrade.type_callback)>(trampolines.type);
  upgrade.extension =
      reinterpret_cast<decltype(upgrade.extension)>(trampolines.extension);
  upgrade.artisan =
      reinterpret_cast<decltype(upgrade.artisan)>(trampolines.artisan);
  upgrade.artisan_callback =
      reinterpret_cast<decltype(upgrade.artisan_callback)>(
          trampolines.artisan_callback);
  upgrade.assembly_request =
      reinterpret_cast<decltype(upgrade.assembly_request)>(
          trampolines.assembly_request);
  upgrade.assembly_callback =
      reinterpret_cast<decltype(upgrade.assembly_callback)>(
          trampolines.assembly_callback);
  upgrade.resource_cost =
      reinterpret_cast<decltype(upgrade.resource_cost)>(trampolines.resource);
  upgrade.wealth_cost =
      reinterpret_cast<decltype(upgrade.wealth_cost)>(trampolines.wealth);
  upgrade.weak_assign =
      reinterpret_cast<decltype(upgrade.weak_assign)>(city_image + 0x14938f0);
  upgrade.weak_get =
      reinterpret_cast<decltype(upgrade.weak_get)>(city_image + 0x1497740);
  upgrade.work_done =
      reinterpret_cast<decltype(upgrade.work_done)>(city_image + 0x4d45280);
  upgrade.work_required =
      reinterpret_cast<decltype(upgrade.work_required)>(city_image + 0x4d452c0);
  upgrade.add_work =
      reinterpret_cast<decltype(upgrade.add_work)>(city_image + 0x4d27780);
  upgrade.visual =
      reinterpret_cast<decltype(upgrade.visual)>(city_image + 0x4d21730);
  upgrade.finish =
      reinterpret_cast<decltype(upgrade.finish)>(city_image + 0x4d0b370);
  upgrade.reconstruction_cost =
      reinterpret_cast<decltype(upgrade.reconstruction_cost)>(city_image +
                                                              0x4d12e00);
  upgrade.initialize_type = reinterpret_cast<decltype(upgrade.initialize_type)>(
      city_image + 0x4d1a650);
  MaturityApi maturity;
  maturity.refresh_progress =
      reinterpret_cast<decltype(maturity.refresh_progress)>(city_image +
                                                            0x4d217d0);
  maturity.refresh_visuals =
      reinterpret_cast<decltype(maturity.refresh_visuals)>(city_image +
                                                           0x4d55240);
  configure_maturity(maturity);
  upgrade.mature = &city_mature_new_orchard;
  configure_upgrade(upgrade);
  native_manor_cost = reinterpret_cast<ManorCost>(trampolines.manor_cost);
  EligibilityApi eligibility;
  eligibility.can_upgrade = reinterpret_cast<decltype(eligibility.can_upgrade)>(
      trampolines.can_upgrade);
  eligibility.options = reinterpret_cast<decltype(eligibility.options)>(
      trampolines.upgrade_options);
  eligibility.upgrade_row = reinterpret_cast<decltype(eligibility.upgrade_row)>(
      city_image + 0x4ccb350);
  eligibility.resize_int_array =
      reinterpret_cast<decltype(eligibility.resize_int_array)>(city_image +
                                                               0x10dd950);
  eligibility.free_memory = reinterpret_cast<decltype(eligibility.free_memory)>(
      city_image + 0x1128900);
  configure_eligibility(eligibility);
  RetinueApi retinue;
  retinue.process_commands =
      reinterpret_cast<decltype(retinue.process_commands)>(
          trampolines.process_commands);
  retinue.initialize_empty_squad =
      reinterpret_cast<decltype(retinue.initialize_empty_squad)>(city_image +
                                                                 0x4b16870);
  retinue.grow_pointer_array =
      reinterpret_cast<decltype(retinue.grow_pointer_array)>(city_image +
                                                             0x4718f10);
  configure_retinue(retinue);
}

auto city_clear_callbacks() noexcept -> void {
  configure_construction({});
  configure_demolition({});
  configure_upgrade({});
  configure_maturity({});
  configure_eligibility({});
  configure_extension_cancel({});
  configure_retinue({});
  configure_windmill({});
  native_manor_cost = nullptr;
  city_image = 0;
}

auto city_supplies_bridge(const void *callback) -> std::vector<std::uint8_t> {
  // Four original register arguments remain intact; RDI becomes argument five.
  std::vector<std::uint8_t> code{0x48, 0x83, 0xec, 0x28, 0x48, 0x89,
                                 0x7c, 0x24, 0x20, 0x48, 0xb8};
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *raw = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), raw, raw + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3});
  return code;
}

auto city_manor_bridge(const void *callback, const void *resume)
    -> std::vector<std::uint8_t> {
  // At this exact native midpoint RSP is 16-byte aligned, R12 is the Manor,
  // and the native instaBuild local is RSP+0x61. Save 0x100 bytes in total.
  std::vector<std::uint8_t> code{0x9c, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
                                 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
                                 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                 0x48, 0x81, 0xec, 0x80, 0,    0,    0};
  for (std::uint8_t reg = 0; reg < 6; ++reg)
    code.insert(code.end(),
                {0xf3, 0x0f, 0x7f, static_cast<std::uint8_t>(0x44 + reg * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + reg * 16)});
  code.insert(code.end(), {0x4c, 0x89, 0xe1, 0x48, 0x8d, 0x94, 0x24, 0x61, 0x01,
                           0, 0, 0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *raw = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), raw, raw + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0});
  for (std::uint8_t reg = 0; reg < 6; ++reg)
    code.insert(code.end(),
                {0xf3, 0x0f, 0x6f, static_cast<std::uint8_t>(0x44 + reg * 8),
                 0x24, static_cast<std::uint8_t>(0x20 + reg * 16)});
  code.insert(code.end(),
              {0x48, 0x81, 0xc4, 0x80, 0,    0,    0,    0x41, 0x5f, 0x41,
               0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x41, 0x5b, 0x41, 0x5a, 0x41,
               0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58,
               0x9d, 0x49, 0x8b, 0x84, 0x24, 0xe8, 0x02, 0,    0});
  append_city_jump(code, resume);
  return code;
}

auto city_make_patches() -> std::vector<CityCodePatch> {
  std::vector<CityCodePatch> patches;
  patches.push_back(make_city_call_patch(
      0x4b244b0, 0x4b2b3d0,
      reinterpret_cast<const void *>(&city_place_building)));
  patches.push_back(make_city_call_patch(
      0x4b2b990, 0x4b343a0,
      reinterpret_cast<const void *>(&city_construction_treasury)));
  for (const auto rva : {0x4b35f59U, 0x4b366a8U}) {
    auto patch = make_city_call_patch(
        rva, 0x4d1a650, reinterpret_cast<const void *>(&city_initialize_plot));
    patch.stub.insert(patch.stub.begin(), {0x48, 0x89, 0xfa}); // RDX=pawn RDI.
    patches.push_back(std::move(patch));
  }
  auto demolition = make_city_call_patch(
      0x4b244f5, 0x4d32ef0, reinterpret_cast<const void *>(&city_demolish));
  demolition.stub.insert(demolition.stub.begin(),
                         {0x49, 0x89, 0xf0}); // R8=pawn RSI.
  patches.push_back(std::move(demolition));
  auto supplies = make_city_call_patch(
      0x4d34068, 0x4ca1bb0,
      reinterpret_cast<const void *>(&city_demolition_supplies));
  supplies.stub = city_supplies_bridge(
      reinterpret_cast<const void *>(&city_demolition_supplies));
  patches.push_back(std::move(supplies));
  patches.push_back(make_city_entry_patch(
      0x4d15250, city_decode("48895c2410"),
      reinterpret_cast<const void *>(&city_upgrade_resource_cost),
      &trampolines.resource));
  patches.push_back(make_city_entry_patch(
      0x4d13440, city_decode("48895c2408"),
      reinterpret_cast<const void *>(&city_upgrade_wealth_cost),
      &trampolines.wealth));
  patches.push_back(make_city_call_patch(
      0x4d1adc3, 0x4d12e00,
      reinterpret_cast<const void *>(&city_reconstruction_resource_cost)));
  patches.push_back(make_city_entry_patch(
      0x4cfc2f0, city_decode("4c8bdc5557"),
      reinterpret_cast<const void *>(&city_begin_upgrade), &trampolines.begin));
  patches.push_back(make_city_entry_patch(
      0x4d0aaa0, city_decode("488bc448895820"),
      reinterpret_cast<const void *>(&city_upgrade_cost_setup),
      &trampolines.setup));
  patches.push_back(make_city_entry_patch(
      0x4cf9560, city_decode("40534883ec40"),
      reinterpret_cast<const void *>(&city_upgrade_type_callback),
      &trampolines.type));
  patches.push_back(make_city_call_patch(
      0x4cf9682, 0x4d1a650,
      reinterpret_cast<const void *>(&city_initialize_reconstruction)));
  patches.push_back(make_city_entry_patch(
      0x4d2cd80, city_decode("4c8bdc5553"),
      reinterpret_cast<const void *>(&city_change_extension),
      &trampolines.extension));
  patches.push_back(make_city_entry_patch(
      0x4d2c8f0, city_decode("4c8bdc5357"),
      reinterpret_cast<const void *>(&city_change_artisan),
      &trampolines.artisan));
  patches.push_back(make_city_entry_patch(
      0x4cf9750, city_decode("48895c2410"),
      reinterpret_cast<const void *>(&city_upgrade_artisan_callback),
      &trampolines.artisan_callback));
  patches.push_back(make_city_entry_patch(
      0x4ce11f0, city_decode("488bc44c894820"),
      reinterpret_cast<const void *>(&city_upgrade_assembly_request),
      &trampolines.assembly_request));
  patches.push_back(make_city_entry_patch(
      0x4cb56d0, city_decode("4055488dac2420dbffff"),
      reinterpret_cast<const void *>(&city_upgrade_assembly_callback),
      &trampolines.assembly_callback));
  patches.push_back(
      make_city_entry_patch(0x4d3c5e0, city_decode("4c8bdc5553"),
                            reinterpret_cast<const void *>(&city_manor_cost),
                            &trampolines.manor_cost));
  patches.push_back(
      {0x4d30626, city_decode("498b8424e8020000"),
       city_manor_bridge(
           reinterpret_cast<const void *>(&city_enable_manor_instant),
           reinterpret_cast<const void *>(city_image + 0x4d3062e)),
       false});
  patches.push_back(make_city_entry_patch(
      0x4cf48b0, city_decode("40534883ec40"),
      reinterpret_cast<const void *>(&city_process_commands),
      &trampolines.process_commands));
  patches.push_back(
      make_city_entry_patch(0x4d2b340, city_decode("4055535657"),
                            reinterpret_cast<const void *>(&city_can_upgrade),
                            &trampolines.can_upgrade));
  patches.push_back(make_city_entry_patch(
      0x4d3a9a0, city_decode("48895c2418"),
      reinterpret_cast<const void *>(&city_upgrade_options),
      &trampolines.upgrade_options));
  return patches;
}
