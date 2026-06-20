using System;
using System.Collections.Generic;
using System.Reflection;

namespace Recreation.Modding;

// The runtime that hosts user mods. It boots the mod surface, drives the
// per-frame lifecycle, and tears everything down cleanly. The engine calls
// Boot() once when the managed world comes up, Tick(dt) each frame, and
// Shutdown() on teardown.
//
// Single-threaded: every call runs on the host thread, the same one that drives
// the guest, so the registry needs no locking.
public static class ModHost
{
    private static readonly List<GameBehaviour> Behaviours = new();
    private static readonly List<IMod> Mods = new();
    private static bool _booted;

    public static IReadOnlyList<GameBehaviour> ActiveBehaviours => Behaviours;
    public static bool Booted => _booted;

    // Discovers and loads every mod in the currently loaded assemblies, then
    // starts the auto-start behaviours. Idempotent.
    public static void Boot()
    {
        if (_booted) return;
        _booted = true;
        Console.WriteLine("[managed] mod host booting");
        // Complete the per-form component lifecycle: when a form unloads, detach
        // the behaviours attached to it so they stop ticking on a stale handle.
        // The subscription is cleared by Shutdown (EventBus.Clear) and re-added on
        // the next Boot.
        EventBus.Subscribe<FormUnloaded>(e => FormScripts.DetachAll(e.Form));
        EventBus.Subscribe<FormUnloaded>(e => FormData.ClearForm(e.Form));
        LoadFrom(AppDomain.CurrentDomain.GetAssemblies());
    }

    // Discovers and loads the mods declared in the given assemblies. Exposed so
    // the host can load external mod assemblies after boot.
    public static void LoadFrom(IEnumerable<Assembly> assemblies)
    {
        var list = new List<Assembly>(assemblies);
        foreach (Type modType in ModDiscovery.FindMods(list))
        {
            if (Activator.CreateInstance(modType) is not IMod mod) continue;
            var meta = modType.GetCustomAttribute<ModAttribute>();
            Console.WriteLine($"[managed] loading mod {meta?.Name ?? modType.Name}");
            Mods.Add(mod);
            try
            {
                mod.OnLoad();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"[managed] mod {modType.Name} OnLoad threw: {ex.Message}");
            }
        }

        foreach (Type behaviourType in ModDiscovery.FindAutoStartBehaviours(list))
        {
            if (Activator.CreateInstance(behaviourType) is GameBehaviour behaviour)
                Register(behaviour);
        }
    }

    // Registers a behaviour and starts it. Mods call this from OnLoad, or any
    // time after, to bring a behaviour online.
    public static void Register(GameBehaviour behaviour)
    {
        ArgumentNullException.ThrowIfNull(behaviour);
        Behaviours.Add(behaviour);
        behaviour.DispatchStart();
    }

    // Stops and removes a behaviour.
    public static void Unregister(GameBehaviour behaviour)
    {
        if (Behaviours.Remove(behaviour))
            behaviour.DispatchDestroy();
    }

    // Advances every active behaviour and publishes the frame event. Called once
    // per frame by the engine.
    public static void Tick(float deltaTime)
    {
        Time.Advance(deltaTime);
        Scheduler.Advance(deltaTime);
        Coroutines.Advance(deltaTime);
        Cooldowns.Advance(deltaTime);
        // Iterate a snapshot so a behaviour may register or unregister mid-frame.
        var snapshot = Behaviours.ToArray();
        foreach (GameBehaviour b in snapshot)
            b.DispatchUpdate(deltaTime);
        EventBus.Publish(new FrameUpdate(deltaTime));
    }

    // Tears the managed world down: destroys behaviours in reverse start order
    // and clears all subscriptions, leaving a clean slate for a reload.
    public static void Shutdown()
    {
        for (int i = Behaviours.Count - 1; i >= 0; i--)
            Behaviours[i].DispatchDestroy();
        Behaviours.Clear();
        Mods.Clear();
        FormScripts.Clear();
        FormData.Clear();
        Abilities.Clear();
        Effects.Clear();
        Scheduler.Clear();
        Coroutines.Clear();
        Cooldowns.Clear();
        FastTravel.Clear();
        PlayerControls.Clear();
        Zones.Clear();
        Time.Reset();
        EventBus.Clear();
        _booted = false;
    }
}
