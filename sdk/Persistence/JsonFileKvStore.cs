using System;
using System.Collections.Generic;
using System.IO;
using System.Text.Json;
using Recreation.Interop;

namespace Recreation.Net;

// A durable store backed by a single JSON file. Loaded once at construction, rewritten
// on every change. Each entry is a typed object ({"k":"int","v":42}) so a Value reloads
// as the kind it was stored as. Writes are atomic (temp file + rename); a missing, empty
// or corrupt file is tolerated by starting empty.
public sealed class JsonFileKvStore : IKvStore
{
    private readonly string _path;
    private readonly Dictionary<string, Value> _entries = new();

    public JsonFileKvStore(string path)
    {
        ArgumentNullException.ThrowIfNull(path);
        _path = path;
        Load();
    }

    public Value Get(string key) => _entries.TryGetValue(key, out Value v) ? v : Value.None;

    public void Set(string key, Value value)
    {
        if (value.IsNone) { Delete(key); return; }
        _entries[key] = value;
        Persist();
    }

    public bool Has(string key) => _entries.ContainsKey(key);

    public void Delete(string key)
    {
        if (_entries.Remove(key)) Persist();
    }

    public IReadOnlyCollection<string> Keys => _entries.Keys;

    public void Flush() => Persist();

    // Read the file into memory. Absence/emptiness/corruption is tolerated: log and
    // start empty rather than throw.
    private void Load()
    {
        if (!File.Exists(_path)) return;
        try
        {
            string json = File.ReadAllText(_path);
            if (string.IsNullOrWhiteSpace(json)) return;
            using var doc = JsonDocument.Parse(json);
            foreach (JsonProperty prop in doc.RootElement.EnumerateObject())
                _entries[prop.Name] = ReadValue(prop.Value);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"[persist] could not read '{_path}', starting empty: {ex.Message}");
            _entries.Clear();
        }
    }

    // Write the whole dictionary atomically: serialize to a temp file in the same
    // directory (so File.Move is a same-filesystem rename) and swap it over the target.
    private void Persist()
    {
        string? dir = Path.GetDirectoryName(_path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

        string tmp = _path + ".tmp";
        using (var stream = new FileStream(tmp, FileMode.Create, FileAccess.Write))
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            foreach (KeyValuePair<string, Value> kv in _entries)
            {
                writer.WritePropertyName(kv.Key);
                WriteValue(writer, kv.Value);
            }
            writer.WriteEndObject();
        }
        File.Move(tmp, _path, overwrite: true);
    }

    // Encode a Value as a typed {k,v} object. Object/Array handles are written as
    // strings because their 64-bit ids exceed JSON's safe-integer range.
    private static void WriteValue(Utf8JsonWriter writer, Value value)
    {
        writer.WriteStartObject();
        switch (value.Kind)
        {
            case ValueKind.Int:
                writer.WriteString("k", "int");
                writer.WriteNumber("v", value.AsInt());
                break;
            case ValueKind.Float:
                writer.WriteString("k", "float");
                writer.WriteNumber("v", value.AsFloat());
                break;
            case ValueKind.Bool:
                writer.WriteString("k", "bool");
                writer.WriteBoolean("v", value.AsBool());
                break;
            case ValueKind.String:
                writer.WriteString("k", "str");
                writer.WriteString("v", value.AsString());
                break;
            case ValueKind.Object:
                writer.WriteString("k", "obj");
                writer.WriteString("v", value.AsHandle().ToString());
                break;
            case ValueKind.Array:
                writer.WriteString("k", "arr");
                writer.WriteString("v", value.AsHandle().ToString());
                break;
            default:
                writer.WriteString("k", "none");
                break;
        }
        writer.WriteEndObject();
    }

    // Reconstruct a Value from a typed {k,v} object, defaulting to None for anything
    // malformed so one bad entry never aborts the whole load.
    private static Value ReadValue(JsonElement e)
    {
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty("k", out JsonElement kEl))
            return Value.None;
        string kind = kEl.GetString() ?? string.Empty;
        e.TryGetProperty("v", out JsonElement v);
        return kind switch
        {
            "int" => Value.Int(v.GetInt32()),
            "float" => Value.Float(v.GetSingle()),
            "bool" => Value.Bool(v.GetBoolean()),
            "str" => Value.String(v.GetString() ?? string.Empty),
            "obj" => Value.Object(ParseHandle(v)),
            "arr" => Value.Array(ParseHandle(v)),
            _ => Value.None,
        };
    }

    private static ulong ParseHandle(JsonElement v) =>
        v.ValueKind == JsonValueKind.String && ulong.TryParse(v.GetString(), out ulong h) ? h : 0;
}
