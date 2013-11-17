#include "_exports.h"
#include "memory.h"
#include "debugger.h"
#include "value.h"
#include "addrinfo.h"
#include "console.h"
#include "threading.h"

extern "C" DLL_EXPORT duint _dbg_memfindbaseaddr(duint addr, duint* size)
{
    return memfindbaseaddr(fdProcessInfo->hProcess, addr, size);
}

extern "C" DLL_EXPORT bool _dbg_memread(duint addr, unsigned char* dest, duint size, duint* read)
{
    dbgdisablebpx();
    bool res=memread(fdProcessInfo->hProcess, (void*)addr, dest, size, read);
    dbgenablebpx();
    return res;
}

extern "C" DLL_EXPORT bool _dbg_memmap(MEMMAP* memmap)
{
    memset(memmap, 0, sizeof(MEMMAP));

    MEMORY_BASIC_INFORMATION mbi;
    DWORD numBytes;
    uint MyAddress=0, newAddress=0;
    SymInitialize(fdProcessInfo->hProcess, 0, true);
    std::vector<MEMPAGE> pageVector;
    do
    {
        numBytes=VirtualQueryEx(fdProcessInfo->hProcess, (LPCVOID)MyAddress, &mbi, sizeof(mbi));
        if(mbi.State==MEM_COMMIT)
        {
            MEMPAGE curPage;
            *curPage.mod=0;
            modnamefromaddr(MyAddress, curPage.mod);
            memcpy(&curPage.mbi, &mbi, sizeof(mbi));
            pageVector.push_back(curPage);
            memmap->count++;
        }
        newAddress=(uint)mbi.BaseAddress+mbi.RegionSize;
        if(newAddress<=MyAddress)
            numBytes=0;
        else
            MyAddress=newAddress;
    }
    while(numBytes);

    //process vector
    int pagecount=memmap->count;
    memmap->page=(MEMPAGE*)BridgeAlloc(sizeof(MEMPAGE)*pagecount);
    memset(memmap->page, 0, sizeof(MEMPAGE)*pagecount);
    for(int i=0; i<pagecount; i++)
        memcpy(&memmap->page[i], &pageVector.at(i), sizeof(MEMPAGE));

    return true;
}

extern "C" DLL_EXPORT bool _dbg_memisvalidreadptr(duint addr)
{
    return memisvalidreadptr(fdProcessInfo->hProcess, addr);
}

extern "C" DLL_EXPORT bool _dbg_valfromstring(const char* string, duint* value)
{
    return valfromstring(string, value, 0, 0, true, 0);
}

extern "C" DLL_EXPORT bool _dbg_isdebugging()
{
    return IsFileBeingDebugged();
}

extern "C" DLL_EXPORT bool _dbg_isjumpgoingtoexecute(duint addr)
{
    static unsigned int cacheFlags;
    static uint cacheAddr;
    static bool cacheResult;
    if(cacheAddr!=addr or cacheFlags!=GetContextData(UE_EFLAGS))
    {
        cacheFlags=GetContextData(UE_EFLAGS);
        cacheAddr=addr;
        cacheResult=IsJumpGoingToExecuteEx(fdProcessInfo->hProcess, fdProcessInfo->hThread, (ULONG_PTR)cacheAddr, cacheFlags);
    }
    return cacheResult;
}

extern "C" DLL_EXPORT bool _dbg_addrinfoget(duint addr, SEGMENTREG segment, ADDRINFO* addrinfo)
{
    bool retval=false;
    if(addrinfo->flags&flagmodule) //get module
    {
        char module[64]="";
        if(modnamefromaddr(addr, module) and strlen(module)<32) //get module name
        {
            strcpy(addrinfo->module, module);
            retval=true;
        }
    }
    if(addrinfo->flags&flaglabel)
    {
        if(labelget(addr, addrinfo->label))
            retval=true;
        else
        {
            //TODO: auto-labels
            /*const char* apiname=(const char*)ImporterGetAPINameFromDebugee(fdProcessInfo->hProcess, addr);
            if(apiname)
            {
                strcpy(addrinfo->label, apiname);
                retval=true;
            }
            uint addr_dw=0;
            if(memread(fdProcessInfo->hProcess, (const void*)addr, &addr_dw, sizeof(uint), 0))
            {
                const char* apiname=(const char*)ImporterGetAPINameFromDebugee(fdProcessInfo->hProcess, addr_dw);
                if(apiname)
                {
                    strcpy(addrinfo->label, apiname);
                    retval=true;
                }
            }*/
            if(!retval)
            {
                DWORD64 displacement=0;
                char buffer[sizeof(SYMBOL_INFO) + MAX_LABEL_SIZE * sizeof(char)];
                PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
                pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                pSymbol->MaxNameLen = MAX_LABEL_SIZE;
                if(SymFromAddr(fdProcessInfo->hProcess, (DWORD64)addr, &displacement, pSymbol) and !displacement)
                {
                    strcpy(addrinfo->label, pSymbol->Name);
                    retval=true;
                }
            }
        }
    }
    if(addrinfo->flags&flagcomment)
    {
        if(commentget(addr, addrinfo->comment))
            retval=true;
        //TODO: auto-comments
        else
        {
            if(!retval)
            {
                DWORD dwDisplacement;
                IMAGEHLP_LINE64 line;
                line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
                if(SymGetLineFromAddr64(fdProcessInfo->hProcess, (DWORD64)addr, &dwDisplacement, &line) and !dwDisplacement)
                {
                    sprintf(addrinfo->comment, "line: %u", line.LineNumber);
                    retval=true;
                }
            }
        }
    }
    return retval;
}

extern "C" DLL_EXPORT bool _dbg_addrinfoset(duint addr, ADDRINFO* addrinfo)
{
    bool retval=false;
    if(addrinfo->flags&flaglabel) //set label
    {
        if(labelset(addr, addrinfo->label))
            retval=true;
    }
    else if(addrinfo->flags&flagcomment) //set comment
    {
        if(commentset(addr, addrinfo->comment))
            retval=true;
    }
    return retval;
}

extern "C" DLL_EXPORT int _dbg_bpgettypeat(duint addr)
{
    int result=bpnone;
    BREAKPOINT* found=bpfind(bplist, 0, addr, 0, BPNORMAL);
    if(found and found->enabled) //none found or disabled
        result|=bpnormal;
    found=bpfind(bplist, 0, addr, 0, BPHARDWARE);
    if(found and found->enabled) //none found or disabled
        result|=bphardware;
    found=bpfind(bplist, 0, addr, 0, BPMEMORY);
    if(found and found->enabled) //none found or disabled
        result|=bpmemory;
    return result;
}

extern "C" DLL_EXPORT bool _dbg_getregdump(REGDUMP* regdump)
{
    if(!IsFileBeingDebugged())
    {
        memset(regdump, 0, sizeof(REGDUMP));
        return true;
    }
    REGDUMP r;
#ifdef _WIN64
    r.cax=GetContextData(UE_RAX);
#else
    r.cax=(duint)GetContextData(UE_EAX);
#endif // _WIN64
#ifdef _WIN64
    r.ccx=GetContextData(UE_RCX);
#else
    r.ccx=(duint)GetContextData(UE_ECX);
#endif // _WIN64
#ifdef _WIN64
    r.cdx=GetContextData(UE_RDX);
#else
    r.cdx=(duint)GetContextData(UE_EDX);
#endif // _WIN64
#ifdef _WIN64
    r.cbx=GetContextData(UE_RBX);
#else
    r.cbx=(duint)GetContextData(UE_EBX);
#endif // _WIN64
#ifdef _WIN64
    r.cbp=GetContextData(UE_RBP);
#else
    r.cbp=(duint)GetContextData(UE_EBP);
#endif // _WIN64
#ifdef _WIN64
    r.csi=GetContextData(UE_RSI);
#else
    r.csi=(duint)GetContextData(UE_ESI);
#endif // _WIN64
#ifdef _WIN64
    r.cdi=GetContextData(UE_RDI);
#else
    r.cdi=(duint)GetContextData(UE_EDI);
#endif // _WIN64
#ifdef _WIN64
    r.r8=GetContextData(UE_R8);
#endif // _WIN64
#ifdef _WIN64
    r.r9=GetContextData(UE_R9);
#endif // _WIN64
#ifdef _WIN64
    r.r10=GetContextData(UE_R10);
#endif // _WIN64
#ifdef _WIN64
    r.r11=GetContextData(UE_R11);
#endif // _WIN64
#ifdef _WIN64
    r.r12=GetContextData(UE_R12);
#endif // _WIN64
#ifdef _WIN64
    r.r13=GetContextData(UE_R13);
#endif // _WIN64
#ifdef _WIN64
    r.r14=GetContextData(UE_R14);
#endif // _WIN64
#ifdef _WIN64
    r.r15=GetContextData(UE_R15);
#endif // _WIN64
    r.csp=(duint)GetContextData(UE_CSP);
    r.cip=(duint)GetContextData(UE_CIP);
    r.eflags=(duint)GetContextData(UE_EFLAGS);
    r.gs=(unsigned short)(GetContextData(UE_SEG_GS)&0xFFFF);
    r.fs=(unsigned short)(GetContextData(UE_SEG_FS)&0xFFFF);
    r.es=(unsigned short)(GetContextData(UE_SEG_ES)&0xFFFF);
    r.ds=(unsigned short)(GetContextData(UE_SEG_DS)&0xFFFF);
    r.cs=(unsigned short)(GetContextData(UE_SEG_CS)&0xFFFF);
    r.ss=(unsigned short)(GetContextData(UE_SEG_SS)&0xFFFF);
    r.dr0=(duint)GetContextData(UE_DR0);
    r.dr1=(duint)GetContextData(UE_DR1);
    r.dr2=(duint)GetContextData(UE_DR2);
    r.dr3=(duint)GetContextData(UE_DR3);
    r.dr6=(duint)GetContextData(UE_DR6);
    r.dr7=(duint)GetContextData(UE_DR7);
    duint cflags=r.eflags;
    r.flags.c=valflagfromstring(cflags, "cf");
    r.flags.p=valflagfromstring(cflags, "pf");
    r.flags.a=valflagfromstring(cflags, "af");
    r.flags.z=valflagfromstring(cflags, "zf");
    r.flags.s=valflagfromstring(cflags, "sf");
    r.flags.t=valflagfromstring(cflags, "tf");
    r.flags.i=valflagfromstring(cflags, "if");
    r.flags.d=valflagfromstring(cflags, "df");
    r.flags.o=valflagfromstring(cflags, "of");
    memcpy(regdump, &r, sizeof(REGDUMP));
    return true;
}

extern "C" DLL_EXPORT bool _dbg_valtostring(const char* string, duint* value)
{
    return valtostring(string, value, true);
}