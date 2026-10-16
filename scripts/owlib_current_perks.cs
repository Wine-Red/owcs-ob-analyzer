// Read the exact active perk references with the public OWLib/TankLib API.
// Built against the separately downloaded OWLib release; no game process access.
using System;
using System.Collections.Generic;
using System.IO;
using DataTool;
using DataTool.Helper;
using TankLib;
using TankLib.STU.Types;
using Newtonsoft.Json;

internal static class CurrentPerks {
    public static void Main(string[] args) {
        if (args.Length != 2) throw new ArgumentException("Expected client directory and output JSON");
        Program.Flags = new ToolFlags {
            OverwatchDirectory = args[0], Language = "zhCN", SpeechLanguage = "zhCN",
            RCN = true, Online = false, NoLanguageRegistry = true
        };
        Program.InitStorage(false);
        var output = new Dictionary<string, object>();
        foreach (var pair in Helpers.GetHeroes()) {
            if (!pair.Value.IsHero) continue;
            var hero = STUHelper.GetInstance<STUHero>(pair.Key);
            if (hero == null) throw new InvalidDataException("Missing hero " + pair.Key);
            var references = hero.m_B25192D9;
            if (hero.m_ECBDC792 != null) {
                var set = STUHelper.GetInstance<STU_B6011D9E>(hero.m_ECBDC792);
                if (set == null) throw new InvalidDataException("Missing current perk set " + pair.Key);
                references = set.m_D75CF0AF;
            }
            if (references == null) throw new InvalidDataException("Missing perks " + pair.Key);
            var perks = new List<object>();
            foreach (var reference in references) {
                var perk = STUHelper.GetInstance<STU_42B75C40>(reference);
                if (perk == null) throw new InvalidDataException("Missing active perk " + (teResourceGUID)reference);
                perks.Add(new {
                    PerkGUID = ((teResourceGUID)reference).ToString(),
                    LoadoutGUID = ((teResourceGUID)perk.m_loadout).ToString(),
                    Major = perk.m_D60C9EA2 != 0,
                    Level = perk.m_4DDE5023
                });
            }
            output.Add(pair.Key.ToString(), new {
                Name = pair.Value.Name,
                PerkSetGUID = hero.m_ECBDC792 == null ? null : ((teResourceGUID)hero.m_ECBDC792).ToString(),
                Perks = perks
            });
        }
        File.WriteAllText(args[1], JsonConvert.SerializeObject(output, Formatting.Indented));
        Console.WriteLine("Current perk lists written: " + output.Count);
    }
}
