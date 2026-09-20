using StreamedDepLib;

namespace StreamedUsesDep;

// Calls across the assembly boundary into StreamedDepLib. Invoking this through
// reflection forces the runtime to resolve StreamedDepLib, which is exactly the
// moment the loader's Resolving probe has to deliver it.
public static class DepCaller
{
    public static string Call() => DepValue.Value();
}
