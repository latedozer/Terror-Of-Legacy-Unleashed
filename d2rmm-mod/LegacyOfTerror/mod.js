// Legacy of Terror (offline): the data half of Blizzard's Diablo 30th anniversary event.
// The Legacy of Terror plugin (d2rl-legacy-of-terror.dll) does the rest.

// ---------------------------------------------------------------------------
// Terror Zones everywhere.
// The game reads its terror zone schedule from desecratedzones.json (Blizzard's own notes on
// the file say several configs with start and end times can be used "to simulate events").
// Each config's scheduled zones are merged into one zone holding every level, so the
// scheduler has a single zone to pick and every level is terrorized at once. Each level keeps
// its own zone_data_id, so zone completion (Heralds) still counts per zone as before.
// Unlocking stays the game's own: a difficulty's Terrorized box appears once Baal is dead
// there, and unticking it in the game creation (gear) menu gives a game without terror zones.
if (config.allZones) {
  const tzFile = 'hd\\global\\excel\\desecratedzones.json';
  const tz = D2RMM.readJson(tzFile);
  (tz.desecrated_zones || []).forEach((cfg) => {
    const seen = new Set();
    const levels = [];
    (cfg.zones || []).forEach((zone) => {
      (zone.levels || []).forEach((level) => {
        if (seen.has(level.level_id)) return;
        seen.add(level.level_id);
        const copy = Object.assign({}, level);
        // a level falls back to its zone's overrides; keep them on the level itself
        if (zone.difficulties && !copy.difficulties) copy.difficulties = zone.difficulties;
        levels.push(copy);
      });
    });
    if (!levels.length) return;
    cfg.zones = [
      {
        type: 'DesecratedZone',
        name: (cfg.name || 'desecratedzones') + '_zones_legacy_of_terror',
        id: 'LegacyOfTerror-AllZones',
        levels,
      },
    ];
    // one long window instead of a new pick every 30 minutes (one day; the game then picks
    // the same single zone again)
    cfg.zone_duration_minutes = 1440;
    cfg.break_duration_minutes = 0;
  });
  D2RMM.writeJson(tzFile, tz);
}

// ---------------------------------------------------------------------------
// Hell Bovine Minion pet type: a copy of the Necromancer's "revive" row for their own count, but
// with no portrait and no warp: like the Assassin's traps and the Sorceress' hydras (rows with
// no warp, partysend, drawhp, icontype or icon) they stay where they rose and out of the party
// frame.
const petFile = 'global\\excel\\pettype.txt';
const pets = D2RMM.readTsv(petFile);
const petName = 'lot_bovine';
if (config.bovines && !pets.rows.some((r) => r['pet type'] === petName)) {
  const revive = pets.rows.find((r) => r['pet type'] === 'revive');
  if (revive) {
    const row = Object.assign({}, revive);
    row['pet type'] = petName;
    row.name = 'LoTBovineMinion';
    ['warp', 'range', 'partysend', 'drawhp', 'icontype', 'baseicon'].forEach((k) => {
      if (k in row) row[k] = '';
    });
    Object.keys(row).forEach((k) => {
      if (/^(mclass|micon|overridename)\d+$/.test(k)) row[k] = '';
    });
    pets.rows.push(row);
    D2RMM.writeTsv(petFile, pets);

    const uiFile = 'local\\lng\\strings\\ui.json';
    const ui = D2RMM.readJson(uiFile);
    if (!ui.some((s) => s.Key === 'LoTBovineMinion')) {
      const id = ui.reduce((m, s) => Math.max(m, Number(s.id) || 0), 0) + 1;
      const text = 'Hell Bovine Minion';
      const entry = { id, Key: 'LoTBovineMinion' };
      Object.keys(ui[0] || {}).forEach((k) => {
        if (k !== 'id' && k !== 'Key') entry[k] = text;
      });
      ui.push(entry);
      D2RMM.writeJson(uiFile, ui);
    }
  }
}

// ---------------------------------------------------------------------------
// Hell Bovine Minion monster: the Hell Bovine as an ally, the way the game makes its own.
// The Hell Bovine's "Skeleton" AI is a hostile monster's: on your side it stood still and only
// hit what came to it. The pets' "NecroPet" AI follows the owner. The Act 5 barbarians who fight
// beside you in the Bloody Foothills use "NpcBarb": allied, they go after the monsters around
// them and follow no one. This row is the Hell Bovine (same look, sounds, attacks and stats)
// with the barbarians' AI settings (their attacks are A1 and A2, as the cow's), allied and
// allowed in town, and with no loot.
const monstatsFile = 'global\\excel\\monstats.txt';
const monstats = D2RMM.readTsv(monstatsFile);
const minionId = 'lot_bovineminion';
{
  const cow = monstats.rows.find((r) => r.Id === 'hellbovine');
  const ally = monstats.rows.find((r) => r.Id === 'act5barb1');
  if (config.bovines && cow && ally && !monstats.rows.some((r) => r.Id === minionId)) {
    const row = Object.assign({}, cow);
    row.Id = minionId;
    row.NextInClass = '';
    monstats.headers.forEach((h) => {
      if (/^(AI|aidel|aidist|aip\d)/.test(h)) row[h] = ally[h] ?? '';
      if (/^TreasureClass/.test(h)) row[h] = '';
    });
    // neverCount: every allied summon in the game's own file has it (Valkyrie, golems,
    // skeletons, the barbarians). Without it the level counts the cow as one more monster to
    // kill, and as an ally its death is never counted, so the Den of Evil never clears.
    ['Align', 'inTown', 'neverCount'].forEach((h) => {
      if (monstats.headers.includes(h)) row[h] = ally[h] ?? '';
    });
    ['isSpawn', 'rarity'].forEach((h) => {
      if (monstats.headers.includes(h)) row[h] = '';
    });
    const hcColumn = monstats.headers.find((h) => h === '*hcIdx' || h === 'hcIdx');
    if (hcColumn) row[hcColumn] = String(monstats.rows.filter((r) => r.Id !== 'Expansion').length);
    monstats.rows.push(row);
    D2RMM.writeTsv(monstatsFile, monstats);
    // the HD look (as Horrors in the Night does for its copies)
    const modelsFile = 'hd\\character\\monsters.json';
    let models = null;
    try {
      models = D2RMM.readJson(modelsFile);
    } catch (e) {
      models = null;
    }
    if (models && models.hellbovine && !models[minionId]) {
      models[minionId] = models.hellbovine;
      D2RMM.writeJson(modelsFile, models);
    }
  }
}

// ---------------------------------------------------------------------------
// Numbers the plugin needs, as the game will count them after every installed mod:
//   bovine_pettype  row of lot_bovine in pettype.txt (absent = the plugin falls back to "revive")
//   bovine_minion   monstats row of the Hell Bovine Minion (rows counted without the "Expansion" line)
//   hellbovine      monstats row of the plain Hell Bovine (used only without the minion row)
{
  const lines = [];
  const petRow = pets.rows.findIndex((r) => r['pet type'] === petName);
  if (petRow >= 0) lines.push({ key: 'bovine_pettype', value: String(petRow) });
  const reviveRow = pets.rows.findIndex((r) => r['pet type'] === 'revive');
  if (reviveRow >= 0) lines.push({ key: 'revive_pettype', value: String(reviveRow) });

  let index = 0;
  monstats.rows.forEach((r) => {
    if (r.Id === 'Expansion') return;
    if (r.Id === 'hellbovine') lines.push({ key: 'hellbovine', value: String(index) });
    if (r.Id === minionId) lines.push({ key: 'bovine_minion', value: String(index) });
    index += 1;
  });
  lines.push({ key: 'all_zones', value: config.allZones ? '1' : '0' });

  D2RMM.writeTsv('global\\excel\\d2rloader\\legacy-of-terror\\info.tsv', { headers: ['key', 'value'], rows: lines });
}
