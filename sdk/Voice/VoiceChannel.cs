using System;

namespace Recreation.Net;

// How a voice channel carries audio. Proximity (channel 0) is positional
// (attenuated by distance); Radio is non-positional (every member hears every
// other at full volume).
public enum VoiceMode
{
    Proximity,
    Radio,
}

// Identifies a voice channel by id. Channel 0 is the reserved proximity channel;
// every other id is a radio/party channel.
public readonly struct VoiceChannel : IEquatable<VoiceChannel>
{
    // The reserved positional channel every player starts on.
    public const int ProximityId = 0;

    public static readonly VoiceChannel Proximity = new(ProximityId);

    public int Id { get; }

    public VoiceChannel(int id) => Id = id;

    // A radio/party channel; a zero id collapses to the proximity channel.
    public static VoiceChannel Radio(int id) => new(id);

    public VoiceMode Mode => Id == ProximityId ? VoiceMode.Proximity : VoiceMode.Radio;
    public bool IsProximity => Id == ProximityId;

    public bool Equals(VoiceChannel other) => Id == other.Id;
    public override bool Equals(object? obj) => obj is VoiceChannel c && Equals(c);
    public override int GetHashCode() => Id;
    public override string ToString() => IsProximity ? "Proximity" : $"Radio({Id})";
}
