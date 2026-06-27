using System.Collections.Generic;
using System.Linq;

namespace Recreation.Modding;

// Attaches and tracks FormBehaviours by the form they run on, the component
// registry behind the per-form scripting model. A mod attaches a behaviour to a
// form (by handle or wrapper); the host drives it through the normal lifecycle,
// and this keeps the form -> behaviours index so they can be found and detached
// together (when the form unloads, say).
public static class FormScripts
{
    private static readonly Dictionary<ulong, List<FormBehaviour>> ByForm = new();

    // Attaches a fresh behaviour of type T to form and starts it. Returns the
    // instance so the caller can configure it further.
    public static T Attach<T>(Form form) where T : FormBehaviour, new()
    {
        var behaviour = new T { Self = form };
        if (!ByForm.TryGetValue(form.Handle, out var list))
        {
            list = new List<FormBehaviour>();
            ByForm[form.Handle] = list;
        }
        list.Add(behaviour);
        ModHost.Register(behaviour);
        return behaviour;
    }

    // The behaviours currently attached to a form (empty if none).
    public static IReadOnlyList<FormBehaviour> For(Form form) =>
        ByForm.TryGetValue(form.Handle, out var list) ? list : System.Array.Empty<FormBehaviour>();

    // Detaches and stops every behaviour on a form (e.g. when it unloads).
    public static void DetachAll(Form form)
    {
        if (!ByForm.Remove(form.Handle, out var list)) return;
        foreach (FormBehaviour b in list) ModHost.Unregister(b);
    }

    // Detaches and stops a single behaviour.
    public static void Detach(FormBehaviour behaviour)
    {
        if (ByForm.TryGetValue(behaviour.Self.Handle, out var list))
        {
            list.Remove(behaviour);
            if (list.Count == 0) ByForm.Remove(behaviour.Self.Handle);
        }
        ModHost.Unregister(behaviour);
    }

    // Total attached behaviours across all forms.
    public static int Count => ByForm.Values.Sum(l => l.Count);

    // Drops all attachments. Used on managed-world teardown.
    public static void Clear() => ByForm.Clear();
}
