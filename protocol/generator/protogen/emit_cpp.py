"""Emit firmware/uno/src/generated/cq_protocol.h (header-only, AVR-friendly)."""

from __future__ import annotations

from .model import EnumDef, Field, Frame, ProtocolModel
from .naming import k_name, pascal

CTYPE: dict[str, str] = {
    "u8": "uint8_t",
    "i8": "int8_t",
    "u16": "uint16_t",
    "i16": "int16_t",
    "u32": "uint32_t",
    "i32": "int32_t",
}
UTYPE: dict[str, str] = {
    "u8": "uint8_t",
    "i8": "uint8_t",
    "u16": "uint16_t",
    "i16": "uint16_t",
    "u32": "uint32_t",
    "i32": "uint32_t",
}
SUFFIX: dict[str, str] = {
    "u8": "u8",
    "i8": "u8",
    "u16": "u16",
    "i16": "u16",
    "u32": "u32",
    "i32": "u32",
}
WIRE_ENUM: dict[str, str] = {
    "u8": "kU8",
    "i8": "kI8",
    "u16": "kU16",
    "i16": "kI16",
    "u32": "kU32",
    "i32": "kI32",
}


def cpp_literal(value: int, wire: str) -> str:
    if wire == "i32" and value == -0x80000000:
        return "(-2147483647L - 1)"
    if wire == "u32":
        return f"{value}UL"
    if wire == "i32":
        return f"{value}L"
    return str(value)


def _enum(e: EnumDef) -> list[str]:
    lines = [f"enum class {e.name} : uint8_t {{"]
    lines += [f"  {k_name(k)} = {v}," for k, v in e.values.items()]
    lines += ["};", ""]
    return lines


def _item_struct(fr: Frame, f: Field) -> list[str]:
    assert f.items is not None
    name = f"{pascal(fr.name)}{pascal(f.name)}Item"
    lines = [f"struct {name} {{"]
    lines += [f"  {CTYPE[i.type]} {i.name};" for i in f.items]
    lines += ["};", ""]
    return lines


def _struct(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    lines: list[str] = []
    for f in fr.fields:
        if f.items is not None:
            lines += _item_struct(fr, f)
    lines += [
        f"struct {cls} {{",
        f"  static constexpr FrameType kType = FrameType::{k_name(fr.name)};",
        f"  static constexpr uint8_t kFixedSize = {fr.fixed_size()};",
        f"  static constexpr uint8_t kMaxSize = {fr.max_payload()};",
    ]
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  {pascal(fr.name)}{pascal(f.name)}Item {f.name}[{f.max_count}];")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  {CTYPE[f.type]} {f.name}[{f.count}];")
        else:
            assert f.type is not None
            lines.append(f"  {CTYPE[f.type]} {f.name};")
    lines += ["};", ""]
    return lines


def _put(wire: str, expr: str) -> str:
    return f"put_{SUFFIX[wire]}(p, static_cast<{UTYPE[wire]}>({expr}));"


def _get(wire: str, target: str) -> str:
    return f"{target} = static_cast<{CTYPE[wire]}>(get_{SUFFIX[wire]}(p));"


def _encode(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    if not fr.fields:
        return [
            f"inline uint8_t encode(const {cls}& m, uint8_t* out) {{",
            "  (void)m;",
            "  (void)out;",
            "  return 0;",
            "}",
            "",
        ]
    lines = [f"inline uint8_t encode(const {cls}& m, uint8_t* out) {{"]
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  if (m.{f.count_field} > {f.max_count}) {{ return kEncodeError; }}")
    lines.append("  uint8_t* p = out;")
    for f in fr.fields:
        if f.items is not None:
            lines.append(f"  for (uint8_t i = 0; i < m.{f.count_field}; ++i) {{")
            lines += [f"    {_put(it.type, f'm.{f.name}[i].{it.name}')}" for it in f.items]
            lines.append("  }")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  for (uint8_t i = 0; i < {f.count}; ++i) {{")
            lines.append(f"    {_put(f.type, f'm.{f.name}[i]')}")
            lines.append("  }")
        else:
            assert f.type is not None
            lines.append(f"  {_put(f.type, f'm.{f.name}')}")
    lines += ["  return static_cast<uint8_t>(p - out);", "}", ""]
    return lines


def _decode(fr: Frame) -> list[str]:
    cls = pascal(fr.name) + "Msg"
    if not fr.fields:
        return [
            f"inline bool decode(const uint8_t* in, uint8_t len, {cls}& m) {{",
            "  (void)in;",
            "  (void)m;",
            "  return len == 0;",
            "}",
            "",
        ]
    variable = any(f.items is not None for f in fr.fields)
    check = f"len < {cls}::kFixedSize" if variable else f"len != {cls}::kFixedSize"
    lines = [
        f"inline bool decode(const uint8_t* in, uint8_t len, {cls}& m) {{",
        f"  if ({check}) {{ return false; }}",
        "  const uint8_t* p = in;",
    ]
    for f in fr.fields:
        if f.items is not None:
            assert f.max_count is not None
            size = f.item_size()
            lines.append(
                f"  if (m.{f.count_field} > {f.max_count} || "
                f"len != {cls}::kFixedSize + m.{f.count_field} * {size}) {{ return false; }}"
            )
            lines.append(f"  for (uint8_t i = 0; i < m.{f.count_field}; ++i) {{")
            lines += [f"    {_get(it.type, f'm.{f.name}[i].{it.name}')}" for it in f.items]
            lines.append("  }")
        elif f.count is not None:
            assert f.type is not None
            lines.append(f"  for (uint8_t i = 0; i < {f.count}; ++i) {{")
            lines.append(f"    {_get(f.type, f'm.{f.name}[i]')}")
            lines.append("  }")
        else:
            assert f.type is not None
            lines.append(f"  {_get(f.type, f'm.{f.name}')}")
    lines += ["  return true;", "}", ""]
    return lines


RUNTIME = """\
inline void put_u8(uint8_t*& p, uint8_t v) { *p++ = v; }
inline void put_u16(uint8_t*& p, uint16_t v) {
  *p++ = static_cast<uint8_t>(v);
  *p++ = static_cast<uint8_t>(v >> 8);
}
inline void put_u32(uint8_t*& p, uint32_t v) {
  put_u16(p, static_cast<uint16_t>(v));
  put_u16(p, static_cast<uint16_t>(v >> 16));
}
inline uint8_t get_u8(const uint8_t*& p) { return *p++; }
inline uint16_t get_u16(const uint8_t*& p) {
  const uint16_t v = static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
  p += 2;
  return v;
}
inline uint32_t get_u32(const uint8_t*& p) {
  const uint32_t lo = get_u16(p);
  const uint32_t hi = get_u16(p);
  return lo | (hi << 16);
}
inline uint8_t crc8(const uint8_t* data, uint8_t len, uint8_t crc = 0x00) {
  for (uint8_t i = 0; i < len; ++i) {
    crc = static_cast<uint8_t>(crc ^ data[i]);
    for (uint8_t b = 0; b < 8; ++b) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}
"""


def emit_cpp(model: ProtocolModel) -> str:
    m = model.meta
    lines = [
        "// DO NOT EDIT - generated by protogen from protocol/defs",
        "#pragma once",
        "#include <stdint.h>",
        "#ifdef __AVR__",
        "#include <avr/pgmspace.h>",
        "#else",
        "#ifndef PROGMEM",
        "#define PROGMEM",
        "#endif",
        "#endif",
        "",
        "namespace cq {",
        "namespace proto {",
        "",
        f"constexpr uint8_t kProtocolVersion = {m.protocol_version};",
        f"constexpr uint8_t kSync = 0x{m.sync:02X};",
        f"constexpr uint8_t kMaxPayload = {m.max_payload};",
        f"constexpr uint32_t kBaud = {m.baud}UL;",
        "constexpr uint8_t kEncodeError = 0xFF;",
        "",
        RUNTIME,
        "enum class FrameType : uint8_t {",
    ]
    lines += [f"  {k_name(fr.name)} = 0x{fr.type:02X}," for fr in model.frames]
    lines += ["};", ""]
    for e in model.enums:
        lines += _enum(e)
    lines += _enum(EnumDef(name="EventKind", values={ev.name: ev.kind for ev in model.events}))
    lines += _enum(EnumDef(name="SettingId", values={s.name.upper(): s.id for s in model.settings}))
    lines += _enum(EnumDef(name="AckStatus", values={r: i for i, r in enumerate(model.ack_status)}))
    for fr in model.frames:
        lines += _struct(fr)
        lines += _encode(fr)
        lines += _decode(fr)
    codes = {e.reason: e.code for d in model.errors for e in d.codes}
    ack_codes = ", ".join(str(codes.get(r, 0)) for r in model.ack_status)
    lines += [
        "enum class SettingScope : uint8_t { kGlobal = 0, kProfile = 1 };",
        "enum class WireType : uint8_t { kU8, kI8, kU16, kI16, kU32, kI32 };",
        "struct SettingDescriptor {",
        "  uint8_t id;",
        "  SettingScope scope;",
        "  WireType type;",
        "  int32_t min;",
        "  int32_t max;",
        "  int32_t def;",
        "};",
        "inline const SettingDescriptor kSettingDescriptors[] PROGMEM = {",
    ]
    for s in model.settings:
        scope = "SettingScope::kProfile" if s.scope == "profile" else "SettingScope::kGlobal"
        lines.append(
            f"  {{{s.id}, {scope}, WireType::{WIRE_ENUM[s.type]}, {cpp_literal(s.min, 'i32')}, "
            f"{cpp_literal(s.max, 'i32')}, {cpp_literal(s.default, 'i32')}}},"
        )
    lines += [
        "};",
        f"constexpr uint8_t kSettingCount = {len(model.settings)};",
        "",
        f"inline const uint16_t kAckStatusCode[] PROGMEM = {{{ack_codes}}};",
        "",
        "}  // namespace proto",
        "}  // namespace cq",
        "",
    ]
    return "\n".join(lines)
