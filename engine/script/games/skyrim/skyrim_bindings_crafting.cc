// Constructible-object (COBJ) recipe accessors for RecordBackedSkyrimBindings.
// COBJ is the one record behind every Skyrim crafting station -- smithing,
// cooking, tempering, tanning -- so parsing it once exposes the whole crafting
// surface to the managed (C#) crafting logic. Split from skyrim_bindings.cc to
// keep that unit from growing into a god file.
#include <cstring>
#include <utility>

#include "bethesda/record.h"
#include "script/games/skyrim/skyrim_bindings.h"

namespace rx::script::skyrim {

using papyrus::ObjectRef;

void RecordBackedSkyrimBindings::BuildRecipes() {
  recipes_built_ = true;
  recipe_cache_.clear();
  if (!records_) return;
  // A COBJ lists its inputs as repeated CNTO ({ formid; uint32 count }) entries,
  // then names the output (CNAM), the workbench keyword (BNAM) and the output
  // quantity (NAM1 uint16). CTDA conditions and COED component data are skipped.
  // Every form id is local to the recipe's plugin, so resolve it against that
  // plugin's masters for a global handle the managed side can match.
  records_->EachOfType(
      FourCc('C', 'O', 'B', 'J'),
      [&](bethesda::GlobalFormId id, const bethesda::RecordStore::StoredRecord& stored) {
        bethesda::Record rec;
        if (!records_->Parse(id, &rec)) return;
        Recipe recipe;
        for (const bethesda::Subrecord& sub : rec.subrecords) {
          if (sub.type == FourCc('C', 'N', 'T', 'O') && sub.data.size() >= 8) {
            u32 raw, count;
            std::memcpy(&raw, sub.data.data(), 4);
            std::memcpy(&count, sub.data.data() + 4, 4);
            recipe.inputs.push_back(
                {records_->ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin).packed(),
                 static_cast<i32>(count)});
          } else if (sub.type == FourCc('C', 'N', 'A', 'M') && sub.data.size() >= 4) {
            u32 raw;
            std::memcpy(&raw, sub.data.data(), 4);
            recipe.output =
                records_->ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin).packed();
          } else if (sub.type == FourCc('B', 'N', 'A', 'M') && sub.data.size() >= 4) {
            u32 raw;
            std::memcpy(&raw, sub.data.data(), 4);
            recipe.workbench =
                records_->ResolveFrom(bethesda::RawFormId{raw}, stored.winning_plugin).packed();
          } else if (sub.type == FourCc('N', 'A', 'M', '1') && sub.data.size() >= 2) {
            u16 quantity;
            std::memcpy(&quantity, sub.data.data(), 2);
            recipe.output_count = quantity;
          }
        }
        if (recipe.output != 0) recipe_cache_.push_back(std::move(recipe));
      });
}

i32 RecordBackedSkyrimBindings::GetRecipeCount() {
  if (!recipes_built_) BuildRecipes();
  return static_cast<i32>(recipe_cache_.size());
}

const RecordBackedSkyrimBindings::Recipe* RecordBackedSkyrimBindings::RecipeAt(i32 index) const {
  if (index < 0 || static_cast<size_t>(index) >= recipe_cache_.size()) return nullptr;
  return &recipe_cache_[static_cast<size_t>(index)];
}

ObjectRef RecordBackedSkyrimBindings::GetNthRecipeOutput(i32 recipe) {
  const Recipe* r = RecipeAt(recipe);
  return r ? ObjectRef{r->output} : ObjectRef{};
}

i32 RecordBackedSkyrimBindings::GetNthRecipeOutputQuantity(i32 recipe) {
  const Recipe* r = RecipeAt(recipe);
  return r ? r->output_count : 0;
}

ObjectRef RecordBackedSkyrimBindings::GetNthRecipeWorkbench(i32 recipe) {
  const Recipe* r = RecipeAt(recipe);
  return r ? ObjectRef{r->workbench} : ObjectRef{};
}

i32 RecordBackedSkyrimBindings::GetNthRecipeInputCount(i32 recipe) {
  const Recipe* r = RecipeAt(recipe);
  return r ? static_cast<i32>(r->inputs.size()) : 0;
}

ObjectRef RecordBackedSkyrimBindings::GetNthRecipeInput(i32 recipe, i32 input) {
  const Recipe* r = RecipeAt(recipe);
  if (!r || input < 0 || static_cast<size_t>(input) >= r->inputs.size()) return {};
  return ObjectRef{r->inputs[static_cast<size_t>(input)].item};
}

i32 RecordBackedSkyrimBindings::GetNthRecipeInputQuantity(i32 recipe, i32 input) {
  const Recipe* r = RecipeAt(recipe);
  if (!r || input < 0 || static_cast<size_t>(input) >= r->inputs.size()) return 0;
  return r->inputs[static_cast<size_t>(input)].count;
}

}  // namespace rx::script::skyrim
