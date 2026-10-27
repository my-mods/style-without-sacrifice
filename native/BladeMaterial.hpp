#pragma once
#include "Reflection.hpp"
#include <cmath>
#include <map>

namespace Wardrobe {
// The weapon Blueprint stores its blood-effect MID separately from BaseMesh.
// A mesh replacement must rebind both; changing only the mesh retains the old
// blade's texture maps. Never change shared materials or weapon class defaults.
struct BladeMaterial {
    inline static bool releasePins=true;
    struct Pin {
        Ref object;bool added{};
        explicit Pin(UObject* value):object(value){if(object&&!value->IsRootSet()){value->SetRootSet();added=true;}}
        ~Pin(){if(releasePins&&added)if(auto value=object.get())value->ClearRootSet();}
    };
    inline static std::map<UObject*,std::weak_ptr<Pin>> pins;
    static std::shared_ptr<Pin> pin(UObject* value){
        if(!value)return {};
        for(auto i=pins.begin();i!=pins.end();)if(i->second.expired())i=pins.erase(i);else ++i;
        auto& weak=pins[value];auto held=weak.lock();
        if(!held){held=std::make_shared<Pin>(value);weak=held;}return held;
    }
    // Original MIDs otherwise lose their last engine reference during the swap.
    // Shared leases retain them until restoration; pre-existing roots are kept.
    // Loader shutdown abandons leases without touching engine state.
    static void abandonPins(){releasePins=false;}
    std::shared_ptr<Pin> originalPin,dynamicPin;
    Ref actor, component, original, originalDynamic, installed;
    static bool hard(FProperty* p){return p&&p->IsA<FObjectProperty>()&&p->GetSize()==sizeof(void*);}
    static bool index(FProperty* p){return p&&p->IsA<FNumericProperty>()&&!static_cast<FNumericProperty*>(p)->IsFloatingPoint();}
    static bool scalar(FProperty* p){return p&&p->IsA<FNumericProperty>()&&static_cast<FNumericProperty*>(p)->IsFloatingPoint();}
    static bool name(FProperty* p){return p&&p->IsA<FNameProperty>()&&p->GetSize()==sizeof(FName);}
    static bool settable(UObject* mesh){Call c(mesh,L"SetMaterial");return c&&index(c.field(L"ElementIndex"))&&hard(c.field(L"Material"));}
    static bool overrideAtZero(UObject* mesh,UObject*& value){
        auto p=property(mesh,L"OverrideMaterials");
        if(!p||!p->IsA<FArrayProperty>())return false;
        auto array=static_cast<FArrayProperty*>(p);if(!hard(array->GetInner()))return false;
        FScriptArrayHelper values(array,p->ContainerPtrToValuePtr<void>(mesh));
        value=values.Num()?readObject(array->GetInner(),values.GetRawPtr(0)):nullptr;return true;
    }
    static void set(UObject* mesh,UObject* material){Call c(mesh,L"SetMaterial");c.num(L"ElementIndex",0).obj(L"Material",material).invoke();}
    static bool blood(UObject* from,UObject* to,bool copy){
        if(!from||!to)return false;
        Call get(from,L"K2_GetScalarParameterValue"),put(to,L"SetScalarParameterValue");
        auto result=get?get.fn->GetReturnProperty():nullptr;
        if(!get||!put||!name(get.field(L"ParameterName"))||!scalar(result)||
           !name(put.field(L"ParameterName"))||!scalar(put.field(L"Value")))return false;
        if(copy){get.str(L"ParameterName",L"BLADE BLOOD LEVEL").invoke();
            auto value=static_cast<FNumericProperty*>(result)->GetFloatingPointPropertyValue(result->ContainerPtrToValuePtr<void>(get.data()));
            if(!std::isfinite(value))return false;
            put.str(L"ParameterName",L"BLADE BLOOD LEVEL").num(L"Value",value).invoke();}
        return true;
    }
    bool intact()const{
        UObject* value{};return actor&&component&&installed&&
            object(actor.get(),L"WeaponDynMat")==installed.get()&&
            overrideAtZero(component.get(),value)&&value==installed.get();
    }
    void restore(bool ownsMesh){
        auto a=actor.get(),c=component.get(),mid=installed.get();UObject* current{};
        if(!mid)return;
        // Release the Blueprint reference even if another writer replaced the
        // mesh. Component restoration additionally requires mesh ownership.
        if(a&&object(a,L"WeaponDynMat")==mid&&(!originalDynamic.address||originalDynamic)){
            if(originalDynamic)blood(mid,originalDynamic.get(),true);
            assignObject(property(a,L"WeaponDynMat"),a,originalDynamic.get());
        }
        if(ownsMesh&&c&&overrideAtZero(c,current)&&current==mid&&(!original.address||original)&&settable(c))set(c,original.get());
    }
    bool apply(UObject* a,UObject* c,UObject* replacement){
        auto field=property(a,L"WeaponDynMat");UObject* prior{};
        Call material(replacement,L"GetMaterial"),create(c,L"CreateDynamicMaterialInstance");
        if(!hard(field)||!overrideAtZero(c,prior)||!settable(c)||!material||
           !index(material.field(L"MaterialIndex"))||!hard(material.fn->GetReturnProperty())||!create||
           !index(create.field(L"ElementIndex"))||!hard(create.field(L"SourceMaterial"))||
           !hard(create.fn->GetReturnProperty()))return false;
        material.num(L"MaterialIndex",0).invoke();auto parent=material.resultObject();
        auto previous=object(a,L"WeaponDynMat");
        // Preserve the current blood scalar, but never copy the old texture or
        // vector parameters onto a different sword's authored material.
        if(!parent||!previous||!blood(previous,previous,false))return false;
        originalPin=pin(prior);dynamicPin=pin(previous);
        actor=Ref(a);component=Ref(c);original=Ref(prior);originalDynamic=Ref(previous);
        create.num(L"ElementIndex",0).obj(L"SourceMaterial",parent).invoke();auto mid=create.resultObject();
        if(!mid){set(c,prior);return false;}
        installed=Ref(mid);
        if(!blood(previous,mid,true)){restore(true);return false;}
        assignObject(field,a,mid);
        if(!intact()){restore(true);return false;}
        return true;
    }
};
}
