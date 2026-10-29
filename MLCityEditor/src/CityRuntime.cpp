#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <Windows.h>

#include "CityRuntime.hpp"

std::uintptr_t city_image{};

auto city_read(std::uintptr_t address, void *output, std::size_t size) noexcept
    -> bool {
  SIZE_T copied{};
  return address != 0 && output != nullptr && size != 0 &&
         ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<void *>(address), output, size,
                           &copied) != 0 &&
         copied == size;
}

auto city_valid_object(void *object) noexcept -> bool {
  if (city_image == 0 || object == nullptr ||
      reinterpret_cast<std::uintptr_t>(object) % alignof(void *) != 0) {
    return false;
  }
  std::array<std::uint8_t, 24> header{};
  if (!city_read(reinterpret_cast<std::uintptr_t>(object), header.data(),
                 header.size())) {
    return false;
  }
  std::uintptr_t vtable{}, object_class{}, class_vtable{};
  std::uint32_t flags{};
  std::int32_t index{};
  std::memcpy(&vtable, header.data(), sizeof(vtable));
  std::memcpy(&flags, header.data() + 8, sizeof(flags));
  std::memcpy(&index, header.data() + 12, sizeof(index));
  std::memcpy(&object_class, header.data() + 16, sizeof(object_class));
  return vtable >= city_image && vtable < city_image + 0x09dfd000 &&
         (flags & 0x40018000U) == 0 && index > 0 &&
         city_read(object_class, &class_vtable, sizeof(class_vtable)) &&
         class_vtable >= city_image && class_vtable < city_image + 0x09dfd000;
}

auto city_player_pawn(void *pawn) noexcept -> bool {
  std::array<std::uint8_t, 2> flags{};
  return city_valid_object(pawn) &&
         city_read(reinterpret_cast<std::uintptr_t>(pawn) + 0x34c, flags.data(),
                   flags.size()) &&
         flags[0] == 1 && flags[1] == 0;
}

auto city_building_owner(void *building) noexcept -> void * {
  void *owner{};
  if (!city_valid_object(building) ||
      !city_read(reinterpret_cast<std::uintptr_t>(building) + 0x2e0, &owner,
                 sizeof(owner))) {
    return nullptr;
  }
  return owner;
}

auto city_player_building(void *building, void *expected_pawn) noexcept
    -> bool {
  void *owner = city_building_owner(building);
  return owner != nullptr &&
         (expected_pawn == nullptr || owner == expected_pawn) &&
         city_player_pawn(owner);
}

auto city_edited_manor(void *building) noexcept -> bool {
  void *owner = city_building_owner(building);
  void *edited{};
  std::int32_t type{};
  return city_player_pawn(owner) &&
         city_read(reinterpret_cast<std::uintptr_t>(building) + 0x3b8, &type,
                   sizeof(type)) &&
         type == 98 &&
         city_read(reinterpret_cast<std::uintptr_t>(owner) + 0x840, &edited,
                   sizeof(edited)) &&
         edited == building;
}
