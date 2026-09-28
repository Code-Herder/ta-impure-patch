/* Research-only DirectPlay host. The Python driver owns the TA replay protocol.
   Build: i686-w64-mingw32-gcc -O2 -Wall dplay_bridge.c -lole32 -luuid -o bridge.exe
   stdin: SEND <from> <to> <hex>; STATE <u1> <u2> <u3> <u4>; STOP
   stdout: READY <drone1> <drone2>; RX <from> <to> <hex>; ERROR <operation> <hr>
   All DirectPlay calls run on this thread. No engine memory is accessed. */
#define INITGUID
#define CINTERFACE
#define COBJMACROS
#define _WIN32_WINNT 0x0501
#include <windows.h>
#include <objbase.h>
#include <dplay.h>
#include <dplobby.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DEFINE_GUID(TA_APP,0x99797420,0xf5f5,0x11cf,0x98,0x27,0,0xa0,0x24,0x14,0x96,0xc8);
static IDirectPlay4A *dp;
static DPSESSIONDESC2 session;
static unsigned char data[65536];
#define COMMAND_MAX 131200
#define QUEUE_MAX 128
static CRITICAL_SECTION queue_lock;
static HANDLE queue_space;
static char *commands[QUEUE_MAX];
static unsigned queued, read_at, write_at;
static volatile LONG input_done;

static DWORD WINAPI read_commands(void *unused)
{
    (void)unused;
    for(;;){
        char *text=malloc(COMMAND_MAX);
        if(!text)break;
        if(!fgets(text,COMMAND_MAX,stdin)){free(text);break;}
        char *end=strchr(text,'\n');
        if(!end){free(text);break;}
        *end=0;
        if(end>text && end[-1]=='\r')end[-1]=0;
        WaitForSingleObject(queue_space,INFINITE);
        EnterCriticalSection(&queue_lock);
        commands[write_at]=text;write_at=(write_at+1)%QUEUE_MAX;queued++;
        LeaveCriticalSection(&queue_lock);
    }
    InterlockedExchange(&input_done,1);return 0;
}

static char *take_command(void)
{
    char *text=NULL;
    EnterCriticalSection(&queue_lock);
    if(queued){text=commands[read_at];read_at=(read_at+1)%QUEUE_MAX;queued--;}
    LeaveCriticalSection(&queue_lock);
    if(text)ReleaseSemaphore(queue_space,1,NULL);
    return text;
}

static int check(const char *op, HRESULT hr)
{
    if (SUCCEEDED(hr)) return 1;
    printf("ERROR %s %08lx\n",op,(unsigned long)hr);fflush(stdout);return 0;
}

static void *address(void)
{
    IDirectPlayLobby3A *lobby=NULL;
    DPCOMPOUNDADDRESSELEMENT el[2];
    DWORD size=0;
    if(!check("Lobby",CoCreateInstance(&CLSID_DirectPlayLobby,NULL,CLSCTX_INPROC_SERVER,
                &IID_IDirectPlayLobby3A,(void**)&lobby))) return NULL;
    el[0].guidDataType=DPAID_ServiceProvider;el[0].dwDataSize=sizeof(GUID);el[0].lpData=(void*)&DPSPGUID_TCPIP;
    el[1].guidDataType=DPAID_INet;el[1].dwDataSize=10;el[1].lpData=(void*)"127.0.0.1";
    HRESULT hr=IDirectPlayLobby_CreateCompoundAddress(lobby,el,2,NULL,&size);
    void *result=NULL;
    if(hr==DPERR_BUFFERTOOSMALL && size<=65536){
        result=malloc(size);
        if(result && !check("Address",IDirectPlayLobby_CreateCompoundAddress(lobby,el,2,result,&size))){free(result);result=NULL;}
    }
    IDirectPlayLobby_Release(lobby);return result;
}

static int hexval(char c)
{
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}

static int command(const char *line)
{
    unsigned long from,to,a,b,c,d;
    int offset=0;
    if(!strcmp(line,"STOP"))return 0;
    if(sscanf(line,"STATE %lu %lu %lu %lu",&a,&b,&c,&d)==4){
        session.dwUser1=a;session.dwUser2=b;session.dwUser3=c;session.dwUser4=d;
        if(!check("SetSessionDesc",IDirectPlayX_SetSessionDesc(dp,&session,0)))return -1;
    }else if(sscanf(line,"SEND %lu %lu %n",&from,&to,&offset)==2 && offset>0){
        size_t n=strlen(line+offset);
        if(n%2||n>sizeof(data)*2)return -1;
        for(size_t i=0;i<n;i+=2){
            int hi=hexval(line[offset+i]),lo=hexval(line[offset+i+1]);
            if(hi<0||lo<0)return -1;
            data[i/2]=(unsigned char)((hi<<4)|lo);
        }
        if(!check("Send",IDirectPlayX_Send(dp,from,to,DPSEND_GUARANTEED,data,(DWORD)(n/2))))return -1;
    }else{return -1;}
    return 1;
}

int main(void)
{
    setvbuf(stdout,NULL,_IONBF,0);
    if(!check("CoInitialize",CoInitialize(NULL)))return 1;
    if(!check("DirectPlay",CoCreateInstance(&CLSID_DirectPlay,NULL,CLSCTX_INPROC_SERVER,
              &IID_IDirectPlay4A,(void**)&dp)))return 1;
    void *addr=address();if(!addr)return 1;
    HRESULT hr=IDirectPlayX_InitializeConnection(dp,addr,0);free(addr);
    if(!check("InitializeConnection",hr))return 1;
    memset(&session,0,sizeof(session));session.dwSize=sizeof(session);
    session.guidApplication=TA_APP;session.dwMaxPlayers=100;
    session.lpszSessionNameA=(char*)"DEMO-PROTOTYPE";
    session.dwUser1=1753284736u;session.dwUser2=4;session.dwUser3=655370;session.dwUser4=16974324;
    if(!check("Open",IDirectPlayX_Open(dp,&session,DPOPEN_CREATE)))return 1;
    DPID ids[20];
    for(int i=0;i<20;i++){
        DPNAME name;memset(&name,0,sizeof(name));name.dwSize=sizeof(name);
        name.lpszShortNameA=i?(char*)"RECORDED2":(char*)"RECORDED1";
        name.lpszLongNameA=name.lpszShortNameA;
        unsigned char pd[21]={0};pd[19]='P';
        if(!check("CreatePlayer",IDirectPlayX_CreatePlayer(dp,&ids[i],&name,NULL,pd,sizeof(pd),0)))return 1;
    }
    for(int i=0;i<20;i++)for(int j=i+1;j<20;j++){
        if(ids[j]<ids[i]){DPID swap=ids[i];ids[i]=ids[j];ids[j]=swap;}
    }
    for(int i=2;i<20;i++){
        if(!check("DestroyUnusedPlayer",IDirectPlayX_DestroyPlayer(dp,ids[i])))return 1;
    }
    printf("READY %lu %lu\n",(unsigned long)ids[0],(unsigned long)ids[1]);
    InitializeCriticalSection(&queue_lock);
    queue_space=CreateSemaphoreA(NULL,QUEUE_MAX,QUEUE_MAX,NULL);
    if(!queue_space)return 1;
    HANDLE reader=CreateThread(NULL,0,read_commands,NULL,0,NULL);
    if(!reader)return 1;
    CloseHandle(reader);
    int running=1;
    while(running){
        for(unsigned i=0;i<128&&running;i++){
            LONG ended=InterlockedCompareExchange(&input_done,0,0);
            char *text=take_command();
            if(!text){if(ended)running=0;break;}
            int result=command(text);free(text);
            if(result<=0){if(result<0)puts("ERROR command 0");running=0;}
        }
        for(unsigned i=0;i<128&&running;i++){
            DPID from=0,to=0;DWORD n=sizeof(data);
            hr=IDirectPlayX_Receive(dp,&from,&to,DPRECEIVE_ALL,data,&n);
            if(hr==DPERR_NOMESSAGES)break;
            if(!check("Receive",hr)||n>sizeof(data)){running=0;break;}
            printf("RX %lu %lu ",(unsigned long)from,(unsigned long)to);
            for(DWORD j=0;j<n;j++)printf("%02x",data[j]);
            putchar('\n');
        }
        Sleep(1);
    }
    IDirectPlayX_Close(dp);IDirectPlayX_Release(dp);CoUninitialize();return 0;
}
