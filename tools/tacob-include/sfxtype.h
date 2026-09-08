// sfxtype.h — the values `emit-sfx TYPE from PIECE` takes. Stock SmokeUnit writes
// 256 | 1 (white) and 256 | 2 (black), which is where 257 and 258 come from; the rest are
// the community's names for the engine's effect kinds and are not yet verified against the
// EMIT_SFX handler (tacob-design.md, gaps).
#define SFXTYPE_VTOL           0
#define SFXTYPE_THRUST         1
#define SFXTYPE_WAKE1          2
#define SFXTYPE_WAKE2          3
#define SFXTYPE_REVERSEWAKE1   4
#define SFXTYPE_REVERSEWAKE2   5
#define SFXTYPE_WHITESMOKE   257
#define SFXTYPE_BLACKSMOKE   258
#define SFXTYPE_SUBBUBBLES   259
