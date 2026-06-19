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

// Rutas ajustadas por Onyx 2026-06-17: apuntan al repo en E:, no a la ruta de Steam original.
const string SrcPath = @"E:\Aplicaciones\Kenshi-Online\kenshi-online.mod";
const string OutPath = @"E:\Aplicaciones\Kenshi-Online\kenshi-online-16.mod";
const int TotalPlayers = 16;

if (args.Length > 0 && args[0] == "dump")
{
    var a = typeof(ModFile).Assembly;
    foreach (var tn in new[] { "OpenConstructionSet.Data.Item", "OpenConstructionSet.Data.ReferenceCategory", "OpenConstructionSet.Data.Reference" })
    {
        var t = a.GetType(tn)!;
        Console.WriteLine($"\n=== {t.FullName} ===");
        foreach (var p in t.GetProperties()) Console.WriteLine($"  prop {p.PropertyType.Name} {p.Name} canWrite={p.CanWrite}");
    }
    return;
}

// Modo "explore" (Onyx): vuelca game starts y squads con su cableado de referencias,
// para diagnosticar el bug del inicio "Multiplayer" (squad que apunta a un animal/Bonedog).
if (args.Length > 0 && args[0] == "explore")
{
    // Acepta ruta opcional: "explore <ruta.mod>" (default = SrcPath). Para verificar el candidato.
    var srcE = new ModFile(args.Length > 1 ? args[1] : SrcPath);
    var dataE = await srcE.ReadDataAsync();
    var byId = dataE.Items.ToDictionary(i => i.StringId, i => i, StringComparer.OrdinalIgnoreCase);
    string NameOf(string sid) => byId.TryGetValue(sid, out var it) ? $"\"{it.Name}\" [{it.Type}]" : "(externo/?)";
    foreach (var it in dataE.Items
        .Where(i => i.Type == ItemType.NewGameStartoff || i.Type == ItemType.SquadTemplate || i.Type == ItemType.Faction || i.Type == ItemType.Character)
        .OrderBy(i => i.Type.ToString()).ThenBy(i => i.Name))
    {
        Console.WriteLine($"\n[{it.Type}] \"{it.Name}\"  ({it.StringId})");
        // Para facciones y personajes, volcamos tambien las propiedades (Values): ahi estan
        // flags como "is player faction" y la raza/facccion por defecto.
        if (it.Type == ItemType.Faction || it.Type == ItemType.Character)
            foreach (var kv in it.Values)
                Console.WriteLine($"    val {kv.Key} = {kv.Value}");
        foreach (var cat in it.ReferenceCategories)
            foreach (var r in cat.References)
                Console.WriteLine($"    {cat.Name} -> {NameOf(r.TargetId)}  ({r.TargetId})  [v0={r.Value0} v1={r.Value1} v2={r.Value2}]");
    }
    return;
}

// Modo "basefac" (Onyx): carga gamedata.base del juego y vuelca la faccion del jugador vanilla
// ("Nameless", 204-gamedata.base) con sus Values y relaciones, para saber que copiar a Player 1/2.
if (args.Length > 0 && args[0] == "basefac")
{
    var basePath = @"E:\SteamLibrary\steamapps\common\Kenshi\data\gamedata.base";
    var baseData = await new ModFile(basePath).ReadDataAsync();
    Console.WriteLine($"gamedata.base: {baseData.Items.Count} items");
    var nameless = baseData.Items.FirstOrDefault(i => i.Type == ItemType.Faction
        && (i.StringId == "204-gamedata.base" || i.Name == "Nameless"));
    if (nameless is null) { Console.WriteLine("Faccion Nameless no encontrada"); return; }
    // Dict tolerante a StringIds duplicados en gamedata.base (los hay, p.ej. 3012-walls.mod).
    var byIdB = new Dictionary<string, Item>(StringComparer.OrdinalIgnoreCase);
    foreach (var it in baseData.Items) byIdB[it.StringId] = it;
    string NameOfB(string sid) => byIdB.TryGetValue(sid, out var it) ? $"\"{it.Name}\" [{it.Type}]" : "(externo/?)";
    Console.WriteLine($"\n[Faction] \"{nameless.Name}\" ({nameless.StringId})");
    foreach (var kv in nameless.Values) Console.WriteLine($"    val {kv.Key} = {kv.Value}");
    foreach (var cat in nameless.ReferenceCategories)
        foreach (var r in cat.References)
            Console.WriteLine($"    {cat.Name} -> {NameOfB(r.TargetId)}  ({r.TargetId})  [v0={r.Value0} v1={r.Value1} v2={r.Value2}]");
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

// ── Clonado de FACCIONES de jugador (Onyx 2026-06-18) ──
// Objetivo: tener hasta 6 facciones de jugador (Player 1..6) en vez de 2, para habilitar
// en el futuro modos "teams"/"per-player" en el servidor. HOY el cliente (Core,
// shared_save_sync.cpp) solo reconoce "10-" y "12-", así que estas facciones extra son
// DATOS PREPARADOS: el server las usará cuando Core amplíe su mapeo. No rompen el co-op
// actual (factionMode="single" sigue usando solo la facción del Player 1).
//
// Cada facción nueva se clona de "Player 1" (que ya tiene el fundamental type correcto tras
// el fix de abajo) para heredar su cableado. Se generan los StringIds y se emite el
// manifiesto faction-slots.json para que el server lea los slots sin hardcodear.
const int TotalFactions = 6; // Player 1..6 como facciones de jugador
var factionSlots = new List<string>();
{
    // Facciones de jugador existentes en el mod fuente (Player 1, Player 2).
    var playerFactions = data.Items
        .Where(i => i.Type == ItemType.Faction && i.Name.StartsWith("Player "))
        .OrderBy(i => {
            var parts = i.Name.Split(' ');
            return parts.Length > 1 && int.TryParse(parts[1], out var n) ? n : 999;
        })
        .ToList();

    var faction1 = playerFactions.FirstOrDefault(i => i.Name == "Player 1");
    if (faction1 is null)
    {
        Console.WriteLine("  [WARN] Faccion 'Player 1' no encontrada — no se clonan facciones extra ni se genera manifiesto");
    }
    else
    {
        // Registrar las facciones ya presentes en el manifiesto (StringId = "{id}-kenshi-online.mod").
        foreach (var pf in playerFactions)
            factionSlots.Add(pf.StringId);

        int facCreated = 0;
        for (int n = playerFactions.Count + 1; n <= TotalFactions; n++)
        {
            string fName = $"Player {n}";
            if (data.Items.Any(i => i.Type == ItemType.Faction && i.Name == fName))
                continue; // Ya existe

            int fid = nextId++;
            string fStringId = $"{fid}-kenshi-online.mod";

            // Deep-copy de Player 1 para no aliasear referencias entre facciones.
            var fValues = new Dictionary<string, object>(faction1.Values);
            var fRefs = faction1.ReferenceCategories.Select(c => new ReferenceCategory(c)).ToList();
            var fInstances = faction1.Instances.Select(ins => new Instance(ins)).ToList();

            var facClone = new Item(ItemType.Faction, fid, fName, fStringId, faction1.SaveData, fValues, fRefs, fInstances);
            data.Items.Add(facClone);
            factionSlots.Add(fStringId);
            facCreated++;
            Console.WriteLine($"  created FACTION {fName} id={fid} stringId={fStringId} (clon de Player 1)");
        }
        data.LastId = Math.Max(data.LastId, nextId - 1);
        Console.WriteLine($"  Facciones de jugador totales: {factionSlots.Count} (creadas {facCreated})");
    }
}

// ── FIX del bug del perro (Onyx 2026-06-17) ──
// El game start "Multiplayer" tenia su 'squad' apuntando a "startoff- Wanderer dead" (22-),
// un escuadron SIN lider que solo contiene un animal ("Bonedog dead", 24-) -> el creador de
// personaje solo ofrecia el perro. Lo re-cableamos a "Player 1 squad" (11-), que SI tiene un
// leader Character jugable -> el jugador crea un personaje normal en vez del perro.
{
    var mpStart = data.Items.FirstOrDefault(i => i.Type == ItemType.NewGameStartoff && i.Name == "Multiplayer");
    if (mpStart is null)
    {
        Console.WriteLine("  [WARN] game start 'Multiplayer' no encontrado — no se aplica el fix del perro");
    }
    else
    {
        int fixedRefs = 0;
        foreach (var cat in mpStart.ReferenceCategories.Where(c => c.Name == "squad"))
            foreach (var r in cat.References)
                if (r.TargetId == "22-kenshi-online.mod") { r.TargetId = "11-kenshi-online.mod"; fixedRefs++; }
        Console.WriteLine(fixedRefs > 0
            ? $"  FIX perro: game start Multiplayer 'squad' 22- (Bonedog) -> 11- (Player 1 squad). Refs cambiadas: {fixedRefs}"
            : "  [WARN] no se encontro la referencia squad 22- en el game start Multiplayer (ya arreglado?)");
    }
}

// ── FIX de facciones v2 (Onyx 2026-06-17) ──
// HISTORIAL: el tipo original era 9 (OT_ADVENTURER) -> enemigos huyen. Un primer fix lo puso a 0
// (OT_NONE) por un diagnostico ERRONEO (se creyo que Nameless usaba 0). NO basto: 0 deja a la
// faccion del jugador sin clase de comportamiento valida y los enemigos siguen huyendo.
// VERIFICADO EMPIRICAMENTE con el modo 'basefac': la faccion del jugador vanilla "Nameless"
// (204-gamedata.base) usa "fundamental type" = 4 (OT_CIVILIAN). Lo correcto es CIVILIAN, no NONE.
// El motor mete al host en la faccion "Player 1" (parche del string .rdata en lobby_manager.cpp),
// asi que estos datos rigen el aggro/combate del jugador. (RE: game-reverse-engineer, enum
// CharacterTypeEnum en KenshiLib Enums.h:254-267, fundamentalNPCType en Faction.h:64 offset +0x34.)
{
    int facFixed = 0;
    // Aplica a TODAS las facciones de jugador (Player 1..N), incluidas las recién clonadas.
    foreach (var fac in data.Items.Where(i => i.Type == ItemType.Faction
                                              && i.Name.StartsWith("Player ")))
    {
        if (fac.Values.ContainsKey("fundamental type"))
        {
            var cur = fac.Values["fundamental type"];
            fac.Values["fundamental type"] = Convert.ChangeType(4, cur.GetType()); // 4 = OT_CIVILIAN (como Nameless)
            facFixed++;
            Console.WriteLine($"  FIX faccion: {fac.Name} 'fundamental type' {cur} -> 4 (OT_CIVILIAN, igual que Nameless)");
        }
    }
    if (facFixed == 0) Console.WriteLine("  [WARN] no se encontraron facciones Player 1/2 para corregir el tipo");
}

// ── Manifiesto de slots de facción (Onyx 2026-06-18) ──
// Lista ORDENADA de los StringIds de facción de jugador. El servidor puede leerlo para
// asignar slots sin hardcodear el array. Se emite junto al mod, en el mismo directorio.
{
    string manifestPath = Path.Combine(Path.GetDirectoryName(OutPath)!, "faction-slots.json");
    // JSON simple a mano (sin dependencias extra): {"factionSlots":["10-...","12-...",...]}
    var quoted = factionSlots.Select(s => "\"" + s.Replace("\"", "\\\"") + "\"");
    string jsonManifest = "{\n  \"factionSlots\": [\n    " +
                          string.Join(",\n    ", quoted) +
                          "\n  ]\n}\n";
    File.WriteAllText(manifestPath, jsonManifest);
    Console.WriteLine($"Manifiesto de facciones escrito: {manifestPath} ({factionSlots.Count} slots)");
}

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
