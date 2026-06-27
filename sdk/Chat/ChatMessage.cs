namespace Recreation.Net;

// Where a chat line is addressed. Global reaches everyone; Team and Proximity are
// narrowed by the recipient filter (a mod decides who shares a team or is in
// earshot); System is engine/mod chatter with no human sender.
public enum ChatChannel
{
    Global,
    Team,
    Proximity,
    System,
}

// One delivered chat line, immutable once the server stamps it. Carries the sender's
// name (not just its id) so a client renders it without a roster lookup, and stays
// correct after that player has left.
public readonly struct ChatMessage
{
    public uint Sender { get; }
    public string SenderName { get; }
    public ChatChannel Channel { get; }
    public string Text { get; }

    // Packed rgba8; 0 lets the HUD pick its default colour for the channel.
    public uint Color { get; }

    public ChatMessage(uint sender, string senderName, ChatChannel channel, string text, uint color = 0)
    {
        Sender = sender;
        SenderName = senderName;
        Channel = channel;
        Text = text;
        Color = color;
    }
}
