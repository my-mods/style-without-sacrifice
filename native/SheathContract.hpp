#pragma once
#include "AppearanceCompletion.hpp"
namespace Wardrobe::SheathContract {
// Validate the intercepted arguments and current reflected player binding.
using AppearanceCompletion::matches;
using AppearanceCompletion::executable;
inline const unsigned char* branch(const unsigned char* p,size_t displacement,size_t length){
    int32_t value{};std::memcpy(&value,p+displacement,4);
    return reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(p)+length+static_cast<int64_t>(value));
}
inline bool setter(NativeProbe::Function f,const NativeProbe::Image&,bool weapon){
 using namespace NativeProbe;if(f.size<32)return false;
 constexpr std::array<unsigned char,9> weaponArgs{0x49,0x8b,0xe8,0x48,0x8b,0xf9,0x48,0x85,0xd2};
 constexpr std::array<unsigned char,10> scabbardArgs{0x48,0x8b,0xf9,0x48,0x8b,0x89,0xf8,0,0,0};
 constexpr std::array<unsigned char,7> weaponField{0x48,0x8b,0x89,0xf0,0,0,0};
 constexpr std::array<unsigned char,2> meshWrite{0xff,0x90};
 constexpr std::array<unsigned char,5> scaleTail{0xf2,0x0f,0x10,0x4d,0x10};
 // Setter identity includes the mesh argument and destination component. For
 // weapon, R8 is a pointer to the 24-byte scale forwarded by our wrapper.
 return (weapon?has(f,weaponArgs)&&has(f,weaponField)&&has(f,scaleTail):has(f,scabbardArgs))&&has(f,meshWrite);
}
inline bool visibility(NativeProbe::Function f,const NativeProbe::Image&,bool weapon){
 using namespace NativeProbe;if(f.size<20)return false;
 auto field=std::array<unsigned char,7>{0x48,0x8b,0x89,static_cast<unsigned char>(weapon?0xf0:0xf8),0,0,0};
 constexpr std::array<unsigned char,8> forward{0x45,0x33,0xc0,0x48,0x8b,0x01,0xff,0x90};
 auto at=find(f,field);return at!=SIZE_MAX&&find(f,forward,at+field.size())!=SIZE_MAX;
}
inline bool weaponDependencies(const unsigned char* p,unsigned char* base,size_t){NativeProbe::Image image(base);return setter(image.entry(p),image,true);}
inline bool scabbardDependencies(const unsigned char* p,unsigned char* base,size_t){NativeProbe::Image image(base);return setter(image.entry(p),image,false);}
inline std::array<void*,4> resolve(std::wstring& error){
 std::array<void*,4> result{};
 result[0]=NativeProbe::resolve([](auto f,const auto& i){return setter(f,i,true);},L"Sheath weapon setter (mesh and scale arguments)",error);if(!result[0])return {};
 result[1]=NativeProbe::resolve([](auto f,const auto& i){return visibility(f,i,true);},L"Sheath weapon visibility (boolean forwarding)",error);if(!result[1])return {};
 result[2]=NativeProbe::resolve([](auto f,const auto& i){return setter(f,i,false);},L"Sheath scabbard setter (mesh argument)",error);if(!result[2])return {};
 result[3]=NativeProbe::resolve([](auto f,const auto& i){return visibility(f,i,false);},L"Sheath scabbard visibility (boolean forwarding)",error);if(!result[3])return {};
 return result;
}
// The interface's component-relative offsets are part of its argument ABI.
// Its offset within the pawn is derived from current reflected properties.
inline constexpr size_t interfaceOffset=0xbc0,weaponOffset=0xcb0,scabbardOffset=0xcb8; // historical fixture layout
inline size_t reflectedInterface(size_t objectSize,size_t weapon,size_t scabbard){
 if(weapon<0xf0||scabbard!=weapon+8||scabbard>objectSize||objectSize-scabbard<sizeof(void*))return 0;
 return weapon-0xf0;
}
inline bool interfaceMatches(void* pawn,size_t objectSize,size_t weapon,size_t scabbard,const std::array<void*,4>& targets){
 auto offset=reflectedInterface(objectSize,weapon,scabbard);
 if(!pawn||!offset||!targets[0]||!targets[1]||!targets[2]||!targets[3]||!NativeProbe::readable(static_cast<unsigned char*>(pawn)+offset,sizeof(void*)))return false;
 void** table{};std::memcpy(&table,static_cast<unsigned char*>(pawn)+offset,sizeof(table));
 if(!NativeProbe::readable(table,6*sizeof(void*)))return false;
 for(size_t i=0;i<4;++i)if(table[i+2]!=targets[i])return false;return true;
}
}
