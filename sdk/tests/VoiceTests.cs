using System;
using System.Collections.Generic;
using Recreation.Interop;
using Recreation.Modding;
using Recreation.Net;

namespace Recreation.Tests;

// Exercises the positional voice model and routing as a pure view over replicated player state: channels, mute, proximity falloff, and the mix push.
public static class VoiceTests
{
    private sealed class Recording : IRpcBackend
    {
        public void Emit(RpcTarget target, uint peer, string name, ReadOnlySpan<Value> args) { }
        public void Subscribe(string name) { }
    }

    // An inert engine backend that records Voice.SetGain pushes (peer -> gain).
    private sealed class GainRecorder : IEngineBackend
    {
        public readonly Dictionary<uint, float> Gains = new();

        public Value CallGlobal(string type, string function, ReadOnlySpan<Value> args)
        {
            if (type == "Voice" && function == "SetGain")
                Gains[(uint)args[0].AsInt()] = args[1].AsFloat();
            return Value.None;
        }

        public Value CallMethod(ulong self, string function, ReadOnlySpan<Value> args) => Value.None;
        public bool IsScriptLoaded(string type) => false;
        public bool LoadScript(string type) => false;
        public ulong CreateInstance(string type) => 0;
        public string TypeOf(ulong handle) => string.Empty;
        public Value GetProperty(ulong self, string name) => Value.None;
        public void SetProperty(ulong self, string name, Value value) { }
        public void Tick(float deltaTime) { }
    }

    public static void Run(Check check)
    {
        Native.Backend = new FakeBackend();   // keeps the Voice natives inert
        Rpc.Clear();
        Rpc.Bind(new Recording());
        Platform.Boot(NetRole.Server);        // binds StateBags + Players (local id 0)
        Voice.Bind(NetRole.Server);

        // Two remote players; the local listener is the server's own player (id 0).
        EventBus.Publish(new ClientJoined(1u));
        EventBus.Publish(new ClientJoined(2u));
        Player p1 = Players.Get(1u)!;
        Player p2 = Players.Get(2u)!;

        // --- Same radio channel + talking -> full volume both ways ---
        p1.State.Set("vchan", 5);
        p1.State.Set("vtalk", true);
        p2.State.Set("vchan", 5);
        p2.State.Set("vtalk", true);
        check.Equal("shared radio channel is full volume", 1f, Voice.GainBetween(p1, p2));
        check.Equal("radio is symmetric", 1f, Voice.GainBetween(p2, p1));
        check.Equal("ChannelOf reads a radio channel", VoiceMode.Radio, Voice.ChannelOf(p1).Mode);

        // --- A muted speaker is silent even on a shared channel ---
        Voice.SetMuted(2u, true);
        check.That("muted speaker reports muted", Voice.IsMuted(p2));
        check.Equal("muted speaker is silent", 0f, Voice.GainBetween(p1, p2));
        Voice.SetMuted(2u, false);
        check.Equal("unmuting restores the channel", 1f, Voice.GainBetween(p1, p2));

        // --- A silent (not transmitting) speaker is not heard ---
        p2.State.Set("vtalk", false);
        check.That("not-talking speaker is silent", !Voice.IsTalking(p2));
        check.Equal("silent speaker has zero gain", 0f, Voice.GainBetween(p1, p2));
        p2.State.Set("vtalk", true);

        // --- Different channels never hear each other ---
        p2.State.Set("vchan", 6);
        check.Equal("different channels are silent", 0f, Voice.GainBetween(p1, p2));

        // --- Proximity: linear falloff, monotonic with distance ---
        p1.State.Set("vchan", VoiceChannel.ProximityId);  // both on proximity
        p2.State.Set("vchan", VoiceChannel.ProximityId);
        p2.State.Set("vtalk", true);
        Voice.ProximityRange = 20f;
        // p1 (listener) sits at the origin; vary p2's distance along x.

        check.That("a close speaker is loud", GainAt(p1, p2, 2f) > 0.8f);
        check.Equal("a speaker past range is silent", 0f, GainAt(p1, p2, 25f));
        check.Equal("a speaker exactly at range is silent", 0f, GainAt(p1, p2, 20f));

        float[] dists = { 0f, 5f, 10f, 15f, 19f };
        float prev = float.MaxValue;
        bool monotonic = true;
        foreach (float d in dists)
        {
            float g = GainAt(p1, p2, d);
            if (g >= prev) monotonic = false;   // strictly decreasing as distance grows
            prev = g;
        }
        check.That("proximity gain falls off monotonically with distance", monotonic);

        // --- JoinChannel writes the local player's vchan; ChannelOf reads it back ---
        Voice.JoinChannel(3);
        check.Equal("JoinChannel writes the local channel", 3, Voice.ChannelOf(Players.Local).Id);
        check.Equal("the joined channel is a radio channel", VoiceMode.Radio,
                    Voice.ChannelOf(Players.Local).Mode);
        Voice.JoinChannel(VoiceChannel.ProximityId);
        check.That("JoinChannel(0) returns to proximity", Voice.ChannelOf(Players.Local).IsProximity);

        // --- Server SetMuted sets vmute; IsMuted reads it ---
        Voice.SetMuted(1u, true);
        check.That("server mute sets the flag", Voice.IsMuted(p1));
        Voice.SetMuted(1u, false);
        check.That("server unmute clears the flag", !Voice.IsMuted(p1));

        // --- UpdateMix pushes a per-peer gain for every other player ---
        // Local listener on proximity at the origin; p1 close and talking, p2 far.
        Players.Local.State.Set("vchan", VoiceChannel.ProximityId);
        p1.State.Set("vchan", VoiceChannel.ProximityId);
        p1.State.Set("vtalk", true);
        p1.State.Set("px", 3f); p1.State.Set("py", 0f); p1.State.Set("pz", 0f);
        p2.State.Set("vchan", VoiceChannel.ProximityId);
        p2.State.Set("vtalk", true);
        p2.State.Set("px", 100f); p2.State.Set("py", 0f); p2.State.Set("pz", 0f);

        var recorder = new GainRecorder();
        Native.Backend = recorder;
        Voice.UpdateMix();
        Native.Backend = new FakeBackend();

        check.That("UpdateMix pushes a gain for the close player", recorder.Gains.ContainsKey(1u));
        check.That("UpdateMix pushes a gain for the far player", recorder.Gains.ContainsKey(2u));
        check.That("pushed gain matches the model for the close player",
                   MathF.Abs(recorder.Gains[1u] - Voice.GainBetween(Players.Local, p1)) < 1e-4f);
        check.That("the close player is audible", recorder.Gains[1u] > 0.5f);
        check.Equal("the far player is silent", 0f, recorder.Gains[2u]);

        Voice.Reset();
        Recreation.Net.Platform.Reset();
        Rpc.Clear();
        EventBus.Clear();
        Native.Backend = null;
    }

    // Places `speaker` at distance `d` from `listener` (at the origin) and returns
    // the gain the listener hears them at.
    private static float GainAt(Player listener, Player speaker, float d)
    {
        listener.State.Set("px", 0f); listener.State.Set("py", 0f); listener.State.Set("pz", 0f);
        speaker.State.Set("px", d); speaker.State.Set("py", 0f); speaker.State.Set("pz", 0f);
        return Voice.GainBetween(listener, speaker);
    }
}
