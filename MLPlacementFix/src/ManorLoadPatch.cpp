#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ManorLoadPatch.hpp"

static auto append_manor_scope_frame(std::vector<std::uint8_t> &code,
                                     bool restore) -> void {
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
}

static auto append_manor_scope_target(std::vector<std::uint8_t> &code,
                                      const void *target, bool call) -> void {
  if (call)
    code.insert(code.end(), {0x48, 0xb8});
  else
    code.insert(code.end(), {0xff, 0x25, 0, 0, 0, 0});
  const auto address = reinterpret_cast<std::uintptr_t>(target);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  if (call)
    code.insert(code.end(), {0xff, 0xd0});
}

auto manor_load_begin_bridge(const void *callback, const void *continuation)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || continuation == nullptr)
    return {};
  // Replay the two original loads. A null plan retains these exact values.
  std::vector<std::uint8_t> code{0x4d, 0x8b, 0xae, 0x98, 0x03, 0, 0,
                                 0x49, 0x63, 0x86, 0xa0, 0x03, 0, 0};
  append_manor_scope_frame(code, false);
  // 0.8.117: begin(master=RSI, manor=RDI, savedBuilding=R14).
  code.insert(code.end(),
              {0x48, 0x89, 0xf1, 0x48, 0x89, 0xfa, 0x4d, 0x89, 0xf0});
  append_manor_scope_target(code, callback, true);
  code.insert(code.end(), {0x48, 0x85, 0xc0, 0x74, 0x17});
  // The callback returns a validated 16-byte array header. Override only the
  // saved R13/data and RAX/count slots; native code computes its own end/R15.
  code.insert(code.end(), {0x48, 0x8b, 0x08, 0x48, 0x63, 0x50, 0x08, 0x48,
                           0x89, 0x8c, 0x24, 0x40, 0x01, 0,    0,    0x48,
                           0x89, 0x94, 0x24, 0xa0, 0x01, 0,    0});
  append_manor_scope_frame(code, true);
  append_manor_scope_target(code, continuation, false);
  return code;
}

auto manor_load_end_bridge(const void *callback, const void *continuation)
    -> std::vector<std::uint8_t> {
  if (callback == nullptr || continuation == nullptr)
    return {};
  std::vector<std::uint8_t> code;
  append_manor_scope_frame(code, false);
  code.insert(code.end(), {0x48, 0x89, 0xf9}); // end(manor=RDI).
  append_manor_scope_target(code, callback, true);
  append_manor_scope_frame(code, true);
  // 0x4c9340a is reached by both empty and nonempty module loops.
  code.insert(code.end(), {0x48, 0x8d, 0x95, 0x48, 0x02, 0, 0});
  append_manor_scope_target(code, continuation, false);
  return code;
}

auto manor_load_bridge(const void *callback, const void *direct,
                       const void *assembly) -> std::vector<std::uint8_t> {
  if (callback == nullptr || direct == nullptr || assembly == nullptr)
    return {};

  std::vector<std::uint8_t> code;
  const auto append32 = [&](std::int32_t value) {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    code.insert(code.end(), bytes, bytes + sizeof(value));
  };
  const auto jump = [&](const void *destination) {
    code.insert(code.end(), {0xff, 0x25, 0, 0, 0, 0});
    const auto address = reinterpret_cast<std::uintptr_t>(destination);
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
    code.insert(code.end(), bytes, bytes + sizeof(address));
  };
  const auto vectors = [&](bool load) {
    for (unsigned reg{}; reg < 16; ++reg) {
      code.push_back(0xf3);
      if (reg >= 8)
        code.push_back(0x44);
      code.insert(code.end(),
                  {0x0f, static_cast<std::uint8_t>(load ? 0x6f : 0x7f),
                   static_cast<std::uint8_t>(0x84 + (reg % 8) * 8), 0x24});
      append32(static_cast<std::int32_t>(0x20 + reg * 16));
    }
  };
  const auto restore = [&] {
    vectors(true);
    // MXCSR and x87 control follow the sixteen saved XMM registers.
    code.insert(code.end(), {0x0f, 0xae, 0x94, 0x24, 0x20, 0x01, 0, 0, 0xd9,
                             0xac, 0x24, 0x24, 0x01, 0, 0});
    // LEA and POP preserve the original CMP flags until POPFQ restores them.
    code.insert(code.end(), {0x48, 0x8d, 0xa4, 0x24, 0x30, 0x01, 0, 0});
    for (unsigned reg = 16; reg-- > 8;)
      code.insert(code.end(),
                  {0x41, static_cast<std::uint8_t>(0x58 + reg % 8)});
    code.insert(code.end(), {0x5f, 0x5e, 0x5d, 0x5b, 0x5a, 0x59, 0x58, 0x9d});
  };

  // 0.8.117 RVA 0x4c931c7 replaces CMP [RBX],R14; JE 0x4c93298.
  // R13 is the saved 0xb0-byte record, RDI is its manor, RAX is the
  // definition row, and RBX is that row's assembly path at +0x180.
  code.insert(code.end(), {0x4c, 0x39, 0x33, 0x9c, 0x50, 0x51, 0x52, 0x53, 0x55,
                           0x56, 0x57});
  for (unsigned reg = 8; reg < 16; ++reg)
    code.insert(code.end(), {0x41, static_cast<std::uint8_t>(0x50 + reg % 8)});
  // Native midpoint RSP%16==0. Flags plus fifteen GPRs consume 0x80;
  // the 0x130 frame reserves separate 0x20-byte Windows call shadow space.
  code.insert(code.end(), {0x48, 0x81, 0xec, 0x30, 0x01, 0, 0});
  vectors(false);
  code.insert(code.end(), {0x0f, 0xae, 0x9c, 0x24, 0x20, 0x01, 0, 0, 0xd9, 0xbc,
                           0x24, 0x24, 0x01, 0, 0});
  code.insert(code.end(), {0x4c, 0x89, 0xe9, 0x48, 0x89, 0xfa, 0x49, 0x89, 0xc0,
                           0x48, 0xb8});
  const auto address = reinterpret_cast<std::uintptr_t>(callback);
  const auto *bytes = reinterpret_cast<const std::uint8_t *>(&address);
  code.insert(code.end(), bytes, bytes + sizeof(address));
  code.insert(code.end(), {0xff, 0xd0, 0x84, 0xc0, 0x0f, 0x85});
  const auto eligible_branch = code.size();
  append32(0);

  restore();
  // Ineligible records retain the actual native comparison against R14,
  // including its flags. JE skips the fourteen-byte absolute assembly jump.
  code.insert(code.end(), {0x74, 0x0e});
  jump(assembly);
  jump(direct);

  const auto eligible = code.size();
  restore();
  jump(direct);
  const auto displacement = static_cast<std::int32_t>(
      eligible - eligible_branch - sizeof(std::int32_t));
  std::memcpy(code.data() + eligible_branch, &displacement,
              sizeof(displacement));
  return code;
}
