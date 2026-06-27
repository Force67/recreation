namespace Recreation.Net;

// A line of work a player can hold: a name and the base pay one shift earns.
public sealed class Job
{
    public string Name { get; }

    // Cash paid per Jobs.Pay call; non-positive pay (an "unemployed" placeholder) yields nothing.
    public int Pay { get; }

    public Job(string name, int pay)
    {
        Name = name;
        Pay = pay;
    }
}
