#pragma once
#include "NativeContract.hpp"
#include <cstring>
namespace Wardrobe::AppearanceCompletion {
// The wrapper needs the component ABI and completion notification identity.
// It does not depend on original rebuild or notification implementations.
template<size_t N>
inline bool matches(const unsigned char* p,const std::array<unsigned char,N>& code,const std::array<unsigned char,N>& mask){
    for(size_t i=0;i<N;++i)if(mask[i]&&p[i]!=code[i])return false;return true;
}
inline bool executable(const unsigned char* p,size_t length,unsigned char* base,size_t size){
    auto at=reinterpret_cast<uintptr_t>(p),begin=reinterpret_cast<uintptr_t>(base);
    if(at<begin||length>size||at-begin>size-length)return false;
    MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(p,&m,sizeof(m))||m.State!=MEM_COMMIT||m.AllocationBase!=base||m.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;
    if(!(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    return at+length<=reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize;
}
inline const unsigned char* relative(const unsigned char* instruction){
    int32_t displacement{};std::memcpy(&displacement,instruction+1,4);
    return reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(instruction)+5+static_cast<int64_t>(displacement));
}
inline bool completion(NativeProbe::Function f,const NativeProbe::Image& image){
 using namespace NativeProbe;if(f.size<32)return false;
 constexpr std::array<unsigned char,3> retain{0x48,0x8b,0xd9};
 constexpr std::array<unsigned char,3> alternate{0x48,0x89,0xcb};
 auto at=find(f,retain);if(at==SIZE_MAX)at=find(f,alternate);
 if(at==SIZE_MAX||at+16>f.size||f.code[at+3]!=0xe8)return false;
 // Both rebuild calls consume the same component; the subsequent native
 // notification receives that component and its appearance-complete delegate.
 constexpr std::array<unsigned char,4> next{0x48,0x8b,0xcb,0xe8};
 auto second=find(f,next,at+8);if(second==SIZE_MAX||second+8>f.size)return false;
 constexpr std::array<unsigned char,10> notification{0x48,0x8d,0x8b,0x28,0x01,0,0,0x48,0x8b,0xd3};
 if(find(f,notification,second+8)==SIZE_MAX)return false;
 return image.entry(relative(f.code+at+3)).code&&image.entry(relative(f.code+second+3)).code;
}
inline bool dependencies(const unsigned char* p,unsigned char* base,size_t){NativeProbe::Image image(base);return completion(image.entry(p),image);}
inline void* resolve(std::wstring& error){return NativeProbe::resolve(completion,L"Appearance completion (component rebuild and completion notification)",error);}
}
