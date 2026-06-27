using System;
using System.Collections.Generic;
using System.Linq;
using Recreation.Modding;

namespace Recreation.Net;

// An immutable snapshot of a party. A party is identified by a small positive int
// (0 means "no party"). Built on demand from the replicated registry.
public readonly struct Party
{
    public int Id { get; }
    public uint Leader { get; }
    public IReadOnlyCollection<uint> Members { get; }

    internal Party(int id, uint leader, IReadOnlyCollection<uint> members)
    {
        Id = id;
        Leader = leader;
        Members = members;
    }

    public int Count => Members.Count;

    public bool Has(uint player) => Members.Contains(player);
}

// Raised on every machine when a party's membership or leader changes. The party
// may already be gone (disbanded) when this fires, so Party is nullable.
public readonly struct PartyChanged(int partyId) : IGameEvent
{
    public int PartyId { get; } = partyId;

    public Party? Party => Parties.ById(PartyId);
}

// Raised on the invitee's client when the server forwards a party invite. The UI
// reacts here (prompt to accept); accepting forwards Parties.RequestAccept.
public readonly struct PartyInviteReceived(int partyId, uint fromLeader) : IGameEvent
{
    public int PartyId { get; } = partyId;
    public uint FromLeader { get; } = fromLeader;
}
