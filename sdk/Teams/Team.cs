namespace Recreation.Net;

// An immutable team definition owned by the host; membership is not stored here
// but in each player's state bag (see Teams). Ids are 1-based; 0 is "no team".
public sealed class Team
{
    public int Id { get; }
    public string Name { get; }

    // Packed rgba8 (0xRRGGBBAA), the team's colour for blips, scoreboards and chat tags.
    public uint Color { get; }

    internal Team(int id, string name, uint color)
    {
        Id = id;
        Name = name;
        Color = color;
    }

    public override string ToString() => $"{Name}#{Id}";
}
