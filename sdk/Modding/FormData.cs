using System.Collections.Generic;

namespace Recreation.Modding;

// Arbitrary per-form data a mod attaches at runtime: the analog of gmod's entity
// table or networked vars. Associate any value with a form by a string key and
// read it back typed. Data is dropped when the form unloads (the mod host wires
// FormUnloaded to ClearForm) or the world tears down, so per-object state cannot
// leak onto a stale handle.
//
//   FormData.Set(chest, "looted", true);
//   if (FormData.Get<bool>(chest, "looted")) return;  // already emptied
//
public static class FormData
{
    private static readonly Dictionary<ulong, Dictionary<string, object>> Store = new();

    // Attaches `value` to `form` under `key`, replacing any existing value.
    public static void Set(Form form, string key, object value)
    {
        if (!Store.TryGetValue(form.Handle, out Dictionary<string, object>? fields))
        {
            fields = new Dictionary<string, object>();
            Store[form.Handle] = fields;
        }
        fields[key] = value;
    }

    // The value attached to `form` under `key`, or `fallback` if it is absent or
    // not a T.
    public static T? Get<T>(Form form, string key, T? fallback = default) =>
        Store.TryGetValue(form.Handle, out Dictionary<string, object>? fields) &&
        fields.TryGetValue(key, out object? value) && value is T typed
            ? typed
            : fallback;

    public static bool Has(Form form, string key) =>
        Store.TryGetValue(form.Handle, out Dictionary<string, object>? fields) &&
        fields.ContainsKey(key);

    // Removes one key from a form (a no-op if absent).
    public static void Remove(Form form, string key)
    {
        if (!Store.TryGetValue(form.Handle, out Dictionary<string, object>? fields)) return;
        fields.Remove(key);
        if (fields.Count == 0) Store.Remove(form.Handle);
    }

    // Drops all data attached to a form (called when it unloads).
    public static void ClearForm(Form form) => Store.Remove(form.Handle);

    // The number of forms currently carrying data.
    public static int TrackedForms => Store.Count;

    // Drops everything. Used on managed-world teardown.
    public static void Clear() => Store.Clear();
}
