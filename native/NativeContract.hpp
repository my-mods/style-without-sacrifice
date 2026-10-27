#pragma once
// Scoped argument and data-flow validation for the clothing row accessor.
#include <Unreal/Core/Windows/AllowWindowsPlatformTypes.hpp>
#include <Windows.h>
#include <array>
#include "NativeProbe.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
namespace Wardrobe::NativeContract {
template<size_t N,class Validator>
inline void* resolveCode(const std::array<unsigned char,N>& expected,const std::array<unsigned char,N>& fixed,Validator validate,std::wstring& error){
 auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
 if(!base){error=L"game image unavailable";return nullptr;}
 auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
 if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>1048576){error=L"invalid executable header";return nullptr;}
 auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
 if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC){error=L"expected a 64-bit PE image";return nullptr;}
 if(!nt->FileHeader.NumberOfSections||nt->FileHeader.NumberOfSections>96){error=L"invalid section table";return nullptr;}
 auto section=IMAGE_FIRST_SECTION(nt);unsigned char* match=nullptr;
 for(unsigned s=0;s<nt->FileHeader.NumberOfSections;++s){
  if(!(section[s].Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
  size_t begin=section[s].VirtualAddress,size=section[s].Misc.VirtualSize;
  if(begin>=nt->OptionalHeader.SizeOfImage||size>nt->OptionalHeader.SizeOfImage-begin||size<N)continue;
  for(size_t i=0;i+N<=size;){
   auto start=base+begin+i;MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery(start,&m,sizeof(m))||!m.RegionSize)break;
   size_t end=std::min<size_t>(size,static_cast<unsigned char*>(m.BaseAddress)+m.RegionSize-(base+begin));
   if(end<=i)break;
   if(m.State==MEM_COMMIT&&m.AllocationBase==base&&!(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))){
    for(;i+N<=end;++i){auto p=base+begin+i;if(p[0]!=expected[0]||p[1]!=expected[1]||p[2]!=expected[2])continue;
     bool same=true;for(size_t k=0;k<N;++k)if(fixed[k]&&p[k]!=expected[k]){same=false;break;}
     if(same&&validate(p,base,nt->OptionalHeader.SizeOfImage)){if(match){error=L"native instruction contract is ambiguous";return nullptr;}match=p;}
    }
   }
   i=end;
  }
 }
 if(!match)error=L"required native instructions changed; hook not installed";
 return match;
}
inline bool accessor(NativeProbe::Function f,const NativeProbe::Image& image){
 using namespace NativeProbe;
 if(f.size<32)return false;
 // RCX item is retained while the context in RDX is passed to the subsystem
 // lookup. The result is a borrowed row pointer, including item+0x100 fallback.
 constexpr std::array<unsigned char,6> args{0x48,0x8b,0xf9,0x48,0x8b,0xca};
 constexpr std::array<unsigned char,6> argsAlternate{0x48,0x89,0xcf,0x48,0x89,0xd1};
 auto at=find(f,args);if(at==SIZE_MAX)at=find(f,argsAlternate);
 if(at==SIZE_MAX||at+11>f.size||f.code[at+6]!=0xe8||!image.entry(relative(f.code+at+6)).code)return false;
 constexpr std::array<unsigned char,7> fallback{0x48,0x8b,0x87,0x00,0x01,0x00,0x00};
 constexpr std::array<unsigned char,7> contextMap{0x48,0x8d,0xb0,0x30,0x01,0x00,0x00};
 constexpr std::array<unsigned char,5> itemKey{0x48,0x89,0x7c,0x24,0x48};
 return has(f,fallback)&&has(f,contextMap)&&has(f,itemKey);
}
inline void* resolve(std::wstring& error){return NativeProbe::resolve(accessor,L"Clothing row accessor (item/context arguments and borrowed row return)",error);}
}
