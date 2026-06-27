using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace Recreation.Modding;

// Per-mod settings persisted as JSON, so a mod is tunable without recompiling and
// remembers state across runs. A mod loads its config by name, reads typed values
// with fallbacks, and saves changes back. Files live under the config directory
// (RECREATION_MODS_DIR/config by default); a missing file yields an empty config
// that uses the fallbacks.
//
//   var cfg = ModConfig.Load("MyMod");
//   float rate = cfg.GetFloat("regenRate", 0.007f);
//   cfg.Set("seenIntro", true);
//   cfg.Save();
public sealed class ModConfig
{
    private readonly Dictionary<string, JsonElement> _values;
    private readonly string _path;

    private ModConfig(string path, Dictionary<string, JsonElement> values)
    {
        _path = path;
        _values = values;
    }

    // The directory config files live in. Defaults from RECREATION_MODS_DIR;
    // assignable by the host or tests.
    public static string ConfigDirectory { get; set; } = DefaultDirectory();

    // Loads the config for `name` (from `directory`, or ConfigDirectory). A
    // missing or unreadable file yields an empty config.
    public static ModConfig Load(string name, string? directory = null)
    {
        string dir = directory ?? ConfigDirectory;
        string path = Path.Combine(dir, name + ".json");
        var values = new Dictionary<string, JsonElement>(StringComparer.OrdinalIgnoreCase);
        try
        {
            if (File.Exists(path))
            {
                using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(path));
                if (doc.RootElement.ValueKind == JsonValueKind.Object)
                    foreach (JsonProperty p in doc.RootElement.EnumerateObject())
                        values[p.Name] = p.Value.Clone();
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[config] cannot read {path}: {ex.Message}");
        }
        return new ModConfig(path, values);
    }

    public float GetFloat(string key, float fallback) =>
        _values.TryGetValue(key, out JsonElement e) && e.ValueKind == JsonValueKind.Number
            ? e.GetSingle()
            : fallback;

    public int GetInt(string key, int fallback) =>
        _values.TryGetValue(key, out JsonElement e) && e.ValueKind == JsonValueKind.Number
            ? e.GetInt32()
            : fallback;

    public bool GetBool(string key, bool fallback) =>
        _values.TryGetValue(key, out JsonElement e) &&
        (e.ValueKind == JsonValueKind.True || e.ValueKind == JsonValueKind.False)
            ? e.GetBoolean()
            : fallback;

    public string GetString(string key, string fallback) =>
        _values.TryGetValue(key, out JsonElement e) && e.ValueKind == JsonValueKind.String
            ? e.GetString() ?? fallback
            : fallback;

    public void Set(string key, float value) => Store(key, value);
    public void Set(string key, int value) => Store(key, value);
    public void Set(string key, bool value) => Store(key, value);
    public void Set(string key, string value) => Store(key, value);

    public bool Has(string key) => _values.ContainsKey(key);

    // Writes the config back to disk, creating the directory if needed.
    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
            using var stream = File.Create(_path);
            using var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true });
            writer.WriteStartObject();
            foreach (KeyValuePair<string, JsonElement> kv in _values)
            {
                writer.WritePropertyName(kv.Key);
                kv.Value.WriteTo(writer);
            }
            writer.WriteEndObject();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[config] cannot write {_path}: {ex.Message}");
        }
    }

    private void Store<T>(string key, T value)
    {
        // Round-trip through JSON so the stored element matches what Load reads.
        _values[key] = JsonSerializer.SerializeToElement(value);
    }

    private static string DefaultDirectory()
    {
        string? mods = Environment.GetEnvironmentVariable("RECREATION_MODS_DIR");
        return string.IsNullOrEmpty(mods) ? "config" : Path.Combine(mods, "config");
    }
}
