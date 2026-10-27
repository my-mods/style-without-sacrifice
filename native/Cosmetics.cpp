#include "Runtime.hpp"
#include "NativeContract.hpp"
#include "AppearanceCompletion.hpp"
#include "SheathContract.hpp"
#include "CatalogOrder.hpp"
#include "WeaponScale.hpp"
#include "BladeMaterial.hpp"
#include "Performance.hpp"
#include <Windows.h>
#include <MinHook.h>
#include <array>
#include <cstring>
#include <map>
#include <chrono>
#include <unordered_map>
#include <deque>
#include <Unreal/UObjectArray.hpp>
#include <Unreal/FFrame.hpp>

namespace Wardrobe {
namespace {
using RowGetter=void*(*)(UObject*,UObject*);
RowGetter originalRow{};void* rowTarget{};DWORD threadId{};
using AppearanceReady=void(*)(UObject*);
AppearanceReady originalAppearanceReady{};void* appearanceReadyTarget{};
using SheathWeapon=void(*)(void*,UObject*,const MeshScale*);
using SheathScabbard=void(*)(void*,UObject*);
using SheathVisibility=void(*)(void*,bool);
SheathWeapon originalSheathWeapon{};SheathScabbard originalSheathScabbard{};
SheathVisibility originalSheathWeaponVisibility{},originalSheathScabbardVisibility{};
std::array<void*,4> sheathTargets{};bool sheathBound{};size_t sheathInterfaceOffset{};
Ref subsystem,clothingTable,weaponTable,rowType,weaponType;
Ref inventorySystem;
struct AddedItem {Ref owner;uint32_t handle;};
std::deque<AddedItem> addedItems;
FProperty* slotProperty{};std::array<int,4> slotValues{1,2,3,4};
struct OwnedRow {
    Ref type;std::vector<uint64_t> bytes;
    OwnedRow(UScriptStruct* t,const void* data):type(t),bytes((t->GetPropertiesSize()+7)/8+1){t->InitializeStruct(bytes.data());if(data)t->CopyScriptStruct(bytes.data(),data);}
    ~OwnedRow(){if(auto t=static_cast<UScriptStruct*>(type.get()))t->DestroyStruct(bytes.data());}
};
std::array<std::array<std::unique_ptr<OwnedRow>,4>,2> overrides;
std::array<Ref,8> weapons;
std::array<Ref,2> weaponMesh,scabbardMesh;
std::array<std::optional<MeshScale>,2> weaponScale;
unsigned tablePhase{},tableCursor{};int expectedRows{};uint64_t lastLoadoutFrame=~0ull;
unsigned itemCursor{},itemLimit{};bool itemsIndexed{};
std::map<std::wstring,Ref> catalogItems;
bool catalogStarted{},insideRefresh{},started{};
std::array<std::array<std::optional<Choice>,5>,2> preparedChoices;
bool inventoryDirty=true;
uint64_t inventoryDue{};unsigned inventoryCursor{};
bool inventoryScanning{};unsigned inventoryPhase{};uint64_t inventoryGeneration{};
Ref scanOwner;
std::vector<Ref> inventoryItems;bool inventoryCaptured{};
struct ItemRow {Ref item;std::wstring row;};
std::unordered_map<UObject*,ItemRow> itemLookup;
struct MeshEdit {BladeMaterial blade;Ref component,original,installed;bool visible{},appliedVisible{},changedMesh{},changedVisibility{};std::optional<MeshScale> originalScale,installedScale;};
std::array<MeshEdit,24> meshEdits;
void restoreMesh(MeshEdit& edit){
 try{auto component=edit.component.get();
  const bool ownsMesh=component&&object(component,L"StaticMesh")==edit.installed.address&&(!edit.installed.address||edit.installed);
  edit.blade.restore(ownsMesh);
  if(component){
  // Restore only our own scale, and only while our mesh still owns it. An
  // equipment refresh or another appearance writer may already have replaced it.
  if(edit.originalScale&&edit.installedScale&&ownsMesh&&readScale(property(component,L"RelativeScale3D"),component)==edit.installedScale)
      writeScale(component,*edit.originalScale);
  if(edit.changedMesh&&ownsMesh&&(!edit.original.address||edit.original)){Call c(component,L"SetStaticMesh");if(c)c.obj(L"NewMesh",edit.original.get()).invoke();}
  if(edit.changedVisibility&&ownsMesh){Call get(component,L"IsVisible");if(get){get.invoke();if((get.resultInteger()!=0)==edit.appliedVisible){Call set(component,L"SetVisibility");set.num(L"bNewVisibility",edit.visible).invoke();}}}
 }}catch(...){warn(L"A previous weapon appearance could not be restored during cleanup.");}edit={};
}
void restoreMeshes(){for(auto& edit:meshEdits)restoreMesh(edit);}
void releaseMesh(UObject* component){if(component)for(auto& edit:meshEdits)if(edit.component.matches(component)){restoreMesh(edit);break;}}
void editMesh(UObject* component,UObject* replacement,bool changeMesh,std::optional<bool> visible,const MeshScale* scale=nullptr,UObject* bladeActor=nullptr){
 if(!component)return;for(auto& e:meshEdits)if(e.component.matches(component)){
  // A character rebuild can overwrite a journaled component between ticks.
  // Keep unchanged edits; rebase only the component the engine has reset.
  bool intact=object(component,L"StaticMesh")==e.installed.address&&(!e.installed.address||e.installed)&&(!changeMesh||e.installed.address==replacement);
  if(intact&&bladeActor)intact=e.blade.intact();
  if(intact&&scale)intact=readScale(property(component,L"RelativeScale3D"),component)==*scale;
  if(intact&&visible){Call get(component,L"IsVisible");if(get){get.invoke();intact=(get.resultInteger()!=0)==*visible;}else intact=false;}
  if(intact)return;
  restoreMesh(e);break;
 }
 MeshEdit* entry=nullptr;for(auto& e:meshEdits)if(!e.component){restoreMesh(e);entry=&e;break;}if(!entry)return;
 if(scale){
  auto original=readScale(property(component,L"RelativeScale3D"),component);
  if(!original){warn(L"Weapon scale cannot be read from this component; keeping its equipped appearance.");return;}
  if(*original!=*scale){
   if(!writeScale(component,*scale)){writeScale(component,*original);warn(L"Weapon scale cannot be applied to this component; keeping its equipped appearance.");return;}
   entry->originalScale=original;entry->installedScale=*scale;
  }
 }
 entry->component=Ref(component);entry->original=Ref(object(component,L"StaticMesh"));entry->installed=Ref(changeMesh?replacement:entry->original.get());
 if(changeMesh&&replacement!=entry->original.get()){
  Call set(component,L"SetStaticMesh");if(set)set.obj(L"NewMesh",replacement).invoke();
  entry->changedMesh=object(component,L"StaticMesh")==replacement;
  if(!entry->changedMesh){if(entry->originalScale)writeScale(component,*entry->originalScale);*entry={};warn(L"Weapon mesh could not be replaced; keeping its equipped appearance.");return;}
 }
 if(bladeActor&&changeMesh&&!entry->blade.apply(bladeActor,component,replacement)){restoreMesh(*entry);warn(L"Drawn blade material could not be rebound; keeping its equipped appearance.");return;}
 if(visible){Call get(component,L"IsVisible");if(get){get.invoke();entry->visible=get.resultInteger()!=0;entry->changedVisibility=entry->visible!=*visible;entry->appliedVisible=*visible;if(entry->changedVisibility){Call set(component,L"SetVisibility");set.num(L"bNewVisibility",*visible).invoke();}}}
}
std::wstring cleanName(std::wstring name){
    for(auto prefix:{L"ITM_",L"Weapon_",L"Armor_",L"Clothing_"})if(name.starts_with(prefix))name.erase(0,wcslen(prefix));
    std::replace(name.begin(),name.end(),L'_',L' ');return name;
}
int slotFor(void* row){if(!slotProperty||!row)return -1;auto v=integer(slotProperty,row);for(int i=0;i<4;++i)if(v==slotValues[i])return i;return -1;}
UDataTable* table(bool weapon){return static_cast<UDataTable*>((weapon?weaponTable:clothingTable).get());}
void* lookup(bool weapon,const std::wstring& key){auto t=table(weapon);if(!t||key.empty())return nullptr;auto p=t->GetRowMap().Find(FName(key.c_str(),FNAME_Find));return p?*p:nullptr;}
unsigned loadout(){auto inv=runtime.inventory.get();if(!inv)return runtime.activeSet;Call c(inv,L"GetActiveLoadoutIndex");if(!c)return runtime.activeSet;c.invoke();return static_cast<unsigned>(std::clamp<int64_t>(c.resultInteger(),0,1));}
bool targetContext(UObject* context){return runtime.appearance.matches(context)||runtime.player.matches(context)||runtime.doll.matches(context)||runtime.dollAppearance.matches(context);}
void* clothingRow(UObject* item,UObject* context){
    auto result=originalRow(item,context);
    if(!started||GetCurrentThreadId()!=threadId||!runtime.settings.enabled||!result||!targetContext(context))return result;
    try{
        if(lastLoadoutFrame!=runtime.frame){lastLoadoutFrame=runtime.frame;auto set=loadout();if(set!=runtime.activeSet){runtime.activeSet=set;if(!runtime.menuOpen)runtime.model.switchSet(set);requestRefresh(false,true);}}
        int slot=slotFor(result);if(slot<0)return result;
        unsigned set=(runtime.doll.matches(context)||runtime.dollAppearance.matches(context))?runtime.model.displayedSet:runtime.activeSet;
        auto& replacement=overrides[set][slot];
        if(replacement&&replacement->type.get()){if(logging)++runtime.nativeOverrides;return replacement->bytes.data();}
    }catch(...){warn(L"Clothing layout unavailable; keeping the equipped appearance.");}
    return result;
}
UObject* loadAsset(FProperty* source,void* data){
    MeasureOperation timing(Operation::AssetLoad);
    if(!source||!data)return nullptr;
    if(source->IsA<FObjectPropertyBase>()&&!source->IsA<FSoftObjectProperty>())return readObject(source,data);
    Call c(find(L"/Script/Engine.Default__KismetSystemLibrary"),L"LoadAsset_Blocking");if(!c)return nullptr;auto target=c.field(L"Asset");
    if(!target||target->GetSize()!=source->GetSize()||!source->IsA<FSoftObjectProperty>())return nullptr;
    target->CopyCompleteValue(c.value(L"Asset"),source->ContainerPtrToValuePtr<void>(data));c.invoke();return c.resultObject();
}
void visibility(UObject* mesh,bool visible){if(!mesh)return;Call c(mesh,L"SetVisibility");if(c){c.num(L"bNewVisibility",visible);if(c.field(L"bPropagateToChildren"))c.num(L"bPropagateToChildren",1);c.invoke();}}
void staticMesh(UObject* component,UObject* mesh){if(!component||!mesh)return;Call c(component,L"SetStaticMesh");if(c)c.obj(L"NewMesh",mesh).invoke();}
UObject* weaponOwner(UObject* actor){
    if(!actor)return nullptr;
    auto owner=object(actor,L"Owner");
    if(runtime.player.matches(owner)||runtime.doll.matches(owner))return owner;
    // Combat-created weapons receive their combat owner after BeginPlay.
    // Use the reflected component owner; never read the native weak pawn slot.
    auto combat=object(actor,L"OwningCombatComponent");if(!combat)return nullptr;
    Call get(combat,L"GetOwner");auto result=get?get.fn->GetReturnProperty():nullptr;
    if(!result||!result->IsA<FObjectProperty>()||result->GetSize()!=sizeof(void*)||
       static_cast<const FProperty*>(result)->GetOffset_Internal()!=0||get.fn->GetParmsSize()!=sizeof(void*))return nullptr;
    get.invoke();owner=get.resultObject();
    return runtime.player.matches(owner)||runtime.doll.matches(owner)?owner:nullptr;
}
bool physicalWeapon(UObject* actor){
    auto p=property(actor,L"WeaponType");UEnum* enumeration=nullptr;
    if(p&&p->GetSize()==1){
        if(p->IsA<FEnumProperty>())enumeration=static_cast<FEnumProperty*>(p)->GetEnum();
        else if(p->IsA<FByteProperty>())enumeration=static_cast<FByteProperty*>(p)->GetEnum();
    }
    if(enumeration&&enumeration->GetName()==L"EWeaponType"){
        // This combat enum groups physical weapons under Sword; Fist and Claw
        // also have WeaponBase actors, often with empty but visible meshes.
        for(auto pair:enumeration->ForEachName())if(pair.Key.ToString()==L"EWeaponType::Sword")return integer(p,actor)==pair.Value;
    }
    warn(L"WeaponType classification is unavailable; drawn weapon overrides are disabled for this actor.");return false;
}
void observeMainWeapon(UObject* pawn){
    auto combat=object(pawn,L"CombatComponent");if(!combat)return;
    Call get(combat,L"GetMainWeapon");auto result=get?get.fn->GetReturnProperty():nullptr;
    if(!result||!result->IsA<FObjectProperty>()||result->GetSize()!=sizeof(void*)||
       static_cast<const FProperty*>(result)->GetOffset_Internal()!=0||get.fn->GetParmsSize()!=sizeof(void*)){
        warn(L"Current weapon lookup is unavailable; waiting for a weapon appearance event.");return;
    }
    get.invoke();observeWeapon(get.resultObject());
}
void weaponAppearance(UObject* actor){
    auto owner=weaponOwner(actor);if(!owner||!physicalWeapon(actor))return;
    auto mesh=object(actor,L"BaseMesh");if(!mesh)return;unsigned set=runtime.doll.matches(owner)?runtime.model.displayedSet:runtime.activeSet;auto& c=runtime.model.sets[set][4];
    // Leave draw/sheath and pooled-weapon visibility under game control.
    // A remembered inactive weapon must never be revealed by changing its look.
    if(runtime.settings.enabled&&c.mode==Choice::Mode::Look&&weaponMesh[set]&&weaponScale[set])editMesh(mesh,weaponMesh[set].get(),true,std::nullopt,&*weaponScale[set],actor);
    else releaseMesh(mesh);
}
void sheathedAppearance(UObject* owner,bool doll,bool preserveVisibility=false){
    if(!owner||!runtime.settings.enabled)return;
    unsigned set=doll?runtime.model.displayedSet:runtime.activeSet;auto& choice=runtime.model.sets[set][4];
    auto sheathed=object(owner,doll?L"WeaponMesh":L"SheathedWeaponMesh");auto scabbard=object(owner,L"Scabbard");
    if(!scabbard)scabbard=object(owner,L"ScabbardMesh");
    if(choice.mode==Choice::Mode::Hidden){editMesh(sheathed,nullptr,false,false);editMesh(scabbard,nullptr,false,false);}
    else if(choice.mode==Choice::Mode::Look&&weaponMesh[set]&&weaponScale[set]){editMesh(sheathed,weaponMesh[set].get(),true,std::nullopt,&*weaponScale[set]);editMesh(scabbard,scabbardMesh[set].get(),true,preserveVisibility&&scabbardMesh[set]?std::nullopt:std::optional<bool>(bool(scabbardMesh[set])),&*weaponScale[set]);}
    else {releaseMesh(sheathed);releaseMesh(scabbard);}
}
void bindSheath(UObject* pawn){
    sheathBound=false;if(!sheathTargets[0])return;
    auto weapon=property(pawn,L"SheathedWeaponMesh"),scabbard=property(pawn,L"ScabbardMesh");
    auto valid=[](FProperty* p){return p&&p->IsA<FObjectProperty>()&&p->GetSize()==sizeof(void*);};
    if(valid(weapon)&&valid(scabbard)&&SheathContract::interfaceMatches(pawn,pawn->GetClassPrivate()->GetPropertiesSize(),
       static_cast<const FProperty*>(weapon)->GetOffset_Internal(),static_cast<const FProperty*>(scabbard)->GetOffset_Internal(),sheathTargets)){sheathBound=true;sheathInterfaceOffset=SheathContract::reflectedInterface(pawn->GetClassPrivate()->GetPropertiesSize(),static_cast<const FProperty*>(weapon)->GetOffset_Internal(),static_cast<const FProperty*>(scabbard)->GetOffset_Internal());}
    if(!sheathBound)warn(L"Player sheath layout changed; immediate sheath protection is unavailable.");
    else if(logging)trace(L"Player weapon, scabbard and visibility setters bound for immediate appearance protection.");
}
struct SheathWrite {
    UObject* pawn{};std::chrono::steady_clock::time_point began{};
    explicit SheathWrite(void* context){
        if(!started||GetCurrentThreadId()!=threadId||insideRefresh||!sheathBound||!runtime.settings.enabled||
           reinterpret_cast<uintptr_t>(context)!=reinterpret_cast<uintptr_t>(runtime.player.address)+sheathInterfaceOffset)return;
        pawn=runtime.player.get();if(!pawn)return;
        insideRefresh=true;if(logging){began=std::chrono::steady_clock::now();++runtime.sheathWrites;}
        // A scabbard-only write copies the weapon's current scale. Release our
        // edits first so that stock code sees equipped values, not the selected
        // look's scale. Both meshes are repaired within the same native call.
        try{releaseMesh(object(pawn,L"SheathedWeaponMesh"));releaseMesh(object(pawn,L"ScabbardMesh"));}
        catch(const std::exception& error){failure(L"Preparing player sheath write",error);}
    }
    ~SheathWrite(){
        if(!pawn)return;
        try{if(runtime.player.matches(pawn)){
            runtime.activeSet=loadout();auto set=runtime.activeSet;const auto& choice=runtime.model.sets[set][4];
            if(preparedChoices[set][4]!=choice||(choice.mode==Choice::Mode::Look&&
               (!weaponMesh[set]||!weaponScale[set]||(scabbardMesh[set].address&&!scabbardMesh[set])))){
                preparedChoices[set][4].reset();requestRefresh(false,false);
            }else sheathedAppearance(pawn,false,true);
        }}catch(const std::exception& error){requestRefresh(false,false);failure(L"Restoring player sheath write",error);}
        insideRefresh=false;
        if(logging){auto elapsed=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
            runtime.sheathMicros+=elapsed;runtime.maxSheathMicros=std::max(runtime.maxSheathMicros,elapsed);}
    }
};
void sheathWeapon(void* context,UObject* mesh,const MeshScale* scale){SheathWrite repair(context);originalSheathWeapon(context,mesh,scale);}
void sheathScabbard(void* context,UObject* mesh){SheathWrite repair(context);originalSheathScabbard(context,mesh);}
void sheathWeaponVisibility(void* context,bool visible){SheathWrite repair(context);originalSheathWeaponVisibility(context,visible);}
void sheathScabbardVisibility(void* context,bool visible){SheathWrite repair(context);originalSheathScabbardVisibility(context,visible);}
void stopSheathHooks(){
    sheathBound=false;
    for(auto& target:sheathTargets)if(target){MH_DisableHook(target);MH_RemoveHook(target);target=nullptr;}
}
void startSheathHooks(){
    std::wstring error;auto targets=SheathContract::resolve(error);
    if(!targets[0]){warn(L"Immediate sheath protection unavailable: "+error);return;}
    std::array<void*,4> hooks{reinterpret_cast<void*>(sheathWeapon),reinterpret_cast<void*>(sheathWeaponVisibility),reinterpret_cast<void*>(sheathScabbard),reinterpret_cast<void*>(sheathScabbardVisibility)};
    std::array<void**,4> originals{reinterpret_cast<void**>(&originalSheathWeapon),reinterpret_cast<void**>(&originalSheathWeaponVisibility),reinterpret_cast<void**>(&originalSheathScabbard),reinterpret_cast<void**>(&originalSheathScabbardVisibility)};
    for(size_t i=0;i<targets.size();++i){
        if(MH_CreateHook(targets[i],hooks[i],originals[i])!=MH_OK){stopSheathHooks();warn(L"A player sheath setter is already modified or cannot be hooked.");return;}
        sheathTargets[i]=targets[i];
    }
    for(auto target:sheathTargets)if(MH_EnableHook(target)!=MH_OK){stopSheathHooks();warn(L"Player sheath protection could not be enabled.");return;}
}
void appearanceCompleted(UObject* app){
    if(!started||GetCurrentThreadId()!=threadId||insideRefresh||
       (!runtime.appearance.matches(app)&&!runtime.dollAppearance.matches(app)))return;
    if(logging)++runtime.appearanceEvents;
    if(!runtime.settings.enabled)return;
    // Character rebuilds replace the sheathed meshes independently of the
    // combat sheath setters. Repair cached looks before returning, rather than
    // exposing the equipped look until a later EngineTickPost continuation.
    // Asset loading and failed preparation stay in the bounded worker.
    bool doll=runtime.dollAppearance.matches(app);
    runtime.activeSet=loadout();unsigned set=doll?runtime.model.displayedSet:runtime.activeSet;
    const auto& choice=runtime.model.sets[set][4];
    if(preparedChoices[set][4]!=choice||(choice.mode==Choice::Mode::Look&&
       (!weaponMesh[set]||!weaponScale[set]||(scabbardMesh[set].address&&!scabbardMesh[set])))){
        preparedChoices[set][4].reset();requestRefresh(false,false);return;
    }
    auto began=logging?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    insideRefresh=true;
    try{
        auto pawn=doll?runtime.doll.get():runtime.player.get();
        if(!doll)sheathedAppearance(app,false);
        sheathedAppearance(pawn,doll);
        observeMainWeapon(pawn);
        for(auto& w:weapons)if(auto actor=w.get();actor&&weaponOwner(actor)==pawn)weaponAppearance(actor);
    }catch(...){insideRefresh=false;requestRefresh(false,false);throw;}
    insideRefresh=false;
    if(logging){auto elapsed=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-began).count());
        ++runtime.immediateAppearances;runtime.appearanceMicros+=elapsed;runtime.maxAppearanceMicros=std::max(runtime.maxAppearanceMicros,elapsed);}
}
void appearanceReady(UObject* app){
    originalAppearanceReady(app);
    try{appearanceCompleted(app);}catch(const std::exception& error){failure(L"Completed player appearance",error);}
}
bool prepareHiddenRow(UScriptStruct* type,unsigned slot,OwnedRow& row){
    if(slot!=static_cast<unsigned>(Slot::Gauntlets))return true;
    auto mesh=property(type,L"Mesh");
    if(!mesh||!mesh->IsA<FStructProperty>())return false;
    auto meshType=static_cast<FStructProperty*>(mesh)->GetStruct();
    auto index=meshType?property(meshType,L"GauntletIndex"):nullptr;
    if(!index||!index->IsA<FByteProperty>()||index->GetSize()!=1||
       static_cast<const FProperty*>(index)->GetOffset_Internal()!=6)return false;
    // AppearanceMesh initializes this byte to 255. The wrist-slot consumer
    // converts it directly to the arm deformation parameter, without treating
    // 255 as a sentinel. With no wrist garment the game uses 0, so this row must too.
    auto data=mesh->ContainerPtrToValuePtr<void>(row.bytes.data());
    number(index,data,0);
    return integer(index,data)==0;
}
void prepareWeaponRows(){
    for(unsigned set=0;set<2;++set){auto& c=runtime.model.sets[set][4];
        // Failed loads are not retried by unrelated inventory/UI events. A new
        // selection or player context starts another attempt.
        if(preparedChoices[set][4]==c&&(!weaponMesh[set].address||weaponMesh[set])&&(!scabbardMesh[set].address||scabbardMesh[set]))continue;
        preparedChoices[set][4]=c;weaponMesh[set]={};scabbardMesh[set]={};weaponScale[set].reset();
        if(auto wt=static_cast<UScriptStruct*>(weaponType.get());wt&&c.mode==Choice::Mode::Look)if(auto row=lookup(true,c.row)){
            // Asset loading can run engine callbacks. Keep an owned row rather
            // than a pointer into the table across those calls.
            OwnedRow selected(wt,row);
            // The stock debug-heavy row is a cart, and PickaxeTest is an axe.
            // Use the actual pickaxe template without changing shared assets.
            bool pickaxe=c.row==L"ITM_Weapon_TestDebugHeavyWeapon";Ref templateMesh;
            auto scale=selectedWeaponScale(c.row,pickaxe?L"/Game/_Dawnwalker/Blueprints/Items/Equippable/BP_Weapon_Pickaxe.BP_Weapon_Pickaxe_C":nullptr,pickaxe?&templateMesh:nullptr);
            if(!scale)continue;
            auto type=static_cast<UScriptStruct*>(selected.type.get());if(!type)continue;
            weaponScale[set]=scale;
            weaponMesh[set]=pickaxe?templateMesh:Ref(loadAsset(property(type,L"WeaponMesh"),selected.bytes.data()));
            if(!pickaxe&&selected.type)scabbardMesh[set]=Ref(loadAsset(property(type,L"ScabbardMesh"),selected.bytes.data()));
            if(logging)trace(L"Selected weapon authored mesh scale: row="+c.row+L", x="+std::to_wstring((*scale)[0])+L", y="+std::to_wstring((*scale)[1])+L", z="+std::to_wstring((*scale)[2]));
        }
    }
}
void prepareRows(){
    auto type=static_cast<UScriptStruct*>(rowType.get());if(!type)return;
    for(unsigned set=0;set<2;++set)for(unsigned slot=0;slot<4;++slot){
        auto& choice=runtime.model.sets[set][slot];auto& snapshot=overrides[set][slot];
        if(preparedChoices[set][slot]==choice&&(!snapshot||snapshot->type))continue;
        preparedChoices[set][slot]=choice;snapshot.reset();
        if(choice.mode==Choice::Mode::Original)continue;
        auto row=choice.mode==Choice::Mode::Look?lookup(false,choice.row):nullptr;
        if(choice.mode==Choice::Mode::Look&&(!row||slotFor(row)!=static_cast<int>(slot))){warn(L"A saved garment is unavailable; keeping the equipped look.");continue;}
        auto prepared=std::make_unique<OwnedRow>(type,row);number(slotProperty,prepared->bytes.data(),slotValues[slot]);
        if(choice.mode==Choice::Mode::Hidden&&!prepareHiddenRow(type,slot,*prepared)){
            warn(L"Hidden wrists require the GauntletIndex byte field; keeping the equipped look.");continue;
        }
        snapshot=std::move(prepared);
        if(logging&&choice.mode==Choice::Mode::Hidden&&slot==static_cast<unsigned>(Slot::Gauntlets))
            trace(L"Hidden wrists prepared with neutral arm deformation: set="+std::to_wstring(set));
    }
    prepareWeaponRows();
}
bool tables(UObject* pawn){
    Call sub(find(L"/Script/Engine.Default__SubsystemBlueprintLibrary"),L"GetGameInstanceSubsystem");if(!sub)return false;
    sub.obj(L"ContextObject",pawn).obj(L"Class",find(L"/Script/DogwoodInventory.AppearanceSubsystem")).invoke();auto system=sub.resultObject();if(!system)return false;
    auto ct=object(system,L"LoadedAppearanceUnitTable");
    auto wt=object(system,L"LoadedWeaponAppearances");
    if(!wt)wt=object(find(L"/Script/DogwoodInventory.Default__DogwoodInventorySettings"),L"LoadedWeaponAppearances");
    if(!wt)wt=find(L"/Game/_Dawnwalker/Inventory/Items/DT_WeaponAppearances.DT_WeaponAppearances");
    if(!ct||!ct->IsA<UDataTable>())return false;
    auto type=static_cast<UDataTable*>(ct)->GetRowStruct().Get();
    // Rows are initialized, copied and destroyed through their current reflected
    // struct. Additional game fields are preserved by CopyScriptStruct.
    if(!type||type->GetName()!=L"AppearanceClothingUnitRow"){warn(L"Clothing table does not expose AppearanceClothingUnitRow; clothing preparation is unavailable.");return false;}
    auto slot=property(type,L"Slot"),mesh=property(type,L"Mesh");if(!slot||!mesh||!mesh->IsA<FStructProperty>())return false;
    auto item=property(type,L"Item");auto meshStruct=static_cast<FStructProperty*>(mesh)->GetStruct();
    auto insideRow=[&](FProperty* field){if(!field)return false;auto offset=static_cast<const FProperty*>(field)->GetOffset_Internal(),size=field->GetSize();return offset>=0&&size>0&&offset<=type->GetPropertiesSize()&&size<=type->GetPropertiesSize()-offset;};
    if(!insideRow(slot)||!insideRow(mesh)||!insideRow(item)||!meshStruct||meshStruct->GetPropertiesSize()!=mesh->GetSize()||!item->IsA<FObjectProperty>()||item->GetSize()!=sizeof(void*)){
        warn(L"Clothing row requires in-bounds Slot, Mesh struct and Item object fields; clothing preparation is unavailable.");return false;
    }
    UEnum* enumeration=nullptr;
    if(slot->IsA<FEnumProperty>())enumeration=static_cast<FEnumProperty*>(slot)->GetEnum();
    else if(slot->IsA<FByteProperty>())enumeration=static_cast<FByteProperty*>(slot)->GetEnum();
    if(!enumeration)return false;
    std::array<bool,4> found{};
    for(auto pair:enumeration->ForEachName()){auto n=pair.Key.ToString();for(unsigned i=0;i<4;++i)if(n==slotNames[i]||n.ends_with(std::wstring(L"::")+slotNames[i])){slotValues[i]=static_cast<int>(pair.Value);found[i]=true;}}
    if(!std::all_of(found.begin(),found.end(),[](bool b){return b;}))return false;
    subsystem=Ref(system);clothingTable=Ref(ct);rowType=Ref(type);slotProperty=slot;
    Call inventorySub(find(L"/Script/Engine.Default__SubsystemBlueprintLibrary"),L"GetGameInstanceSubsystem");
    if(inventorySub){inventorySub.obj(L"ContextObject",pawn).obj(L"Class",find(L"/Script/DogwoodInventory.InventorySubsystem")).invoke();inventorySystem=Ref(inventorySub.resultObject());}
    if(wt&&wt->IsA<UDataTable>()){weaponTable=Ref(wt);weaponType=Ref(static_cast<UDataTable*>(wt)->GetRowStruct().Get());}
    return true;
}
}
bool startCosmetics(){
    threadId=GetCurrentThreadId();std::wstring error;
    auto initialized=MH_Initialize();if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED){warn(L"Native hook service unavailable.");return false;}
    startSheathHooks();
    appearanceReadyTarget=AppearanceCompletion::resolve(error);
    if(appearanceReadyTarget){
        if(MH_CreateHook(appearanceReadyTarget,reinterpret_cast<void*>(appearanceReady),reinterpret_cast<void**>(&originalAppearanceReady))!=MH_OK){warn(L"Appearance completion is already modified or cannot be hooked.");appearanceReadyTarget=nullptr;}
        else if(MH_EnableHook(appearanceReadyTarget)!=MH_OK){MH_RemoveHook(appearanceReadyTarget);appearanceReadyTarget=nullptr;warn(L"Appearance completion listener could not be enabled.");}
    }else warn(L"Saved weapon appearance recovery unavailable: "+error);
    started=true;
    rowTarget=NativeContract::resolve(error);
    if(!rowTarget){warn(L"Clothing overrides unavailable: "+error);return false;}
    if(MH_CreateHook(rowTarget,reinterpret_cast<void*>(clothingRow),reinterpret_cast<void**>(&originalRow))!=MH_OK){warn(L"Clothing lookup is already modified or cannot be hooked.");rowTarget=nullptr;return false;}
    if(MH_EnableHook(rowTarget)!=MH_OK){MH_RemoveHook(rowTarget);rowTarget=nullptr;return false;}
    started=true;return true;
}
void stopCosmetics(){started=false;stopSheathHooks();if(appearanceReadyTarget){MH_DisableHook(appearanceReadyTarget);MH_RemoveHook(appearanceReadyTarget);appearanceReadyTarget=nullptr;}if(rowTarget){MH_DisableHook(rowTarget);MH_RemoveHook(rowTarget);rowTarget=nullptr;}resetCosmetics();}
void resetCosmetics(bool restore){
    if(!restore)BladeMaterial::abandonPins();
    sheathBound=false;
    // Game-thread context changes release owned edits before forgetting them.
    // Loader-thread shutdown passes false and never invokes engine functions.
    if(restore){
        if(logging){unsigned count=0;for(const auto& edit:meshEdits)if(edit.component)++count;
            if(count)trace(L"Releasing weapon appearance edits for player context: "+std::to_wstring(count));}
        restoreMeshes();
    }
    meshEdits={};preparedChoices={};inventoryDirty=true;inventoryScanning=false;scanOwner={};inventoryItems.clear();inventoryCaptured=false;++inventoryGeneration;inventorySystem={};runtime.storage={};addedItems.clear();inventoryCursor=0;inventoryDue=0;runtime.playerRefresh=runtime.previewRefresh=false;for(auto& set:overrides)for(auto& row:set)row.reset();subsystem={};clothingTable={};weaponTable={};rowType={};weaponType={};slotProperty=nullptr;weapons={};weaponMesh={};scabbardMesh={};weaponScale={};catalogStarted=false;runtime.catalogReady=runtime.playerReady=false;runtime.looks.clear();itemLookup.clear();lastLoadoutFrame=~0ull;}
void cancelCosmeticWork(){catalogStarted=false;inventoryDirty=false;addedItems.clear();inventoryScanning=false;scanOwner={};inventoryItems.clear();inventoryCaptured=false;++inventoryGeneration;runtime.refreshWork.cancel();runtime.playerRefresh=runtime.previewRefresh=false;}
bool attach(UObject* pawn){
    MeasureOperation timing(Operation::Attach);
    if(!pawn)return false;auto controller=object(pawn,L"Controller");if(!controller||!object(controller,L"Player"))return false;
    auto inv=object(pawn,L"InventoryComponent"),app=object(pawn,L"AppearanceComponent");if(!inv||!app)return false;
    if(runtime.playerReady||!runtime.player.matches(pawn)||!runtime.inventory.matches(inv)||!runtime.appearance.matches(app)){resetCosmetics(true);runtime.player=Ref(pawn);runtime.controller=Ref(controller);runtime.inventory=Ref(inv);runtime.appearance=Ref(app);runtime.doll={};runtime.dollAppearance={};}
    if(!tables(pawn))return false;bindSheath(pawn);beginCatalog();runtime.activeSet=loadout();runtime.model.switchSet(runtime.activeSet);runtime.playerReady=true;requestRefresh();if(logging)trace(L"Attached player: "+pawn->GetName());return true;
}
void beginCatalog(){if(catalogStarted||!clothingTable)return;catalogStarted=true;tablePhase=tableCursor=0;itemCursor=itemLimit=0;itemsIndexed=false;catalogItems.clear();expectedRows=table(false)->GetRowMap().Num();runtime.looks.clear();itemLookup.clear();runtime.catalogReady=false;}
bool indexCatalogItems(){
    MeasureOperation timing(Operation::CatalogIndex);
    if(itemsIndexed)return true;
    auto weapons=table(true);if(!weapons){itemsIndexed=true;return true;}
    if(!itemLimit)itemLimit=static_cast<unsigned>(std::max(0,FUObjectArray::GetNumElements()));
    const auto start=std::chrono::steady_clock::now();unsigned count=0;
    // One bounded pass replaces one global name search for every weapon row.
    // Only inspect live slots on the game thread; retained objects use Ref.
    while(itemCursor<itemLimit&&count++<2048){
        auto slot=FUObjectArray::IndexToObject(static_cast<int>(itemCursor++));
        if(slot&&FUObjectArray::IsValid(slot,false))if(auto item=slot->GetUObject()){
            if(weapons->GetRowMap().Find(item->GetFName())){
                auto name=item->GetName();auto path=L"/Game/_Dawnwalker/Inventory/Items/"+name+L"."+name;
                if(item->GetPathName()==path)catalogItems[name]=Ref(item);
            }
        }
        if(count%32==0&&std::chrono::steady_clock::now()-start>std::chrono::microseconds(500))break;
    }
    itemsIndexed=itemCursor>=itemLimit;return itemsIndexed;
}
bool hasPreviewIcon(UObject* item){
    if(!item)return false;auto p=property(item,L"ItemImage");if(!p)return false;
    if(p->IsA<FSoftObjectProperty>()){
        // Test the saved reference, not whether its texture happens to be loaded.
        // This never loads icons for off-screen catalog entries.
        auto system=find(L"/Script/Engine.Default__KismetSystemLibrary");
        if(function(system,L"IsValidSoftObjectReference")){
            Call c(system,L"IsValidSoftObjectReference");auto target=c.field(L"SoftObjectReference");
            if(target&&target->IsA<FSoftObjectProperty>()&&target->GetSize()==p->GetSize()){
                target->CopyCompleteValue(c.value(L"SoftObjectReference"),p->ContainerPtrToValuePtr<void>(item));c.invoke();return c.resultInteger()!=0;
            }
        }
        static bool reported{};if(!reported){warn(L"Item icon references could not be classified; appearances remain available in the NPC group.");reported=true;}return false;
    }
    return p->IsA<FObjectProperty>()&&readObject(p,item)!=nullptr;
}
std::string lookSortKey(const std::wstring& name){
    // Compute the user's locale-aware, case-insensitive collation key once in
    // the bounded catalog pass. Comparisons then use only owned bytes.
    constexpr DWORD flags=LCMAP_SORTKEY|NORM_IGNORECASE|SORT_STRINGSORT;
    int size=LCMapStringEx(LOCALE_NAME_USER_DEFAULT,flags,name.c_str(),-1,nullptr,0,nullptr,nullptr,0);
    if(size<=0)return {};std::string key(size,'\0');
    if(!LCMapStringEx(LOCALE_NAME_USER_DEFAULT,flags,name.c_str(),-1,reinterpret_cast<LPWSTR>(key.data()),size,nullptr,nullptr,0))return {};
    return key;
}
bool stepCatalog(){
    MeasureOperation timing(Operation::Catalog);
    if(!catalogStarted||runtime.catalogReady)return true;
    auto complete=[] {std::sort(runtime.looks.begin(),runtime.looks.end(),lookOrder<Look>);runtime.catalogReady=true;requestInventoryScan();menuRedraw();};
    auto t=table(tablePhase==1);if(!t){catalogStarted=false;if(tablePhase==1)complete();return true;}
    auto type=static_cast<UScriptStruct*>((tablePhase==1?weaponType:rowType).get());if(!type){catalogStarted=false;warn(L"Appearance catalog type is no longer available; reopen Wardrobe after loading.");return true;}
    auto& rows=t->GetRowMap();if(rows.Num()>16384||rows.GetMaxIndex()>32768){warn(L"Appearance catalog exceeds the supported safety limit.");catalogStarted=false;return true;}
    if(rows.Num()!=expectedRows){catalogStarted=false;beginCatalog();return false;}
    if(tablePhase==1&&!itemsIndexed){indexCatalogItems();return false;}
    unsigned count=0;auto start=std::chrono::steady_clock::now();
    while(tableCursor<static_cast<unsigned>(rows.GetMaxIndex())&&count++<16){
        auto id=FSetElementId::FromInteger(tableCursor++);if(!rows.IsValidId(id))continue;const auto& pair=rows.Get(id);auto row=pair.Value;if(!row)continue;
        int slot=tablePhase?4:slotFor(row);if(slot<0)continue;
        Look look{static_cast<Slot>(slot),pair.Key.ToString(),{}, {}};look.label=cleanName(look.row);
        auto item=readObject(property(type,L"Item"),row);
        if(!item&&tablePhase==1){auto found=catalogItems.find(look.row);if(found!=catalogItems.end())item=found->second.get();}
        if(item){look.item=Ref(item);look.itemPath=item->GetPathName();auto name=text(property(item,L"ItemName"),item);if(!name.empty())look.label=name;itemLookup[item]={look.item,look.row};}
        look.hasPreviewIcon=hasPreviewIcon(item);look.sortKey=lookSortKey(look.label);
        runtime.looks.push_back(std::move(look));
        if(std::chrono::steady_clock::now()-start>std::chrono::microseconds(500))break;
    }
    if(logging)++runtime.catalogSteps;
    if(tableCursor>=static_cast<unsigned>(rows.GetMaxIndex())){
        if(tablePhase==0&&weaponTable){tablePhase=1;tableCursor=0;expectedRows=table(true)->GetRowMap().Num();}
        else complete();
    }
    return runtime.catalogReady;
}
void requestRefresh(bool player,bool preview){runtime.playerRefresh|=player;runtime.previewRefresh|=preview;runtime.refreshWork.request(runtime.now);}
bool inventorySource(UObject* owner){return runtime.inventory.matches(owner)||runtime.storage.matches(owner);}
void requestInventoryScan(){if(!inventoryDirty){inventoryDirty=true;inventoryDue=runtime.now+100;}}
void inventoryTransferred(UObject* system){if(inventorySystem.matches(system))requestInventoryScan();}
void inventoryChanged(bool equipment){if(equipment){requestInventoryScan();requestRefresh(false,bool(runtime.doll));}}
void inventoryAdded(UObject* owner,UFunction* fn,FFrame& frame){
    if(!inventorySource(owner))return;
    auto p=property(static_cast<UStruct*>(fn),L"ItemHandle");
    if(!inventorySystem||!p||!p->IsA<FStructProperty>()||p->GetSize()!=sizeof(uint32_t)||static_cast<FStructProperty*>(p)->GetStruct()->GetName()!=L"ItemHandle"){
        warn(L"Collection pickup capture needs a four-byte ItemHandle; scheduling an inventory scan.");requestInventoryScan();return;
    }
    // Const references are OutParm parameters too. Locals may contain only an
    // initialized sentinel; copy from the caller-owned address during the hook.
    auto data=(p->GetPropertyFlags()&CPF_OutParm)?FindOutParamValueAddress(frame,p):
        (frame.Locals()?p->ContainerPtrToValuePtr<void>(frame.Locals()):nullptr);
    if(!data){warn(L"Collection pickup reference unavailable; scheduling an inventory scan.");requestInventoryScan();return;}
    uint32_t handle{};std::memcpy(&handle,data,sizeof(handle));
    for(const auto& entry:addedItems)if(entry.owner.matches(owner)&&entry.handle==handle)return;
    if(addedItems.size()<64)addedItems.push_back({Ref(owner),handle});else requestInventoryScan();
}
bool learnInventoryItem(UObject* item){
    if(!item)return false;
    auto it=itemLookup.find(item);
    if(it!=itemLookup.end()&&it->second.item.matches(item))return runtime.model.learn(it->second.row);
    // Both incremental updates and scans see live, currently owned objects.
    // Weapons loaded after catalog indexing use the same exact identity rule.
    auto name=item->GetName();
    if(item->GetPathName()!=L"/Game/_Dawnwalker/Inventory/Items/"+name+L"."+name||!lookup(true,name))return false;
    itemLookup[item]={Ref(item),name};return runtime.model.learn(name);
}
bool catalogPending(){return catalogStarted&&!runtime.catalogReady;}
bool inventoryPending(){return runtime.catalogReady&&(!addedItems.empty()||inventoryScanning||inventoryDirty);}
void stepInventory(){
    const auto generation=inventoryGeneration;
    if(!runtime.inventory){inventoryScanning=false;scanOwner={};inventoryItems.clear();inventoryCaptured=false;addedItems.clear();inventoryDirty=false;return;}
    if(!addedItems.empty()){
        auto start=std::chrono::steady_clock::now();unsigned count=0;bool learned=false;
        while(!addedItems.empty()&&count++<8){
            auto entry=addedItems.front();addedItems.pop_front();auto owner=entry.owner.get();
            if(!owner||!inventorySource(owner))continue;
            Call quantity(owner,L"GetItemQuantity"),resolve(inventorySystem.get(),L"GetAssetForItem");
            auto source=quantity.field(L"Item"),target=resolve.field(L"Handle");
            if(!quantity||!resolve||!source||!target||source->GetSize()!=4||target->GetSize()!=4||!source->IsA<FStructProperty>()||!target->IsA<FStructProperty>()||static_cast<FStructProperty*>(source)->GetStruct()!=static_cast<FStructProperty*>(target)->GetStruct()){
                warn(L"Collection item resolution needs matching four-byte handles; scheduling an inventory scan.");requestInventoryScan();addedItems.clear();break;
            }
            std::memcpy(quantity.value(L"Item"),&entry.handle,4);quantity.num(L"bMatchAssetOnly",0).invoke();
            // Ownership is authoritative, including a failed addition of an
            // already owned item. Failed additions never unlock unowned looks.
            if(generation!=inventoryGeneration||!entry.owner.matches(owner)||!inventorySource(owner)){if(learned){runtime.dirty=true;menuRedraw();}return;}
            if(quantity.resultInteger()>0){
                std::memcpy(resolve.value(L"Handle"),&entry.handle,4);resolve.invoke();
                if(generation!=inventoryGeneration||!entry.owner.matches(owner)||!inventorySource(owner)){if(learned){runtime.dirty=true;menuRedraw();}return;}
                learned|=learnInventoryItem(resolve.resultObject());
            }
            if(logging)++runtime.inventoryItems;
            if(std::chrono::steady_clock::now()-start>std::chrono::microseconds(500))break;
        }
        if(learned){runtime.dirty=true;menuRedraw();}return;
    }
    if(!inventoryScanning){
        if(!inventoryDirty||runtime.now<inventoryDue)return;
        inventoryDirty=false;inventoryScanning=true;inventoryPhase=inventoryCursor=0;scanOwner=runtime.inventory;
        Call storage(inventorySystem.get(),L"GetPlayerStorageComponent");runtime.storage={};
        if(storage){storage.invoke();if(generation!=inventoryGeneration||!scanOwner.matches(runtime.inventory.get()))return;runtime.storage=Ref(storage.resultObject());}
        if(logging)++runtime.inventorySnapshots;
        return; // Source acquisition and traversal have separate frame budgets.
    }
    auto advance=[] {inventoryCursor=0;inventoryItems.clear();inventoryCaptured=false;if(++inventoryPhase>=2){inventoryScanning=false;scanOwner={};if(logging)trace(L"Collection scan complete: known appearances="+std::to_wstring(runtime.model.collected.size())+L", stash="+(runtime.storage?L"available":L"unavailable"));}else scanOwner=runtime.storage;};
    auto owner=scanOwner.get();if(!owner||!inventorySource(owner)){advance();return;}
    if(!inventoryCaptured){
        // InventoryItems and EquipmentSlots are authored starting contents.
        // GetCurrentItems reads the live inventory, including equipped items.
        // Its owned return buffer dies here; only deletion-aware identities
        // survive to the bounded learning slices on subsequent frames.
        Call snapshot(owner,L"GetCurrentItems");auto result=snapshot?snapshot.fn->GetReturnProperty():nullptr;
        auto array=result&&result->IsA<FArrayProperty>()?static_cast<FArrayProperty*>(result):nullptr;
        auto inner=array?array->GetInner():nullptr;
        auto type=inner&&inner->IsA<FStructProperty>()?static_cast<FStructProperty*>(inner)->GetStruct():nullptr;
        auto item=type?property(type,L"ItemDataAsset"):nullptr,quantity=type?property(type,L"Quantity"):nullptr;
        if(!item||!item->IsA<FObjectProperty>()||!quantity||!quantity->IsA<FNumericProperty>()||static_cast<FNumericProperty*>(quantity)->IsFloatingPoint()){
            warn(L"Collection query needs item references and integer quantities; skipping this inventory.");advance();return;
        }
        snapshot.invoke();
        if(generation!=inventoryGeneration||!scanOwner.matches(owner)||!inventorySource(owner))return;
        FScriptArrayHelper values(array,result->ContainerPtrToValuePtr<void>(snapshot.data()));
        if(values.Num()<0||values.Num()>16384){warn(L"Collection inventory exceeds the supported safety limit.");advance();return;}
        inventoryItems.clear();inventoryItems.reserve(values.Num());
        for(int i=0;i<values.Num();++i){auto value=values.GetRawPtr(i);if(integer(quantity,value)>0)if(auto object=readObject(item,value))inventoryItems.emplace_back(object);}
        inventoryCaptured=true;
        if(logging)trace(L"Collection live inventory: source="+std::wstring(inventoryPhase?L"stash":L"player")+L", items="+std::to_wstring(inventoryItems.size()));
        return; // One inventory query/capture or learning slice per frame.
    }
    bool learned=false;auto start=std::chrono::steady_clock::now();unsigned count=0;
    while(inventoryCursor<inventoryItems.size()&&count++<32){
        learned|=learnInventoryItem(inventoryItems[inventoryCursor++].get());
        if(logging)++runtime.inventoryItems;
        if(generation!=inventoryGeneration||!scanOwner.matches(owner))return;
        if(std::chrono::steady_clock::now()-start>std::chrono::microseconds(500))break;
    }
    if(learned){runtime.dirty=true;menuRedraw();}
    if(inventoryCursor>=inventoryItems.size())advance();
}
bool refresh(){
    MeasureOperation timing(Operation::Refresh);
    if(insideRefresh)return true;auto app=runtime.appearance.get();if(!app)return false;
    insideRefresh=true;
    try{
        runtime.activeSet=loadout();restoreMeshes();prepareRows();
        // Rebuild cosmetics through the reflected appearance API. Equipment and
        // active loadout are never changed to refresh a preview.
        if(runtime.playerRefresh){Call c(app,L"OnInventoryContentsChanged");if(!c||c.fn->GetParmsSize()!=0){insideRefresh=false;warn(L"Appearance refresh signature unavailable.");return true;}c.invoke();}
        if(runtime.previewRefresh)if(auto doll=runtime.dollAppearance.get()){Call preview(doll,L"OnInventoryContentsChanged");if(preview&&preview.fn->GetParmsSize()==0)preview.invoke();}
        sheathedAppearance(app,false);sheathedAppearance(runtime.player.get(),false);sheathedAppearance(runtime.doll.get(),true);
        observeMainWeapon(runtime.player.get());observeMainWeapon(runtime.doll.get());
        for(auto& w:weapons)if(auto actor=w.get())weaponAppearance(actor);
        runtime.playerRefresh=runtime.previewRefresh=false;if(logging)++runtime.refreshes;insideRefresh=false;return true;
    }catch(...){insideRefresh=false;warn(L"Appearance refresh could not complete; normal equipment remains intact.");return false;}
}
void selectLook(Slot slot,const Choice& c){if(runtime.model.choose(slot,c)){runtime.dirty=true;requestRefresh(runtime.model.displayedSet==runtime.activeSet,true);if(logging)trace(L"Look selected: set="+std::to_wstring(runtime.model.displayedSet)+L", slot="+slotNames[static_cast<unsigned>(slot)]+L", mode="+std::to_wstring(static_cast<int>(c.mode))+L", row="+c.row);}}
void setDoll(UObject* p){runtime.doll=Ref(p);runtime.dollAppearance=Ref(object(p,L"AppearanceComponent"));requestRefresh(false,true);}
void observeWeapon(UObject* p){
    if(!p||!weaponOwner(p)||!physicalWeapon(p))return;
    watchWeaponEvents(p);
    if(logging)++runtime.weaponEvents;
    bool known=false;for(auto& r:weapons)if(r.matches(p)){known=true;break;}
    if(!known){
        bool retained=false;
        for(auto& r:weapons)if(!r||!weaponOwner(r.get())){r=Ref(p);retained=true;break;}
        if(!retained){warn(L"Too many active player weapons; the new drawn appearance could not be retained.");return;}
    }
    // Appearance completion can happen repeatedly on the same pooled actor.
    // Revisit it after the game finishes writing, even if already observed.
    // Events during our refresh are consumed by its final weapon pass.
    if(!insideRefresh)runtime.refreshWork.request(runtime.now);
}
}
