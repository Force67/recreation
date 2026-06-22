using Recreation.Modding;

namespace Recreation.Tests;

// A behaviour that records its lifecycle, for asserting the host drives it right.
public sealed class CountingBehaviour : GameBehaviour
{
    public int Starts;
    public int Updates;
    public int Destroys;
    public float LastDelta;

    protected override void OnStart() => Starts++;
    protected override void OnUpdate(float dt)
    {
        Updates++;
        LastDelta = dt;
    }
    protected override void OnDestroy() => Destroys++;
}

// A sample mod + auto-start behaviour, discovered by LoadFrom over this assembly.
public sealed class SampleMod : IMod
{
    public static int LoadCount;
    public void OnLoad() => LoadCount++;
}

[AutoStart]
public sealed class SampleAutoBehaviour : GameBehaviour
{
    public static int StartCount;
    protected override void OnStart() => StartCount++;
}

public static class ModHostTests
{
    public static void Run(Check check)
    {
        ModHost.Shutdown();

        var b = new CountingBehaviour();
        ModHost.Register(b);
        check.Equal("register starts the behaviour", 1, b.Starts);

        ModHost.Tick(0.5f);
        ModHost.Tick(0.25f);
        check.Equal("tick updates the behaviour", 2, b.Updates);
        check.Equal("tick passes the delta", 0.25f, b.LastDelta);

        b.Enabled = false;
        ModHost.Tick(1.0f);
        check.Equal("disabled behaviour skips update", 2, b.Updates);

        ModHost.Unregister(b);
        check.Equal("unregister destroys the behaviour", 1, b.Destroys);
        ModHost.Tick(1.0f);
        check.Equal("unregistered behaviour gets no updates", 2, b.Updates);

        // Discovery over this test assembly: the sample mod loads, the auto-start
        // behaviour starts.
        SampleMod.LoadCount = 0;
        SampleAutoBehaviour.StartCount = 0;
        ModHost.LoadFrom(new[] { typeof(ModHostTests).Assembly });
        check.Equal("IMod discovered and loaded", 1, SampleMod.LoadCount);
        check.Equal("auto-start behaviour started", 1, SampleAutoBehaviour.StartCount);

        ModHost.Shutdown();
        check.Equal("shutdown clears behaviours", 0, ModHost.ActiveBehaviours.Count);
    }
}
