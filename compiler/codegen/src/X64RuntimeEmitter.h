#pragma once
// B-2 codegen - runtime x86-64 byte emitter for T2 lowering.
// Minimal, correct, no disassembler, no external deps. RBP = activation base.
#include <cstdint>
#include <cstring>
#include <vector>

namespace b2::codegen {

enum class Reg : std::uint8_t {
  RAX = 0, RCX = 1, RDX = 2, RSI = 6, RDI = 7,
  R8 = 8, R9 = 9, R10 = 10, R11 = 11,
};
enum class Reg32 : std::uint8_t {
  EAX = 0, ECX = 1, EDX = 2, ESI = 6, EDI = 7,
  R8D = 8, R9D = 9, R10D = 10, R11D = 11,
};

class X64RuntimeEmitter {
public:
  std::vector<std::uint8_t> buf;
  [[nodiscard]] std::uint32_t offset() const noexcept { return static_cast<std::uint32_t>(buf.size()); }
  void byte(std::uint8_t b) { buf.push_back(b); }
  void bytes(std::initializer_list<std::uint8_t> bs) { for (auto b : bs) buf.push_back(b); }
  void rex(bool w, bool r=false, bool x=false, bool b=false) {
    const std::uint8_t v = 0x40 | (w?8:0) | (r?4:0) | (x?2:0) | (b?1:0);
    if (v != 0x40) byte(v);
  }
  void modrm(std::uint8_t mod, std::uint8_t reg, std::uint8_t rm) {
    byte(static_cast<std::uint8_t>((mod<<6)|((reg&7)<<3)|(rm&7)));
  }
  void imm32(std::int32_t v) { std::uint32_t u; std::memcpy(&u,&v,4); byte(u&0xFF); byte((u>>8)&0xFF); byte((u>>16)&0xFF); byte((u>>24)&0xFF); }
  void imm64(std::int64_t v) { std::uint64_t u; std::memcpy(&u,&v,8); for(int i=0;i<8;++i) byte((u>>(i*8))&0xFF); }
  void loadRbpDisp32(Reg32 dst, std::int32_t disp) { rex(false,false,false,dst>=Reg32::R8D); byte(0x8B); modrm(2,static_cast<std::uint8_t>(dst)&7,5); imm32(disp); }
  void storeRbpDisp32(Reg32 src, std::int32_t disp) { rex(false,false,false,src>=Reg32::R8D); byte(0x89); modrm(2,static_cast<std::uint8_t>(src)&7,5); imm32(disp); }
  void loadRbpDisp64(Reg dst, std::int32_t disp) { rex(true,false,false,dst>=Reg::R8); byte(0x8B); modrm(2,static_cast<std::uint8_t>(dst)&7,5); imm32(disp); }
  void storeRbpDisp64(Reg src, std::int32_t disp) { rex(true,false,false,src>=Reg::R8); byte(0x89); modrm(2,static_cast<std::uint8_t>(src)&7,5); imm32(disp); }
  void addEaxEcx()  { byte(0x01); modrm(3,1,0); }
  void subEaxEcx()  { byte(0x29); modrm(3,1,0); }
  void andEaxEcx()  { byte(0x21); modrm(3,1,0); }
  void orEaxEcx()   { byte(0x09); modrm(3,1,0); }
  void xorEaxEcx()  { byte(0x31); modrm(3,1,0); }
  void imulEaxEcx() { bytes({0x0F,0xAF}); modrm(3,1,0); }
  void idivEcx()    { byte(0x99); byte(0xF7); modrm(3,7,1); }
  void movEaxEdx()  { byte(0x89); modrm(3,2,0); } // mov eax,edx (remainder)
  void shlEaxCl()   { byte(0xD3); modrm(3,4,0); }
  void shrEaxCl()   { byte(0xD3); modrm(3,5,0); }
  void sarEaxCl()   { byte(0xD3); modrm(3,7,0); }
  void andEcxImm31() { byte(0x83); modrm(3,4,1); byte(0x1F); } // and ecx,0x1F
  void negEax()     { byte(0xF7); modrm(3,3,0); }
  void cmpEaxEcx()  { byte(0x39); modrm(3,1,0); }
  void testEaxEax() { byte(0x85); modrm(3,0,0); }
  void setccAl(std::uint8_t cc) { bytes({0x0F,cc}); modrm(3,0,0); }
  void movzxEaxAl() { bytes({0x0F,0xB6}); modrm(3,0,0); }
  void addRaxRcx()  { rex(true); byte(0x01); modrm(3,1,0); }
  void subRaxRcx()  { rex(true); byte(0x29); modrm(3,1,0); }
  void imulRaxRcx() { rex(true); bytes({0x0F,0xAF}); modrm(3,1,0); }
  void andRaxRcx()  { rex(true); byte(0x21); modrm(3,1,0); }
  void orRaxRcx()   { rex(true); byte(0x09); modrm(3,1,0); }
  void xorRaxRcx()  { rex(true); byte(0x31); modrm(3,1,0); }
  void negRax()     { rex(true); byte(0xF7); modrm(3,3,0); }
  void cmpRaxRcx()  { rex(true); byte(0x39); modrm(3,1,0); }
  void movsxdRaxMem(int disp) { rex(true); byte(0x63); modrm(2,0,5); imm32(disp); }
  void movsxEaxAl() { bytes({0x0F,0xBE}); modrm(3,0,0); }
  void movzxEaxAx() { bytes({0x0F,0xB7}); modrm(3,0,0); }
  void movsxEaxAx() { bytes({0x0F,0xBF}); modrm(3,0,0); }
  void movEaxImm32(std::int32_t v) { byte(0xB8); imm32(v); }
  void movRaxImm64(std::int64_t v) { rex(true); byte(0xB8); imm64(v); }
  void movEcxImm32(std::int32_t v) { byte(0xB9); imm32(v); }
  void xorEaxEax() { byte(0x31); modrm(3,0,0); }
  std::uint32_t jmpRel32() { byte(0xE9); const std::uint32_t p=offset(); imm32(0); return p; }
  std::uint32_t jccRel32(std::uint8_t cc) { bytes({0x0F,cc}); const std::uint32_t p=offset(); imm32(0); return p; }
  void patchRel32(std::uint32_t patchOff, std::uint32_t targetOff) {
    const std::int32_t rel = static_cast<std::int32_t>(targetOff)-static_cast<std::int32_t>(patchOff+4);
    std::uint32_t u; std::memcpy(&u,&rel,4);
    buf[patchOff]=u&0xFF; buf[patchOff+1]=(u>>8)&0xFF; buf[patchOff+2]=(u>>16)&0xFF; buf[patchOff+3]=(u>>24)&0xFF;
  }
  void prologue() { byte(0x55); bytes({0x48,0x89,0xFD}); } // push rbp; mov rbp,rdi
  // WHY: pop rbp (NOT leave!). RBP = activation pointer, not stack frame.
  // leave = mov rsp,rbp; pop rbp — would corrupt RSP. We just restore RBP.
  void epilogueNormal() { xorEaxEax(); byte(0x5D); byte(0xC3); } // xor eax,eax; pop rbp; ret
  void epilogueDeopt()  { byte(0xB8); imm32(1); byte(0x5D); byte(0xC3); } // mov eax,1; pop rbp; ret
  std::uint32_t callRip32() { byte(0xFF); modrm(0,2,5); const std::uint32_t p=offset(); imm32(0); return p; }
  void patchCallAbs(std::uint32_t patchOff, void* target) {
    const std::uintptr_t ripAfter = reinterpret_cast<std::uintptr_t>(buf.data())+patchOff+4;
    const std::uintptr_t tgt = reinterpret_cast<std::uintptr_t>(target);
    const std::int32_t rel = static_cast<std::int32_t>(tgt-ripAfter);
    std::uint32_t u; std::memcpy(&u,&rel,4);
    buf[patchOff]=u&0xFF; buf[patchOff+1]=(u>>8)&0xFF; buf[patchOff+2]=(u>>16)&0xFF; buf[patchOff+3]=(u>>24)&0xFF;
  }
  void movRdiRbp()    { bytes({0x48,0x89,0xEF}); }
  void movEsiImm32(std::uint32_t v)  { byte(0xBE); imm32(static_cast<std::int32_t>(v)); }
  void movEdxImm32(std::uint32_t v)  { byte(0xBA); imm32(static_cast<std::int32_t>(v)); }
  void movEcxImm32u(std::uint32_t v) { byte(0xB9); imm32(static_cast<std::int32_t>(v)); }
  void movR8dImm32(std::uint32_t v)  { bytes({0x41,0xB8}); imm32(static_cast<std::int32_t>(v)); }
};

} // namespace b2::codegen
