// exptype.h — the flags `explode PIECE type ...` takes, as the community has always named
// them. Values: stock Killed scripts compile BITMAPONLY | BITMAP1 to 32 | 256 and
// FALL | SMOKE | FIRE | EXPLODE_ON_HIT to 4 | 8 | 16 | 2 (tools/tacob dump armstump Killed).
// The engine-side handler has not been located yet; see tacob-design.md, gaps.
#define SHATTER          1
#define EXPLODE_ON_HIT   2
#define FALL             4
#define SMOKE            8
#define FIRE            16
#define BITMAPONLY      32
#define BITMAP1        256
#define BITMAP2        512
#define BITMAP3       1024
#define BITMAP4       2048
#define BITMAP5       4096
#define BITMAPNUKE    8192
