#pragma once
// arg_p3_dork.h — P3 "Dork" (handoff §6.2/§6.3): a small Zork-parody text
// adventure through a cave of DEFCON villages. Spine: SOLDER SPOOL -> Gremlin ->
// BLINKY SAO -> Mallory -> LANYARD (+unlock) -> Bouncer -> WRISTBAND (+unlock) ->
// Queue -> the per-badge MAC passphrase -> P3 complete. Soft-lock-proof: spine
// items are never consumed by wrong actions; Queue is a fourth-wall safety net.
// Full state persists to NVS (p3state) and resumes on re-enter (§6.3).
// Design spec by a content agent; only the Queue passphrase is per-badge (§2.8).

#include "arg_clipcli.h"

// directions: 0=n 1=s 2=e 3=w 4=u 5=d
#define P3_NROOM 14
#define P3_NITEM 8
// quest flags
#define QF_SAO   0x01
#define QF_LANY  0x02
#define QF_WRIST 0x04
#define QF_QUEUE 0x08

struct P3Room { const char *name; const char *desc; int8_t exit[6]; uint8_t lock[6]; };
static const P3Room P3R[P3_NROOM] = {
 {"Cave Mouth","A cooling-fan breeze pushes out of the dark. A sign: 'BADGE REQUIRED BEYOND THIS POINT (you have one. congrats).'",{1,-1,2,-1,-1,-1},{0,0,0,0,0,0}},
 {"Switchback Tunnel","LED rope-light staples the rock. Graffiti: 'GOON TERRITORY' and, smaller, '(be nice)'.",{3,0,4,-1,-1,-1},{0,0,0,0,0,0}},
 {"RF Village","A forest of antennas hums. A HAM in a vest is excited about a frequency you can't hear.",{5,-1,-1,0,-1,-1},{0,0,0,0,0,0}},
 {"The Commons","A wide cavern, the social heart of the complex. Tunnels branch everywhere. A coffee urn labeled 'FREE (lie)' steams.",{6,1,7,8,-1,9},{0,0,0,0,0,0}},
 {"Lockpick Village","Pin tumblers the size of melons hang as art. A hooded figure picks a padlock blindfolded, smug.",{10,-1,-1,1,-1,-1},{0,0,0,0,0,0}},
 {"Antenna Gallery","A dead end of beautiful, useless dishes pointed at nothing. A whiteboard: 'IT'S ALWAYS DNS.' It is not, here.",{-1,2,-1,-1,-1,-1},{0,0,0,0,0,0}},
 {"Social Engineering Village","Warm light, free lanyards, suspiciously friendly. A woman works a booth marked 'TRUST ME - Q&A' - her badge says Mallory. A velvet rope blocks the north tunnel.",{11,3,-1,-1,-1,-1},{QF_LANY,0,0,0,0,0}},
 {"Hardware Hacking Village","Soldering-iron smell. Dev boards crucified to corkboard. A gremlin solders something definitely not to spec.",{-1,-1,12,3,-1,-1},{0,0,0,0,0,0}},
 {"AI Village","Screens scroll confident wrong answers. A chatbot apologizes for things it didn't do, repeatedly.",{-1,-1,3,-1,-1,-1},{0,0,0,0,0,0}},
 {"The Underqueue","Stairs spiral down toward a glow and the shuffle of a line that never moves.",{-1,-1,-1,-1,3,13},{0,0,0,0,0,QF_WRIST}},
 {"Lockpick Deep Vault","A 'vault' propped open with a folding chair. Inside: snacks, and dignity left behind by faster pickers.",{-1,4,-1,-1,-1,-1},{0,0,0,0,0,0}},
 {"VIP Mixer","Past the velvet rope. A Bouncer (no neck, all clipboard) guards a down-stair. He eyes your wrist.",{-1,6,-1,-1,-1,9},{0,0,0,0,0,QF_WRIST}},
 {"Soldering Annex","A fume hood, a 'WASH HANDS - LEAD' sign, a bin of bent resistors shaped like tiny disappointed people.",{-1,-1,-1,7,-1,-1},{0,0,0,0,0,0}},
 {"The Queue","The line curls back on itself forever. At its head stands Queue, and behind him a door of pure standby-light.",{-1,-1,-1,-1,9,-1},{0,0,0,0,0,0}},
};

struct P3Item { const char *kw; const char *name; const char *examine; int8_t start; bool crit; };
static const P3Item P3I[P3_NITEM] = {
 {"solder",  "SOLDER SPOOL", "Leaded solder. Smells like the 90s and poor ventilation.", 12, true},
 {"sao",     "BLINKY SAO",   "A half-finished add-on board. One LED blinks hopefully.",  -1, true},
 {"lanyard", "LANYARD",      "A laminated 'TRUST ME' lanyard. Astonishing social proof.", -1, true},
 {"wristband","WRISTBAND",   "A glowing VIP wristband. The good kind. Non-transferable, allegedly.", -1, true},
 {"duck",    "RUBBER DUCKY", "A debugging companion and/or USB attack platform. It judges your code silently.", 10, false},
 {"pick",    "LOCKPICK SET", "A practice pick set. The blindfolded picker insists you 'don't need to see.'", 4, false},
 {"coffee",  "COLD COFFEE",  "'FREE' coffee, now cold. A paperweight with caffeine guilt.", 3, false},
 {"sticker", "STICKER SHEET","DEFCON village stickers. Currency of the realm. No one will trade theirs.", 2, false},
};
#define P3_CARRIED -2

// ── state (persisted blob, §6.3) ───────────────────────────────────────────
static uint8_t p3_room = 0;
static int8_t  p3_iloc[P3_NITEM];   // item location: room id, P3_CARRIED, or -1 (limbo/given)
static uint8_t p3_qf = 0;           // quest flags
static uint8_t p3_replay = 0;
static bool    p3_in = false;
static uint8_t p3_hintn = 0;
static uint8_t p3_deadends = 0;   // dead-ends visited (exploration reward): [5]=1 [8]=2 [10]=4 [12]=8
#define P3_ALL_DEADENDS 0x0F

static void p3_save(void) {
    if (p3_replay) return;
    arg_prefs.begin(ARG_NVS_NS, false);
    uint8_t blob[2 + P3_NITEM + 1];
    blob[0] = p3_room; blob[1] = p3_qf;
    for (int i = 0; i < P3_NITEM; i++) blob[2+i] = (uint8_t)p3_iloc[i];
    blob[2 + P3_NITEM] = p3_deadends;
    arg_prefs.putBytes("p3state", blob, sizeof blob);
    arg_prefs.end();
}
static void p3_fresh(void) {
    p3_room = 0; p3_qf = 0; p3_deadends = 0;
    for (int i = 0; i < P3_NITEM; i++) p3_iloc[i] = P3I[i].start;
}
static void p3_load(void) {
    arg_prefs.begin(ARG_NVS_NS, true);
    uint8_t blob[2 + P3_NITEM + 1];
    size_t n = arg_prefs.getBytes("p3state", blob, sizeof blob);
    arg_prefs.end();
    if (n == sizeof blob) {
        p3_room = blob[0]; p3_qf = blob[1];
        for (int i = 0; i < P3_NITEM; i++) p3_iloc[i] = (int8_t)blob[2+i];
        p3_deadends = blob[2 + P3_NITEM];
        // Harden against a corrupt-but-right-sized blob: p3_room indexes P3R[]
        // directly (const char* derefs), so an out-of-range value would crash.
        if (p3_room >= P3_NROOM) { p3_fresh(); }
    } else p3_fresh();
}

// p3_passphrase() lives in arg_core.h now (shared with the L.E.E.T. callsign display).

// ── helpers ─────────────────────────────────────────────────────────────────
static const char *P3DIR[6] = {"north","south","east","west","up","down"};
static int p3_dir(const char *w) {
    if (!strcmp(w,"n")||!strcmp(w,"north")) return 0;
    if (!strcmp(w,"s")||!strcmp(w,"south")) return 1;
    if (!strcmp(w,"e")||!strcmp(w,"east"))  return 2;
    if (!strcmp(w,"w")||!strcmp(w,"west"))  return 3;
    if (!strcmp(w,"u")||!strcmp(w,"up"))    return 4;
    if (!strcmp(w,"d")||!strcmp(w,"down"))  return 5;
    return -1;
}
static int p3_item_by_kw(const char *w) {
    // Synonyms for items whose NAME has a word their kw doesn't (so "get spool"
    // works as well as "get solder"). ("ducky"/"band" already substring-match.)
    static const struct { const char *alias; uint8_t item; } P3SYN[] = {
        { "spool",  0 },  // SOLDER SPOOL
        { "blinky", 1 },  // BLINKY SAO ("sao" kw doesn't contain "blinky") -- Bryce
    };
    for (auto &s : P3SYN) if (!strcmp(w, s.alias)) return s.item;
    for (int i = 0; i < P3_NITEM; i++) if (strstr(P3I[i].kw, w) || strstr(w, P3I[i].kw)) return i;
    return -1;
}
static bool p3_have(int i) { return i>=0 && p3_iloc[i]==P3_CARRIED; }

static void p3_describe(void) {
    const P3Room *r = &P3R[p3_room];
    // exploration reward: remember which dead-ends the player has set foot in
    uint8_t de_before = p3_deadends;
    switch (p3_room) {
        case 5:  p3_deadends |= 1; break;  case 8:  p3_deadends |= 2; break;
        case 10: p3_deadends |= 4; break;  case 12: p3_deadends |= 8; break;
    }
    if (p3_deadends != de_before) p3_save();   // persist a newly-found dead-end
    Serial.printf("\r\n== %s ==\r\n%s\r\n", r->name, r->desc);
    // items on the floor
    for (int i = 0; i < P3_NITEM; i++)
        if (p3_iloc[i] == p3_room) Serial.printf("There is a %s here.\r\n", P3I[i].name);
    // exits
    Serial.print(F("Exits:"));
    for (int d = 0; d < 6; d++) if (r->exit[d] >= 0) Serial.printf(" %s", P3DIR[d]);
    Serial.print(F("\r\n> "));
}

// ── NPC interactions (room-scoped) ──────────────────────────────────────────
// give item -> npc in the current room. Returns true if consumed.
static void p3_give(int it, const char *who) {
    (void)who;
    if (it < 0) { Serial.println(F("Give what? (try 'inventory')")); Serial.print(F("> ")); return; }
    if (!p3_have(it)) { Serial.printf("You aren't carrying the %s.\r\n> ", P3I[it].name); return; }
    // Gremlin in HW Hacking (7): wants SOLDER
    if (p3_room==7 && it==0) {
        p3_iloc[0]=-1; p3_qf|=QF_SAO; p3_iloc[1]=P3_CARRIED;
        Serial.println(F("The Gremlin grabs the spool: 'Solder after midnight - my favorite crime.'"));
        Serial.println(F("He finishes the board and presses a BLINKY SAO into your hand.")); p3_save();
    }
    // Mallory in SE Village (6): wants BLINKY SAO
    else if (p3_room==6 && it==1) {
        p3_iloc[1]=-1; p3_qf|=QF_LANY; p3_iloc[2]=P3_CARRIED;
        Serial.println(F("Mallory beams: 'Ooh, shiny. I'd never lie to you.' She clips a TRUST ME LANYARD on you"));
        Serial.println(F("and drops the velvet rope. The north tunnel is open.")); p3_save();
    }
    // Queue (13): doesn't want things
    else if (p3_room==13) {
        Serial.printf("Queue: 'I am beyond *things*. I want the word. Keep the %s.'\r\n", P3I[it].name);
    }
    else Serial.printf("They don't want the %s. (It stays with you.)\r\n", P3I[it].name);
    Serial.print(F("> "));
}

static void p3_use(int it) {
    // use lanyard at the Bouncer (VIP Mixer 11)
    if (p3_room==11 && it==2 && (p3_qf&QF_LANY)) {
        if (p3_qf&QF_WRIST) { Serial.println(F("The Bouncer already waved you through. 'Go on.'")); }
        else { p3_qf|=QF_WRIST; p3_iloc[3]=P3_CARRIED;
            Serial.println(F("The Bouncer scans the lanyard, grunts, and snaps a VIP WRISTBAND on you."));
            Serial.println(F("'List says you're fine. The stairs DOWN are open -- go on.'")); p3_save(); }
    } else if (it>=0 && p3_have(it)) {
        Serial.printf("You wave the %s around. Nothing happens here.\r\n", P3I[it].name);
    } else Serial.println(F("Use what? (something you're carrying)"));
    Serial.print(F("> "));
}

static void p3_talk(const char *who) {
    (void)who;
    switch (p3_room) {
        case 2:  Serial.println(F("HAM: '73! The whole complex feeds toward the Commons - and there's always a queue past the stairs.'")); break;
        case 7:  Serial.println(F("Gremlin: 'Bring me a SOLDER SPOOL and I'll finish this SAO. Never solder after midnight. I always do.'")); break;
        case 6:  Serial.println(F("Mallory: 'Hi! I'm Mallory. Bring me something shiny and blinky and I'll get you *access*.'")); break;
        case 11: if (p3_qf&QF_LANY) p3_use(2); else Serial.println(F("Bouncer: 'No lanyard, no list, no entry. I don't make the rules. I AM the rules.'")); return;
        case 13: Serial.println(F("Queue: 'Mon capitaine. You made it to the front. Now prove the rig is LIVE, not a recording.'"));
                 Serial.println(F("'Your rig was issued a word the moment it woke - stamped on its L.E.E.T. sheet. SPEAK it: say <word>.'")); break;
        default: Serial.println(F("There's no one here worth talking to.")); break;
    }
    Serial.print(F("> "));
}

static void p3_win(void) {
    p3_qf |= QF_QUEUE; p3_save();
    if (!p3_replay) arg_set_flag(ARG_P3_DORK);
    Serial.println(F("\r\nQueue: 'Magnifique. Live, current, continuous - the rare trifecta. The line parts.'"));
    Serial.println(F("The door of standby-light swings wide. [P3 COMPLETE - access granted.]"));
    // Exploration reward: turn over every dead-end AND carry the RUBBER DUCKY out
    // (it lives in the Lockpick Deep Vault dead-end) -> unlock the Rubber Ducky LED
    // theme. The duck is the tangible token, so picking it up actually matters.
    bool got_duck = p3_have(4);   // item 4 = RUBBER DUCKY, carried to the finish
    if (p3_deadends == P3_ALL_DEADENDS && got_duck) {
        if (!p3_replay) arg_set_duck();
        Serial.println(F("Queue: 'And you turned over every stone - even the ones clearly marked DEAD END."));
        Serial.println(F("        Exhausting. Admirable. Mostly exhausting.' He waves a bored hand. 'That rubber"));
        Serial.println(F("        duck you fished out of the vault has, against all reason, possessed your lights."));
        Serial.println(F("        The Rubber Ducky show is yours. Try not to look so pleased with yourself.'"));
        Serial.println(F("[CLIP] Unlocked: DATA > LEDs > All LEDs > Rubber Duck."));
    } else if (p3_deadends == P3_ALL_DEADENDS) {
        // Explored everything but left the duck behind -- tell them, so the missable
        // reward isn't a mystery (a determined player can 'clipcli reset' to retry).
        Serial.println(F("Queue: 'You turned over every stone... and left the one that mattered sitting in the"));
        Serial.println(F("        vault. That rubber debugging duck WAS the prize - it possesses the lights, when"));
        Serial.println(F("        someone bothers to pocket it. Yours stay ordinary.' A small, disappointed sniff."));
        Serial.println(F("[CLIP] (The RUBBER DUCKY in the Lockpick Deep Vault was the key. 'clipcli reset' to try again.)"));
    } else {
        Serial.println(F("Queue: 'Though you read the word off your own sheet and marched straight up. The quick"));
        Serial.println(F("        and easy path - a certain small green creature warned you about that one.' He"));
        Serial.println(F("        inspects a fingernail. 'There were corners you never turned, a prize in one of them."));
        Serial.println(F("        X never marks the spot, a wiser adventurer said. You didn't even check the X.'"));
    }
    Serial.println(F("[CLIP] 'clipcli challenge' for what's next."));
    p3_in = false; arg_session_end();
}

static void p3_say(const char *word) {
    if (p3_room != 13) { Serial.println(F("You say it to the cave. The cave is unmoved.")); Serial.print(F("> ")); return; }
    char pass[8]; p3_passphrase(pass);
    char up[16]; int n=0; for (const char*q=word; *q && n<15; q++) if(!isspace((unsigned char)*q)) up[n++]=toupper((unsigned char)*q); up[n]='\0';
    if (!strcmp(up, pass)) { p3_win(); return; }
    Serial.println(F("Queue: 'That's *a* word. It is not *your* word. No penalty - type the one your badge holds.'"));
    Serial.print(F("> "));
}

static void p3_hint(int level);

// ── parser (returns true: consumed) ─────────────────────────────────────────
static bool p3_line(const char *line) {
    const char *pp = line; while (*pp==' ') pp++;
    if (!strncmp(pp,"clipcli",7)) return false;   // let commands interleave
    // tokenize (lowercase) into up to 4 words
    char buf[80]; int bl=0; for (const char*q=pp; *q && bl<79; q++) buf[bl++]=tolower((unsigned char)*q); buf[bl]='\0';
    char *w[4]={0,0,0,0}; int wc=0;
    for (char *t=strtok(buf," \t"); t && wc<4; t=strtok(NULL," \t")) w[wc++]=t;
    if (wc==0) { Serial.print(F("> ")); return true; }
    const char *v = w[0];

    if (!strcmp(v,"quit")||!strcmp(v,"exit")||!strcmp(v,"leave")||!strcmp(v,"q")) {
        p3_in = false; arg_session_end();   // back to the CLI; NVS p3state untouched
        Serial.println(F("You slip back out of the cave. Your place is saved."));
        Serial.println(F("[CLIP] 'clipcli challenge' to drop back in."));
        return true;
    }

    // bare direction
    int d = p3_dir(v);
    if (d<0 && !strcmp(v,"go") && w[1]) d = p3_dir(w[1]);
    if (d>=0) {
        const P3Room *r=&P3R[p3_room];
        if (r->exit[d]<0) { Serial.println(F("You can't go that way.")); Serial.print(F("> ")); return true; }
        if (r->lock[d] && !(p3_qf & r->lock[d])) {
            Serial.println(p3_room==6 ? F("A velvet rope blocks the way. (Someone needs to drop it for you.)")
                                      : F("The stairs are roped off. You need a wristband.")); Serial.print(F("> ")); return true; }
        p3_room=(uint8_t)r->exit[d]; p3_save(); p3_describe(); return true;
    }
    if (!strcmp(v,"look")||!strcmp(v,"l")) { p3_describe(); return true; }
    if (!strcmp(v,"verbs")||!strcmp(v,"help")) {
        Serial.println(F("verbs: go(n/s/e/w/u/d) look examine take drop use give talk inventory hint say quit")); Serial.print(F("> ")); return true; }
    if (!strcmp(v,"inventory")||!strcmp(v,"i")) {
        bool any=false; Serial.println(F("You are carrying:"));
        for (int i=0;i<P3_NITEM;i++) if (p3_iloc[i]==P3_CARRIED){ Serial.printf("  %s\r\n",P3I[i].name); any=true; }
        if(!any) Serial.println(F("  (nothing)")); Serial.print(F("> ")); return true; }
    if (!strcmp(v,"examine")||!strcmp(v,"x")) {
        int i=w[1]?p3_item_by_kw(w[1]):-1;
        if (i>=0 && (p3_have(i)||p3_iloc[i]==p3_room)) Serial.println(P3I[i].examine);
        else Serial.println(F("You don't see that here.")); Serial.print(F("> ")); return true; }
    if (!strcmp(v,"take")||!strcmp(v,"get")) {
        int i=w[1]?p3_item_by_kw(w[1]):-1;
        if (i>=0 && p3_iloc[i]==p3_room){ p3_iloc[i]=P3_CARRIED; Serial.printf("Taken: %s.\r\n",P3I[i].name); p3_save(); }
        else Serial.println(F("There's nothing like that to take here.")); Serial.print(F("> ")); return true; }
    if (!strcmp(v,"drop")) {
        int i=w[1]?p3_item_by_kw(w[1]):-1;
        if (p3_have(i)){ p3_iloc[i]=p3_room; Serial.printf("Dropped: %s.\r\n",P3I[i].name); p3_save(); }
        else Serial.println(F("You aren't carrying that.")); Serial.print(F("> ")); return true; }
    if (!strcmp(v,"give")) {   // give X [to Y]
        int i=w[1]?p3_item_by_kw(w[1]):-1; p3_give(i, w[3]?w[3]:w[2]); return true; }
    if (!strcmp(v,"use")) { int i=w[1]?p3_item_by_kw(w[1]):-1; p3_use(i); return true; }
    if (!strcmp(v,"talk")) { p3_talk(w[2]?w[2]:w[1]); return true; }
    if (!strcmp(v,"say"))  { p3_say(w[1]?w[1]:""); return true; }
    if (!strcmp(v,"hint")) { p3_hint(++p3_hintn>3?3:p3_hintn); return true; }
    Serial.println(F("I don't know how to do that; try 'verbs'.")); Serial.print(F("> "));
    return true;
}

static void p3_hint(int level) {
    if (level<=1) Serial.println(F("[CLIP] Everything flows to the Commons, and everyone wants something first. Trade up. Talk to people."));
    else if (level==2) Serial.println(F("[CLIP] Finish the blinky thing in Hardware Hacking, sweet-talk Social Engineering, then get past the Bouncer."));
    else {  // state-aware
        if (!(p3_qf&QF_SAO))       Serial.println(F("[CLIP] The Gremlin (Hardware Hacking, east of the Commons) wants a SOLDER SPOOL. It's in the Soldering Annex just past him - east again. Grab it, then 'give solder'."));
        else if (!(p3_qf&QF_LANY)) Serial.println(F("[CLIP] Go north to Social Engineering and 'give sao to mallory'."));
        else if (!(p3_qf&QF_WRIST))Serial.println(F("[CLIP] Go north to the VIP Mixer and 'use lanyard' on the Bouncer."));
        else                       Serial.println(F("[CLIP] From the Commons go down, down, then 'talk to queue' and 'say' your word - it's on your L.E.E.T. screen (STATS tab)."));
    }
    Serial.print(F("> "));
}

static void p3_enter(bool replay) {
    p3_replay = replay; p3_in = true; p3_hintn = 0;
    if (replay) p3_fresh(); else p3_load();
    arg_session_active = true; arg_session_line = p3_line;
    Serial.println(F("\r\n=== THE CAVE COMPLEX ==="));
    Serial.println(F("SIGNAL_9's trial drops you into the warren under the con. Type 'verbs' for what you can do."));
    if (replay) Serial.println(F("(replay) read-only - nothing here is saved."));
    p3_describe();
}

static void p3_reset(void) {
    p3_fresh();
    arg_prefs.begin(ARG_NVS_NS, false); arg_prefs.remove("p3state"); arg_prefs.end();
}

// Full map/content dump for design review (serial `p3map`). Rooms + exits + locks
// + items are read straight from P3R/P3I (always in sync); the NPC/spine lines are
// a fixed design summary (NPCs live in p3_give/p3_talk logic, not a data table).
static const char *p3_qf_name(uint8_t f) {
    switch (f) { case QF_SAO: return "SAO"; case QF_LANY: return "LANYARD";
                 case QF_WRIST: return "WRISTBAND"; case QF_QUEUE: return "QUEUE"; default: return "?"; }
}
static void p3_map_dump(void) {
    Serial.println(F("\r\n=== P3 \"DORK\" - FULL MAP ==="));
    Serial.println(F("ROOMS  (exit: dir -> [id] name; (needs FLAG) = locked until you earn it)"));
    for (int i = 0; i < P3_NROOM; i++) {
        Serial.printf("[%2d] %s\r\n", i, P3R[i].name);
        for (int d = 0; d < 6; d++) {
            int8_t t = P3R[i].exit[d];
            if (t < 0) continue;
            Serial.printf("      %-5s -> [%2d] %s", P3DIR[d], t, P3R[t].name);
            if (P3R[i].lock[d]) Serial.printf("   (needs %s)", p3_qf_name(P3R[i].lock[d]));
            Serial.print(F("\r\n"));
        }
    }
    Serial.println(F("ITEMS  (kw : NAME @ start;  * = spine/critical)"));
    for (int i = 0; i < P3_NITEM; i++)
        Serial.printf("   %-9s : %s%s  @ %s\r\n", P3I[i].kw, P3I[i].name, P3I[i].crit ? " *" : "",
                      P3I[i].start < 0 ? "(handed to you by an NPC)" : P3R[P3I[i].start].name);
    Serial.println(F("NPCs   (room : who -> what they want)"));
    Serial.println(F("   [ 2] RF Village          : HAM     -> talk (lore + directions)"));
    Serial.println(F("   [ 7] Hardware Hacking     : Gremlin -> give solder      => BLINKY SAO"));
    Serial.println(F("   [ 6] Social Engineering   : Mallory -> give sao         => LANYARD (drops the velvet rope)"));
    Serial.println(F("   [11] VIP Mixer            : Bouncer -> use lanyard      => WRISTBAND (opens the down-stair)"));
    Serial.println(F("   [13] The Queue            : Queue   -> say <per-badge word>  (serial `p3pass` reveals it)"));
    Serial.println(F("SPINE  solder(12) -> Gremlin(7) -> sao -> Mallory(6) -> lanyard -> Bouncer(11) -> wristband -> down to Underqueue(9) -> down to Queue(13) -> say the word"));
    Serial.println(F("VERBS  go(n/s/e/w/u/d) look examine take drop use give talk inventory hint say verbs"));
    Serial.println(F("=== end map ==="));
}

static void p3_register(void) {
    arg_puzzles[3].enter = p3_enter;
    arg_puzzles[3].hint  = p3_hint;
    arg_puzzles[3].reset = p3_reset;
}
