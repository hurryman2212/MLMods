#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "Patches.hpp"

inline auto make_road_rule_bridge(const void *callback, const void *fallthrough,
                                  const void *blocked)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || fallthrough == nullptr || blocked == nullptr)
    return {};
  std::vector<std::uint8_t> code;
  // RoadTick's midpoint has RSP%16==0. Flags plus fifteen GPRs occupy0x80;
  // the0x130 frame contains independent shadow space and all vector/control
  // state. The noexcept predicate receives the original RDI pawn.
  const auto frame = [&](bool restore) {
    if (!restore) {
      code.insert(code.end(), {0x9c, 0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});
      for (unsigned reg = 8; reg < 16; ++reg)
        code.insert(code.end(),
                    {0x41, static_cast<std::uint8_t>(0x50 + reg % 8)});
      code.insert(code.end(), {0x48, 0x81, 0xec, 0x30, 0x01, 0, 0});
    }
    for (unsigned reg{}; reg < 16; ++reg) {
      code.push_back(0xf3);
      if (reg >= 8)
        code.push_back(0x44);
      code.insert(code.end(),
                  {0x0f, static_cast<std::uint8_t>(restore ? 0x6f : 0x7f),
                   static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
      const auto offset = static_cast<std::int32_t>(0x20 + reg * 16);
      const auto *bytes = reinterpret_cast<const std::uint8_t *>(&offset);
      code.insert(code.end(), bytes, bytes + sizeof(offset));
    }
    code.insert(code.end(),
                {0x0f, 0xae, static_cast<std::uint8_t>(restore ? 0x94 : 0x9c),
                 0x24, 0x20, 0x01, 0, 0, 0xd9,
                 static_cast<std::uint8_t>(restore ? 0xac : 0xbc), 0x24, 0x24,
                 0x01, 0, 0});
    if (restore) {
      code.insert(code.end(), {0x48, 0x8d, 0xa4, 0x24, 0x30, 0x01, 0, 0});
      for (unsigned reg = 16; reg-- > 8;)
        code.insert(code.end(),
                    {0x41, static_cast<std::uint8_t>(0x58 + reg % 8)});
      code.insert(code.end(), {0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d});
    }
  };
  frame(false);
  code.insert(code.end(), {0x48, 0x89, 0xf9, 0x48, 0xb8});
  const auto target = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&target);
  code.insert(code.end(), bytes, bytes + sizeof(target));
  code.insert(code.end(),
              {0xff, 0xd0, 0x80, 0xbc, 0x24, 0x48, 0x01, 0, 0, 0, 0x0f, 0x85});
  const auto unexpected_register = code.size();
  code.insert(code.end(), {0, 0, 0, 0, 0x84, 0xc0, 0x0f, 0x85});
  const auto conditional = code.size();
  code.insert(code.end(), {0, 0, 0, 0});
  const auto original = code.size();
  frame(true);
  // Original comparison at RVA0x4b68dfa; its flags are consumed by JNE.
  code.insert(code.end(), {0x44, 0x38, 0xa7, 0x48, 0x08, 0, 0, 0x75, 0x0e});
  append_absolute_jump(code, fallthrough);
  append_absolute_jump(code, blocked);

  const auto eligible = code.size();
  frame(true);
  // Native R12B is zeroed at0x4b68be6 before this collision loop. Produce the
  // same flags as ordinary CastleEdit==0 without changing the stored flag,
  // registers, or the later independent gate-regeneration branch.
  code.insert(code.end(), {0x45, 0x38, 0xe4}); // CMP R12B,R12B.
  append_absolute_jump(code, fallthrough);
  const auto displacement =
      static_cast<std::int32_t>(eligible - conditional - 4);
  std::memcpy(code.data() + conditional, &displacement, sizeof(displacement));
  const auto register_fallback =
      static_cast<std::int32_t>(original - unexpected_register - 4);
  std::memcpy(code.data() + unexpected_register, &register_fallback,
              sizeof(register_fallback));
  return code;
}
