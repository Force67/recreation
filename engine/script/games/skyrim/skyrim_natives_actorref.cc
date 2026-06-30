#include <vector>

#include "script/games/skyrim/skyrim_natives_ext.h"

namespace rec::script::skyrim {

using papyrus::ObjectRef;
using papyrus::Value;
using papyrus::VirtualMachine;
using ext::Args;
using ext::Resolve;

void RegisterActorRefGetters(papyrus::NativeRegistry& reg, SkyrimBindings* bindings) {
  reg.Register("Actor", "GetActorValueMax", [bindings](VirtualMachine&, ObjectRef self, Args& a) {
    return Value::Float(Resolve(bindings).GetBaseActorValue(self, ext::ArgS(a, 0)));
  });

  reg.Register("ObjectReference", "IsContainerEmpty",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 return Value::Bool(Resolve(bindings).GetNumItems(self) == 0);
               });

  reg.Register("ObjectReference", "GetAllItemsCount",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 auto& b = Resolve(bindings);
                 i32 total = 0;
                 for (i32 i = 0, n = b.GetNumItems(self); i < n; ++i) {
                   ObjectRef form = b.GetNthForm(self, i);
                   total += b.GetItemCount(self, form);
                 }
                 return Value::Int(total);
               });

  reg.Register("ObjectReference", "RemoveAllItems",
               [bindings](VirtualMachine&, ObjectRef self, Args&) {
                 auto& b = Resolve(bindings);
                 // Removing items shifts the inventory indices, so collect every
                 // form up front and then remove each, rather than iterating live.
                 std::vector<ObjectRef> forms;
                 for (i32 i = 0, n = b.GetNumItems(self); i < n; ++i) {
                   forms.push_back(b.GetNthForm(self, i));
                 }
                 for (ObjectRef form : forms) {
                   b.RemoveItem(self, form, b.GetItemCount(self, form));
                 }
                 return Value();
               });
}

}  // namespace rec::script::skyrim
