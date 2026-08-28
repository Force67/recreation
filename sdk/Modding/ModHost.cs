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
    // The engine's role, set from the handshake before Boot. Standalone (run
    // everything) until told otherwise, so single-player and tests are unchanged.
    private static int _hostRealm = ModDiscovery.HostStandalone;
    // The optional game mode the launcher armed, and the set of mods that are
    // selectable modes rather than base rulesets or ordinary mods. Both come from
    // the handshake before Boot; an empty set filters nothing, so a launch with no
    // launcher (and every test) loads mods exactly as it did before.
    private static string? _gameMode;
    private static readonly HashSet<string> SelectableModes = new(StringComparer.Ordinal);

    public static IReadOnlyList<GameBehaviour> ActiveBehaviours => Behaviours;
    public static bool Booted => _booted;

    // Sets which role this process runs as (server, client or standalone), so Boot
    // starts only the mods that role admits. Call before Boot.
    public static void SetHostRealm(int hostRealm) => _hostRealm = hostRealm;

    // Names the armed game mode (null or empty for none) and every mod name that
    // is a selectable mode. A mod in `selectable` loads only when it is the armed
    // one; an armed mode adds to the domain's base ruleset, it does not replace
    // it. Call before Boot.
    public static void SetGameModes(string? armed, IEnumerable<string> selectable)
    {
        _gameMode = string.IsNullOrEmpty(armed) ? null : armed;
        SelectableModes.Clear();
        foreach (string id in selectable)
            if (!string.IsNullOrEmpty(id)) SelectableModes.Add(id);
    }

    // Discovers and loads every mod in the currently loaded assemblies, then
    // starts the auto-start behaviours. Idempotent.
    public static void Boot()
    {
        if (_booted) return;
        _booted = true;
        Console.WriteLine("[managed] mod host booting");
        if (SelectableModes.Count > 0)
            Console.WriteLine(_gameMode != null
                ? $"[mods] game mode {_gameMode} armed, {SelectableModes.Count} selectable"
                : $"[mods] no game mode armed, {SelectableModes.Count} selectable");
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
        foreach (Type modType in ModDiscovery.FindMods(list, _hostRealm))
        {
            var meta = modType.GetCustomAttribute<ModAttribute>();
            // Decided before instantiation so a mode the launcher did not arm
            // never reaches OnLoad.
            if (meta != null && SelectableModes.Contains(meta.Name) && meta.Name != _gameMode)
            {
                Console.WriteLine($"[mods] game mode {meta.Name} not armed, skipping");
                continue;
            }
            if (Activator.CreateInstance(modType) is not IMod mod) continue;
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

        foreach (Type behaviourType in ModDiscovery.FindAutoStartBehaviours(list, _hostRealm))
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
        Rpc.Clear();
        Recreation.Net.Platform.Reset();
        Time.Reset();
        EventBus.Clear();
        _booted = false;
    }
}
