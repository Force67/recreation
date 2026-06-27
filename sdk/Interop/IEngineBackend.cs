using System;

namespace Recreation.Interop;

// The seam between the SDK and whatever serves engine calls. The production
// backend marshals across the native bridge; tests supply a fake that returns
// canned values, so game logic written against Game/Form/Actor can be exercised
// without a running engine. Every engine wrapper funnels through Native, which
// forwards to the bound backend.
public interface IEngineBackend
{
    bool IsScriptLoaded(string type);
    bool LoadScript(string type);
    ulong CreateInstance(string type);
    string TypeOf(ulong handle);

    Value CallGlobal(string type, string function, ReadOnlySpan<Value> args);
    Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args);

    Value GetProperty(ulong self, string name);
    void SetProperty(ulong self, string name, Value value);

    void Tick(float deltaTime);
}
