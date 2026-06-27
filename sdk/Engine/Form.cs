using System.Collections.Generic;
using Recreation.Interop;

namespace Recreation;

// The base of every engine object the API exposes: a thin, reference-typed
// handle to a Bethesda form (Skyrim, Fallout, ...). All the concrete kinds
// (ObjectReference, Actor, Quest, Faction, ...) derive from this and add the
// calls that make sense for them, mirroring the Papyrus type hierarchy modders
// already know.
//
// A Form is cheap: it carries only a handle and dispatches each call to the
// engine. Identity is the handle, so two Forms wrapping the same object compare
// equal. Handle 0 is the null form (Exists == false), the analog of Papyrus None.
public class Form
{
    public ulong Handle { get; }

    protected Form(ulong handle) => Handle = handle;

    public static readonly Form None = new(0);

    // Wraps a raw handle as a plain Form. Concrete kinds expose their own typed
    // factories; this is the fallback when only a handle is known.
    public static Form From(ulong handle) => new(handle);

    public bool Exists => Handle != 0;

    // The form's runtime (load-order) form id.
    public uint FormId => (uint)Call("GetFormID").AsInt();

    // The display name (FULL record), empty if the form has none.
    public string Name => Call("GetName").AsString();

    // The Papyrus form-type code (record signature class). See the engine's form
    // type table; common values: Actor, Weapon, Armor, ...
    public int FormType => Call("GetType").AsInt();

    public bool HasKeyword(Form keyword) => Call("HasKeyword", keyword).AsBool();

    // Every keyword on the form (its KWDA), resolved. Mods filter by these to
    // categorise forms: weapon types, vendor categories, armor materials, edible
    // food, and so on. Empty if the form carries none.
    public IReadOnlyList<Form> Keywords
    {
        get
        {
            int count = Call("GetKeywordCount").AsInt();
            var result = new Form[count];
            for (int i = 0; i < count; i++)
                result[i] = From(Call("GetNthKeyword", i).AsHandle());
            return result;
        }
    }

    // Item weight and gold value from the form's record. Available for the common
    // inventory item types (weapons, armor, misc, ingredients, soul gems, keys,
    // potions); other forms report 0.
    public float Weight => Call("GetWeight").AsFloat();
    public int GoldValue => Call("GetGoldValue").AsInt();

    // Base physical damage if this form is a weapon, else 0.
    public int WeaponDamage => Call("GetWeaponDamage").AsInt();

    // Base armor rating if this form is armor, else 0.
    public float ArmorRating => Call("GetArmorRating").AsFloat();

    // The item a harvestable flora produces (its FLOR produce), or Form.None.
    public Form HarvestIngredient => Form.From(Call("GetHarvestIngredient").AsHandle());

    // The enchantment on a weapon or armor (its EITM), or a null Enchantment if
    // unenchanted.
    public Enchantment Enchantment => Recreation.Enchantment.From(Call("GetEnchantment").AsHandle());
    public bool IsEnchanted => Enchantment.Exists;

    // True if the engine resolves this handle to a script instance of typeName
    // or one of its ancestors.
    public bool Is(string typeName)
    {
        string actual = Native.TypeOf(Handle);
        return string.Equals(actual, typeName, System.StringComparison.OrdinalIgnoreCase);
    }

    // Dispatch helpers used by this class and its subclasses. Every engine call
    // funnels through here, keeping the native boundary in one place.
    protected Value Call(string function, params System.ReadOnlySpan<Value> args) =>
        Native.CallMethod(Handle, function, args);

    public Value GetProperty(string name) => Native.GetProperty(Handle, name);
    public void SetProperty(string name, Value value) => Native.SetProperty(Handle, name, value);

    public static implicit operator Value(Form? form) => Value.Object(form?.Handle ?? 0);

    public override bool Equals(object? obj) => obj is Form f && f.Handle == Handle;
    public override int GetHashCode() => Handle.GetHashCode();
    public override string ToString() => $"{GetType().Name}(0x{Handle:X})";
}
