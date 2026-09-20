namespace StreamedDepLib;

// The payload a dependent assembly (StreamedUsesDep) reaches across the
// assembly boundary, proving streamed scripts can depend on each other.
public static class DepValue
{
    public static string Value() => "streamed-dep-value";
}
