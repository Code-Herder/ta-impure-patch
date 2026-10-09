#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tagpu_recmp_arena.h"
#include "tagpu_detour.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); exit(1); } } while (0)
#define ENGINE_BASE 0x00400000u
#define ENGINE_MAIN 0x00511DE8u
#define PLAYER_AT 0x1B63u
#define PLAYER_SIZE 0x14Bu
#define UNIT_SIZE 0x118u

static unsigned mode, enabled, visits, refusals;
static RecMpRoster roster;
static unsigned char* world;
static void* allocation[3];
static unsigned allocated;
static unsigned char* engine_image;

static void* engine_address(unsigned address)
{
    CHECK(address>=ENGINE_BASE && address-ENGINE_BASE<0x200000);
    return engine_image+address-ENGINE_BASE;
}

static unsigned word(const void* data, unsigned at)
{
    unsigned value; memcpy(&value,(const unsigned char*)data+at,4); return value;
}

static unsigned half(const void* data, unsigned at)
{
    unsigned short value; memcpy(&value,(const unsigned char*)data+at,2); return value;
}

static void put(void* data, unsigned at, unsigned value, unsigned bytes)
{
    memcpy((unsigned char*)data+at,&value,bytes);
}

static RecMpRoster make_roster(unsigned count, unsigned each, unsigned turn)
{
    RecMpRoster r={ .version=1, .count=count, .slots=10*each+1, .each=each };
    for (unsigned i=0;i<count;i++) {
        unsigned block=(i*3+turn)%10, row=(i*7+turn+2)%10;
        r.actor[i]=(RecMpSeat){100+i,row,1+block*each,(block+1)*each};
    }
    return r;
}

static void expected_rows(const RecMpRoster* r, unsigned out[10])
{
    unsigned spare[10], count=0, next=0;
    for (unsigned row=0;row<10;row++) {
        unsigned i;
        for (i=0;i<r->count && r->actor[i].seat!=row;i++) {}
        if (i==r->count) spare[count++]=row;
    }
    for (unsigned block=0;block<10;block++) {
        unsigned i;
        for (i=0;i<r->count && r->actor[i].first!=1+block*r->each;i++) {}
        out[block]=i<r->count ? r->actor[i].seat : spare[next++];
    }
    CHECK(next==count);
}

static void copied_controls(void)
{
    uint32_t rows[10], ranks[10], saved[10];
    for (unsigned i=0;i<10;i++) rows[i]=0x11000000+i*PLAYER_SIZE;
    for (unsigned count=1;count<=10;count++) for (unsigned turn=0;turn<64;turn++) {
        unsigned expected[10];
        RecMpRoster r=make_roster(count,20,turn);
        for (unsigned i=0;i<10;i++) ranks[i]=rows[(i*3+turn)%10];
        expected_rows(&r,expected);
        CHECK(tagpu_recmp_arena_order(&r,rows,ranks));
        for (unsigned i=0;i<10;i++) CHECK(ranks[i]==rows[expected[i]]);
    }
    RecMpRoster r=make_roster(10,20,0);
    memcpy(ranks,rows,sizeof rows);
    CHECK(tagpu_recmp_arena_order(&r,ranks,ranks));
    for (unsigned bad=0;bad<9;bad++) {
        uint32_t source[10]; RecMpRoster broken=r;
        memcpy(source,rows,sizeof rows); memcpy(ranks,rows,sizeof rows);
        switch (bad) {
        case 0: source[0]=0; break;
        case 1: source[9]=source[0]; break;
        case 2: ranks[4]=ranks[3]; break;
        case 3: ranks[7]++; break;
        case 4: broken.actor[1].seat=broken.actor[0].seat; break;
        case 5: broken.actor[0].first++; break;
        case 6: broken.slots--; break;
        case 7: broken.count=11; break;
        case 8: broken.actor[0].last=broken.slots; break;
        }
        memcpy(saved,ranks,sizeof ranks);
        CHECK(!tagpu_recmp_arena_order(&broken,source,ranks));
        CHECK(!memcmp(saved,ranks,sizeof ranks)); refusals++;
    }
    CHECK(!tagpu_recmp_arena_order(NULL,rows,ranks));
    CHECK(!tagpu_recmp_arena_order(&r,NULL,ranks));
    CHECK(!tagpu_recmp_arena_order(&r,rows,NULL));
    refusals+=3;
    printf("COPIED_ARENA 640 permutations, overlap, %u unchanged-output refusals PASS\n",refusals);
}

/* The retail allocator's three heap calls are the only substituted engine
   dependency. Sorting, stack layout, slot IDs and paired ownership stores run
   as the original machine code, with absolute references relocated inside the
   fixture's own image. This never invokes the exe entry point. */
static void* __cdecl heap_new(const char* name, unsigned bytes)
{
    (void)name;
    CHECK(allocated<3 && bytes);
    allocation[allocated]=calloc(1,bytes); CHECK(allocation[allocated]);
    return allocation[allocated++];
}

static int __cdecl allocation_boundary(void* entry_esp)
{
    uint32_t rows[10];
    unsigned char* arena=(unsigned char*)(size_t)word(entry_esp,0x14);
    CHECK(arena==(unsigned char*)(size_t)word(world,0x14357));
    CHECK(allocated==3 && half(world,0x14351)==roster.slots);
    CHECK(half(world,0x37EE6)==roster.each);
    for (unsigned slot=0;slot<roster.slots;slot++) {
        const unsigned char* unit=arena+(size_t)slot*UNIT_SIZE;
        CHECK(half(unit,0xA8)==slot);
        CHECK(!half(unit,0xA6) && !word(unit,0x96) && !word(unit,0x9A));
        CHECK(!word(unit,0x5C) && !word(unit,0x60));
    }
    if (enabled) {
        for (unsigned i=0;i<10;i++) rows[i]=(uint32_t)(size_t)(world+PLAYER_AT+i*PLAYER_SIZE);
        CHECK(tagpu_recmp_arena_order(&roster,rows,(uint32_t*)((char*)entry_esp+0x18)));
        visits++;
    }
    return 0;
}

static void native_load(const char* path)
{
    FILE* file=fopen(path,"rb"); CHECK(file);
    CHECK(!fseek(file,0,SEEK_END)); long size=ftell(file); CHECK(size>0 && size<8*1024*1024);
    CHECK(!fseek(file,0,SEEK_SET));
    unsigned char* bytes=malloc((size_t)size); CHECK(bytes);
    CHECK(fread(bytes,1,(size_t)size,file)==(size_t)size); CHECK(!fclose(file));
    CHECK((size_t)size>=sizeof(IMAGE_DOS_HEADER));
    IMAGE_DOS_HEADER* dos=(IMAGE_DOS_HEADER*)bytes;
    CHECK(dos->e_magic==IMAGE_DOS_SIGNATURE && dos->e_lfanew>=0);
    CHECK((size_t)dos->e_lfanew+sizeof(IMAGE_NT_HEADERS32)<=(size_t)size);
    IMAGE_NT_HEADERS32* nt=(IMAGE_NT_HEADERS32*)(bytes+dos->e_lfanew);
    CHECK(nt->Signature==IMAGE_NT_SIGNATURE && nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386);
    CHECK(nt->OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC && nt->OptionalHeader.ImageBase==ENGINE_BASE);
    CHECK(nt->FileHeader.NumberOfSections>0 && nt->FileHeader.NumberOfSections<=16);
    unsigned image_bytes=nt->OptionalHeader.SizeOfImage;
    CHECK(image_bytes>0x11DE8+4 && image_bytes<=0x200000);
    unsigned char* image=engine_image;
    IMAGE_SECTION_HEADER* section=IMAGE_FIRST_SECTION(nt);
    CHECK((unsigned char*)(section+nt->FileHeader.NumberOfSections)<=bytes+size);
    for (unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        unsigned at=section[i].VirtualAddress, span=section[i].SizeOfRawData, source=section[i].PointerToRawData;
        CHECK(at<=image_bytes && span<=image_bytes-at && source<=(unsigned)size && span<=(unsigned)size-source);
        memcpy(image+at,bytes+source,span);
    }
    free(bytes);
    /* DISASSEMBLED: these operands in 0x4854A0..0x485974 are absolute MOV reads
       of the main pointer or PUSH arguments naming the comparator. Relative
       calls keep their displacement because the whole code image moves once.
       Check each instruction and old operand before retargeting that operand;
       neither the original file nor anyone else's mapping is changed. */
    static const unsigned main_operand[]={
        0x4854A4,0x4854B5,0x4854CA,0x4854DD,0x4854F7,0x48551C,0x48552C,
        0x485562,0x485584,0x4855A3,0x4855B2,0x4855E0,0x48560C,0x48561E,
        0x48562D,0x48565C,0x485794,0x48582D,0x485896,0x4858C2,0x485941
    };
    for (unsigned i=0;i<sizeof main_operand/sizeof *main_operand;i++) {
        unsigned char* operand=engine_address(main_operand[i]);
        CHECK(word(operand,0)==ENGINE_MAIN);
        CHECK(operand[-1]==0xA1 || (operand[-2]==0x8B && (operand[-1]&0xC7)==5));
        put(operand,0,(unsigned)(size_t)engine_address(ENGINE_MAIN),4);
    }
    static const unsigned comparator_operand[]={0x48568B,0x485744,0x485765};
    for (unsigned i=0;i<sizeof comparator_operand/sizeof *comparator_operand;i++) {
        unsigned char* operand=engine_address(comparator_operand[i]);
        CHECK(operand[-1]==0x68 && word(operand,0)==0x485940);
        put(operand,0,(unsigned)(size_t)engine_address(0x485940),4);
    }
    static const unsigned char first[9]={0x33,0xFF,0xC6,0x85,0xFF,0x00,0x00,0x00,0xFF};
    CHECK(tagpu_detour_observe((unsigned)(size_t)engine_address(0x0048587Eu),first,sizeof first,allocation_boundary,NULL));
    CHECK(tagpu_detour_land((unsigned)(size_t)engine_address(0x004D83B0u),(unsigned char*)(size_t)heap_new,5));
}

static void native_case(unsigned count, unsigned each, unsigned turn, unsigned game_mode, unsigned ordered)
{
    unsigned expected[10];
    roster=make_roster(count,each,turn); enabled=ordered; mode=game_mode;
    memset(world,0,0x40000);
    put(world,0x391E9,(unsigned)(size_t)&mode,4); put(world,0x37EE6,each,2);
    put(engine_address(ENGINE_MAIN),0,(unsigned)(size_t)world,4);
    for (unsigned row=0;row<10;row++) {
        unsigned char* player=world+PLAYER_AT+row*PLAYER_SIZE;
        player[0x146]=(unsigned char)row; player[0x73]=(unsigned char)(1+row%3);
        put(player,4,1+(row*7+3)%10,4);
    }
    ((void (__cdecl*)(void))engine_address(0x004854A0u))();
    CHECK(allocated==3 && half(world,0x14351)==roster.slots);
    unsigned char* arena=(unsigned char*)(size_t)word(world,0x14357);
    CHECK(word(world,0x1435B)==(unsigned)(size_t)(arena+(roster.slots-1)*UNIT_SIZE));
    CHECK(arena[0xFF]==255 && !word(arena,0x96) && !half(arena,0xA8));
    if (ordered) expected_rows(&roster,expected);
    else for (unsigned block=0;block<10;block++) {
        if (game_mode==2) expected[block]=block;
        else for (unsigned row=0;row<10;row++) if ((row*7+3)%10==block) expected[block]=row;
    }
    for (unsigned block=0;block<10;block++) {
        unsigned row=expected[block], first=1+block*each, last=(block+1)*each;
        const unsigned char* player=world+PLAYER_AT+row*PLAYER_SIZE;
        CHECK(word(player,0x67)==(unsigned)(size_t)(arena+first*UNIT_SIZE));
        CHECK(word(player,0x6B)==(unsigned)(size_t)(arena+last*UNIT_SIZE));
        CHECK(half(player,0x6F)==first && half(player,0x71)==last);
        for (unsigned slot=first;slot<=last;slot++) {
            const unsigned char* unit=arena+(size_t)slot*UNIT_SIZE;
            CHECK(word(unit,0x96)==(unsigned)(size_t)player && unit[0xFF]==row);
            CHECK(word(unit,0xAC)==UINT32_MAX && half(unit,0xA8)==slot);
            CHECK(!half(unit,0xA6) && !word(unit,0x9A));
        }
    }
    for (unsigned i=0;i<3;i++) { free(allocation[i]); allocation[i]=NULL; }
    allocated=0;
}

int main(int argc, char** argv)
{
    CHECK(argc==2);
    engine_image=VirtualAlloc(NULL,0x200000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE);
    CHECK(engine_image);
    copied_controls(); native_load(argv[1]);
    world=calloc(1,0x40000); CHECK(world);
    native_case(4,20,0,2,0); native_case(4,20,0,3,0);
    for (unsigned count=1;count<=10;count++) for (unsigned turn=0;turn<10;turn++)
        for (unsigned game_mode=2;game_mode<=3;game_mode++) native_case(count,20,turn,game_mode,1);
    const unsigned each[]={1,1500,6553};
    for (unsigned i=0;i<3;i++) for (unsigned game_mode=2;game_mode<=3;game_mode++)
        native_case(10,each[i],9,game_mode,1);
    CHECK(visits==206); free(world);
    printf("NATIVE_ARENA %u reordered allocations, two untouched ordering controls, all ten paired blocks PASS\n",visits);
    puts("PASS: relocated retail allocator execution; full game and saved playback remain untested");
    return 0;
}
