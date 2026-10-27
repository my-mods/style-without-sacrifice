#pragma once
// Function-boundary discovery and small argument/data-flow anchors. Original
// callees, cleanup code and exact function lengths are not mod dependencies.
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include <hde64.h>
}
namespace Wardrobe::NativeProbe {
inline bool readable(const void* p,size_t n,bool code=false){
 auto at=reinterpret_cast<uintptr_t>(p);if(!at||!n||at>UINTPTR_MAX-n)return false;
 const auto end=at+n;
 while(at<end){MEMORY_BASIC_INFORMATION m{};
  if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m))||m.State!=MEM_COMMIT||(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))||!(m.Protect&0xee))return false;
  if(code&&!(m.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
  auto begin=reinterpret_cast<uintptr_t>(m.BaseAddress);if(at<begin||at-begin>=m.RegionSize)return false;
  at+=std::min<size_t>(end-at,m.RegionSize-(at-begin));
 }return true;
}
struct Function {const unsigned char* code{};size_t size{};};
struct Image {
 unsigned char* base{};size_t size{};const RUNTIME_FUNCTION* functions{};size_t count{};
 std::vector<std::pair<uintptr_t,uintptr_t>> executableRanges;
 bool contains(const void* p,size_t n)const{auto a=reinterpret_cast<uintptr_t>(p),b=reinterpret_cast<uintptr_t>(base);return n<=size&&a>=b&&a-b<=size-n;}
 explicit Image(unsigned char* p=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr))):base(p){
  if(!readable(p,sizeof(IMAGE_DOS_HEADER)))return;auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(p);
  if(dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>1048576||!readable(p+dos->e_lfanew,sizeof(IMAGE_NT_HEADERS64)))return;
  auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(p+dos->e_lfanew);
  if(nt->Signature!=IMAGE_NT_SIGNATURE||nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64||nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC)return;
  size=nt->OptionalHeader.SizeOfImage;auto d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  if(d.Size%sizeof(RUNTIME_FUNCTION)||!contains(p+d.VirtualAddress,d.Size)||!readable(p+d.VirtualAddress,d.Size))return;
  functions=reinterpret_cast<RUNTIME_FUNCTION*>(p+d.VirtualAddress);count=d.Size/sizeof(RUNTIME_FUNCTION);
  for(size_t at=0;at<size;){MEMORY_BASIC_INFORMATION m{};if(!VirtualQuery(p+at,&m,sizeof(m))||!m.RegionSize)break;auto begin=reinterpret_cast<uintptr_t>(m.BaseAddress),here=reinterpret_cast<uintptr_t>(p+at);if(here<begin||here-begin>=m.RegionSize)break;auto length=std::min<size_t>(size-at,m.RegionSize-(here-begin));
   if(m.State==MEM_COMMIT&&!(m.Protect&(PAGE_GUARD|PAGE_NOACCESS))&&(m.Protect&(PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))){if(!executableRanges.empty()&&executableRanges.back().second==here)executableRanges.back().second=here+length;else executableRanges.emplace_back(here,here+length);}at+=length;
  }
 }
 Function entry(const RUNTIME_FUNCTION& f)const{if(f.BeginAddress>=f.EndAddress||f.EndAddress>size)return {};auto start=reinterpret_cast<uintptr_t>(base+f.BeginAddress),end=start+std::min<size_t>(f.EndAddress-f.BeginAddress,2048);for(auto [a,b]:executableRanges)if(start>=a&&end<=b)return {base+f.BeginAddress,f.EndAddress-f.BeginAddress};return {};}
 Function entry(const void* p)const{
  if(!contains(p,1)||!functions)return {};auto rva=static_cast<DWORD>(static_cast<const unsigned char*>(p)-base);size_t lo=0,hi=count;
  while(lo<hi){auto mid=lo+(hi-lo)/2;if(functions[mid].BeginAddress<rva)lo=mid+1;else hi=mid;}
  return lo<count&&functions[lo].BeginAddress==rva?entry(functions[lo]):Function{};
 }
};
inline bool instructionBoundary(Function f,size_t target){
 size_t at=0;while(at<target){hde64s instruction{};auto available=std::min<size_t>(std::min<size_t>(f.size,2048)-at,16);unsigned char bytes[16]{};std::memcpy(bytes,f.code+at,available);auto length=hde64_disasm(bytes,&instruction);if(!length||length>available||(instruction.flags&F_ERROR))return false;at+=length;}return at==target;
}
// Anchors describe the argument/return relationship used by the wrapper. They
// may move within a function; relocation and unrelated instructions are ignored.
template<size_t N> inline size_t find(Function f,const std::array<unsigned char,N>& bytes,size_t from=0){
 if(f.size<N||from>f.size-N)return SIZE_MAX;
 auto end=std::min<size_t>(f.size,2048);if(end<N||!f.code)return SIZE_MAX;
 for(size_t i=from;i+N<=end;++i)if(std::memcmp(f.code+i,bytes.data(),N)==0&&instructionBoundary(f,i))return i;return SIZE_MAX;
}
template<size_t N> inline bool has(Function f,const std::array<unsigned char,N>& bytes){return find(f,bytes)!=SIZE_MAX;}
inline const unsigned char* relative(const unsigned char* p,size_t operand=1,size_t length=5){int32_t d{};std::memcpy(&d,p+operand,4);return reinterpret_cast<const unsigned char*>(reinterpret_cast<uintptr_t>(p)+length+static_cast<int64_t>(d));}
template<class Predicate> inline void* resolve(Predicate predicate,const wchar_t* operation,std::wstring& error){
 Image image;if(!image.functions){error=std::wstring(operation)+L": PE function-entry metadata unavailable";return nullptr;}
 const unsigned char* result{};
 for(size_t i=0;i<image.count;++i){auto f=image.entry(image.functions[i]);if(!f.code||!predicate(f,image))continue;
  if(result){error=std::wstring(operation)+L": more than one entry satisfies the required argument/data-flow anchors";return nullptr;}result=f.code;
 }
 if(!result)error=std::wstring(operation)+L": no executable function entry satisfies the required argument/data-flow anchors";
 return const_cast<unsigned char*>(result);
}
}
