using System;
using System.Collections.Generic;
using Recreation;
using Recreation.Interop;
using Recreation.Modding;

namespace Recreation.Net;

// Positional voice chat: who hears whom and how loud. This layer owns the model
// and routing (channels, mutes, push-to-talk, per-listener gain); the engine owns
// capture, mixing and transport, reached through the "Voice" native.
//
// Per-player voice state lives in each player's replicated state bag:
//   vchan  (int)  the channel the player transmits on (0 = proximity, default)
//   vmute  (bool) server/admin mute: nobody hears this player
//   vtalk  (bool) the player is currently transmitting (push-to-talk)
// Positions come from the px/py/pz bag keys a gameplay mod (or the engine) writes.
public static class Voice
{
    // Player state-bag keys, kept short since every change replicates.
    private const string ChannelKey = "vchan";
    private const string MutedKey = "vmute";
    private const string TalkingKey = "vtalk";
    private const string PosX = "px", PosY = "py", PosZ = "pz";

    // The native surface the engine mixer reads.
    private const string NativeType = "Voice";
    private const string SetGainFn = "SetGain";
    private const string SetChannelFn = "SetChannel";
    private const string SetPushToTalkFn = "SetPushToTalk";

    // Client -> server: request an admin mute (a client cannot write another player's bag).
    private const string MuteRpc = "voice:mute";

    // How often the per-frame mix is recomputed; gain only needs to track movement.
    private const float UpdateInterval = 0.1f;

    private const float DefaultProximityRange = 20f;

    private static readonly List<IDisposable> Subscriptions = new();
    private static NetRole _role = NetRole.Standalone;
    private static float _sinceUpdate;

    // The distance, in world units (meters), at which a proximity speaker fades to silence.
    public static float ProximityRange { get; set; } = DefaultProximityRange;

    // Wire voice for a role. Idempotent (resets first).
    public static void Bind(NetRole role)
    {
        Reset();
        _role = role;
        // Recompute the local listener's mix as players move (throttled).
        Subscriptions.Add(EventBus.Subscribe<FrameUpdate>(OnFrame));
        // The host honours client mute requests; clients only ever send them.
        if (role == NetRole.Server)
            Rpc.On(MuteRpc, OnMuteRequest);
    }

    // Drop subscriptions and return to the neutral baseline. Called on teardown.
    public static void Reset()
    {
        foreach (IDisposable s in Subscriptions) s.Dispose();
        Subscriptions.Clear();
        _role = NetRole.Standalone;
        _sinceUpdate = 0f;
        ProximityRange = DefaultProximityRange;
    }

    // --- Local controls (this machine's own player) ---

    // Switch the local player to a channel: writes vchan and tells the engine.
    public static void JoinChannel(int id)
    {
        Players.Local.State.Set(ChannelKey, id);
        Native.CallGlobal(NativeType, SetChannelFn, new[] { Value.Int(id) });
    }

    public static void JoinChannel(VoiceChannel channel) => JoinChannel(channel.Id);

    // Push-to-talk: writes the vtalk flag and gates the engine's local capture.
    public static void SetTalking(bool on)
    {
        Players.Local.State.Set(TalkingKey, on);
        Native.CallGlobal(NativeType, SetPushToTalkFn, new[] { Value.Bool(on) });
    }

    // --- Server authority ---

    // Admin-mute a player. On the host it applies directly; on a client it is a
    // request the host validates.
    public static void SetMuted(uint peer, bool muted)
    {
        if (_role == NetRole.Client)
        {
            Rpc.Emit(MuteRpc, Value.Int((int)peer), Value.Bool(muted));
            return;
        }
        StateBags.Player(peer).Set(MutedKey, muted);
    }

    // Host: a client asked to mute someone. Accept it only for a connected peer.
    private static void OnMuteRequest(RpcEvent e)
    {
        if (e.Args.Length < 2) return;
        uint peer = (uint)e.Args[0].AsInt();
        if (Players.IsConnected(peer))
            StateBags.Player(peer).Set(MutedKey, e.Args[1].AsBool());
    }

    // --- Queries ---

    public static bool IsMuted(Player player) => player.State.Get(MutedKey).AsBool();
    public static bool IsTalking(Player player) => player.State.Get(TalkingKey).AsBool();

    public static VoiceChannel ChannelOf(Player player) =>
        new(player.State.Get(ChannelKey).AsInt());

    // --- Routing model ---

    // The gain (0..1) at which `listener` hears `speaker` right now.
    //
    //   * 0 if the speaker is muted, not transmitting, or is the listener itself.
    //   * 0 if the two are on different channels.
    //   * 1 on a shared radio channel (non-positional, full volume).
    //   * On the shared proximity channel, a linear distance falloff:
    //         gain = 1 - distance / ProximityRange, clamped to [0, 1]
    public static float GainBetween(Player listener, Player speaker)
    {
        if (speaker.Id == listener.Id) return 0f;
        if (IsMuted(speaker) || !IsTalking(speaker)) return 0f;

        int speakerChan = ChannelOf(speaker).Id;
        if (speakerChan != ChannelOf(listener).Id) return 0f;    // different channels
        if (speakerChan != VoiceChannel.ProximityId) return 1f;  // shared radio channel

        float distance = (PositionOf(listener) - PositionOf(speaker)).Length;
        return Mathf.Clamp01(1f - distance / ProximityRange);
    }

    // Recompute the local listener's mix and push per-peer gain to the engine mixer.
    // Public so tests (or a mod) can force an immediate update.
    public static void UpdateMix()
    {
        Player listener = Players.Local;
        foreach (Player speaker in Players.All)
        {
            if (speaker.Id == listener.Id) continue;
            float gain = GainBetween(listener, speaker);
            Native.CallGlobal(NativeType, SetGainFn,
                new[] { Value.Int((int)speaker.Id), Value.Float(gain) });
        }
    }

    private static void OnFrame(FrameUpdate frame)
    {
        _sinceUpdate += frame.DeltaTime;
        if (_sinceUpdate < UpdateInterval) return;
        _sinceUpdate = 0f;
        UpdateMix();
    }

    // The player's world position from its px/py/pz bag keys (0 when unset).
    private static Vector3 PositionOf(Player player)
    {
        StateBag s = player.State;
        return new Vector3(s.Get(PosX).AsFloat(), s.Get(PosY).AsFloat(), s.Get(PosZ).AsFloat());
    }
}
