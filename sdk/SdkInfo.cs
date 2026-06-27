using System.Reflection;

namespace Recreation;

// The SDK's public version, the single number a mod is built against. Sourced
// from the assembly version (set once in Directory.Build.props) so there is one
// knob, not a hand-edited constant that drifts. SemVer: additive bumps minor,
// breaking bumps major, and a mod built against X.Y runs on any engine shipping
// SDK X.>=Y. See README.md.
public static class SdkInfo
{
    public static string Version { get; } =
        typeof(SdkInfo).Assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? typeof(SdkInfo).Assembly.GetName().Version?.ToString()
        ?? "0.0.0";
}
