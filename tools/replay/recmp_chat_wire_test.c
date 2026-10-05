#include "tagpu_recmp_chat_wire.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %u: %s\n",__LINE__,#x); return 1; } } while (0)

int main(void)
{
    GUID match={1,2,3,{4,5,6,7,8,9,10,11}}, other=match, zero={0};
    RecMpChatWire a,b,out,sentinel,bad;
    unsigned char stock[RECMP_CHAT_STOCK_BYTES];
    unsigned n, refused=0;
    SYSTEM_INFO system;
    unsigned char* pages;
    DWORD old;
    memset(stock,'x',sizeof stock); stock[0]=5;
    CHECK(tagpu_recmp_chat_wire_make(&a,&match,1,0,stock,sizeof stock));
    CHECK(tagpu_recmp_chat_wire_make(&b,&match,2,UINT32_MAX,stock,sizeof stock));
    CHECK(a.event!=b.event && !memcmp(a.stock,b.stock,sizeof stock));
    CHECK(tagpu_recmp_chat_wire_read(&out,&match,&a,sizeof a) && !memcmp(&out,&a,sizeof a));
    CHECK(tagpu_recmp_chat_wire_read(&b,&match,&b,sizeof b) && b.event==2);
    CHECK(tagpu_recmp_chat_wire_make(&a,&a.match,UINT64_MAX,17,a.stock,sizeof stock));
    CHECK(tagpu_recmp_chat_wire_read(&a,&a.match,&a,sizeof a) && a.event==UINT64_MAX);
    memset(&sentinel,0xA5,sizeof sentinel);
    GetSystemInfo(&system);
    pages=VirtualAlloc(NULL,2*system.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(pages && VirtualProtect(pages+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old));
    for (n=0;n<=sizeof a;n++) {
        unsigned char* end=pages+system.dwPageSize-n;
        if (n) memcpy(end,&a,n);
        out=sentinel;
        if (n==sizeof a) CHECK(tagpu_recmp_chat_wire_read(&out,&match,end,n));
        else { CHECK(!tagpu_recmp_chat_wire_read(&out,&match,end,n)); CHECK(!memcmp(&out,&sentinel,sizeof out)); refused++; }
    }
    for (n=0;n<=sizeof stock;n++) {
        unsigned char* end=pages+system.dwPageSize-n;
        if (n) memcpy(end,stock,n);
        out=sentinel;
        if (n==sizeof stock) CHECK(tagpu_recmp_chat_wire_make(&out,&match,1,17,end,n));
        else { CHECK(!tagpu_recmp_chat_wire_make(&out,&match,1,17,end,n)); CHECK(!memcmp(&out,&sentinel,sizeof out)); refused++; }
    }
    CHECK(VirtualFree(pages,0,MEM_RELEASE));
    other.Data1++;
    out=sentinel;
    CHECK(!tagpu_recmp_chat_wire_read(&out,&other,&a,sizeof a));
    CHECK(!tagpu_recmp_chat_wire_read(&out,&zero,&a,sizeof a));
    CHECK(!tagpu_recmp_chat_wire_read(&out,&match,&a,sizeof a+1));
    CHECK(!tagpu_recmp_chat_wire_make(&out,&zero,1,0,stock,sizeof stock));
    CHECK(!tagpu_recmp_chat_wire_make(&out,&match,0,0,stock,sizeof stock));
    CHECK(!memcmp(&out,&sentinel,sizeof out));
    for (n=0;n<6;n++) {
        bad=a;
        switch (n) {
            case 0: bad.code=5; break;
            case 1: bad.version++; break;
            case 2: bad.bytes--; break;
            case 3: bad.event=0; break;
            case 4: bad.stock[0]=0x0B; break;
            case 5: bad.stock[1]=0; break;
        }
        out=sentinel;
        CHECK(!tagpu_recmp_chat_wire_read(&out,&match,&bad,sizeof bad));
        CHECK(!memcmp(&out,&sentinel,sizeof out)); refused++;
    }
    stock[1]=0;
    CHECK(!tagpu_recmp_chat_wire_make(&out,&match,1,0,stock,sizeof stock));
    CHECK(!memcmp(&out,&sentinel,sizeof out));
    printf("PASS: repeated/full-length text, maximum serial, alias-safe copies, %u guarded/malformed refusals; output retained\n",refused);
    return 0;
}
