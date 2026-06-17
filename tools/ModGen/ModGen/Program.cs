using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;
using OpenConstructionSet.Data;
using OpenConstructionSet.Mods;

// ── Kenshi-Online mod generator ──
// Produces a SELF-CONTAINED kenshi-online-16.mod by faithfully reading the proven
// kenshi-online.mod (read-modify-write via ModFile, so ALL original records — the
// Multiplayer game-start, factions, squads, Player 1/2 — are preserved) and cloning
// the game-validated "Player 1" CHARACTER record into Player 3..16. Cloning a record
// the game already loads guarantees every field/reference (clothing, weapons, etc.)
// is valid. Writes a CANDIDATE file — never overwrites the live mod.

const string SrcPath = @"C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiMP\kenshi-online.mod";
const string OutPath = @"C:\Program Files (x86)\Steam\steamapps\common\Kenshi\KenshiMP\kenshi-online-16.mod";
const int TotalPlayers = 16;

if (args.Length > 0 && args[0] == "dump")
{
    var a = typeof(ModFile).Assembly;
    foreach (var tn in new[] { "OpenConstructionSet.Data.Item", "OpenConstructionSet.Data.ReferenceCategory" })
    {
        var t = a.GetType(tn)!;
        Console.WriteLine($"\n=== {t.FullName} ===");
        foreach (var p in t.GetProperties()) Console.WriteLine($"  prop {p.PropertyType.Name} {p.Name} canWrite={p.CanWrite}");
    }
    return;
}

var src = new ModFile(SrcPath);
var data = await src.ReadDataAsync();
Console.WriteLine($"Read {data.Items.Count} items from kenshi-online.mod (lastId={data.LastId}, type={data.Type})");
Console.WriteLine("  types: " + string.Join(", ", data.Items.GroupBy(i => i.Type).OrderByDescending(g => g.Count()).Select(g => $"{g.Key}={g.Count()}")));

var player1 = data.Items.FirstOrDefault(i => i.Type == ItemType.Character && i.Name == "Player 1");
if (player1 is null)
{
    Console.WriteLine("ERROR: 'Player 1' character not found — cannot clone.");
    return;
}
Console.WriteLine($"Source 'Player 1': id={player1.Id} stringId={player1.StringId} " +
                  $"fields={player1.Values.Count} refCats=[{string.Join(",", player1.ReferenceCategories.Select(c => c.Name))}] " +
                  $"instances={player1.Instances.Count}");

int nextId = Math.Max(data.LastId, data.Items.Max(i => i.Id)) + 1;
int created = 0;
for (int n = 1; n <= TotalPlayers; n++)
{
    string name = $"Player {n}";
    if (data.Items.Any(i => i.Type == ItemType.Character && i.Name == name))
    {
        Console.WriteLine($"  keep existing {name}");
        continue;
    }

    int id = nextId++;
    string stringId = $"{id}-kenshi-online.mod";

    // Deep-copy Player 1's fields/references so clones never alias each other.
    var values = new Dictionary<string, object>(player1.Values);
    var refCats = player1.ReferenceCategories.Select(c => new ReferenceCategory(c)).ToList();
    var instances = player1.Instances.Select(ins => new Instance(ins)).ToList();

    var clone = new Item(ItemType.Character, id, name, stringId, player1.SaveData, values, refCats, instances);
    data.Items.Add(clone);
    created++;
    Console.WriteLine($"  created {name} id={id} stringId={stringId}");
}

data.LastId = Math.Max(data.LastId, nextId - 1);
Console.WriteLine($"Created {created} new Player records. Total items now {data.Items.Count}. Writing self-contained mod:\n  {OutPath}");

var dst = new ModFile(OutPath);
await dst.WriteDataAsync(data);

// ── Verify by faithful re-read ──
var verify = await new ModFile(OutPath).ReadDataAsync();
var players = verify.Items.Where(i => i.Type == ItemType.Character && i.Name.StartsWith("Player "))
                          .OrderBy(i => int.Parse(i.Name.Split(' ')[1])).ToList();
Console.WriteLine($"\nVERIFY: candidate has {verify.Items.Count} items, {players.Count} Player characters:");
foreach (var p in players)
    Console.WriteLine($"  {p.Name}: id={p.Id} refCats=[{string.Join(",", p.ReferenceCategories.Select(c => c.Name))}]");
var nonPlayer = verify.Items.Where(i => !(i.Type == ItemType.Character && i.Name.StartsWith("Player "))).GroupBy(i => i.Type);
Console.WriteLine("  preserved non-player records: " + string.Join(", ", nonPlayer.Select(g => $"{g.Key}={g.Count()}")));
Console.WriteLine(players.Count == TotalPlayers ? "\nOK: self-contained mod with all 16 players + original content." : "\nWARNING: player count mismatch!");
