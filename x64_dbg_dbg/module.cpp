#include "module.h"
#include "debugger.h"
#include "threading.h"
#include "symbolinfo.h"
#include "murmurhash.h"
#include "memory.h"
#include "label.h"

std::map<Range, MODINFO, RangeCompare> modinfo;

void GetModuleInfo(MODINFO & Info, ULONG_PTR FileMapVA)
{
    // Get the entry point
    uint moduleOEP = GetPE32DataFromMappedFile(FileMapVA, 0, UE_OEP);

    // Fix a problem where the OEP is set to zero (non-existent).
    // OEP can't start at the PE header/offset 0 -- except if module is an EXE.
    Info.entry = moduleOEP + Info.base;

    if(!moduleOEP)
    {
        WORD characteristics = (WORD)GetPE32DataFromMappedFile(FileMapVA, 0, UE_CHARACTERISTICS);

        // If this wasn't an exe, invalidate the entry point
        if((characteristics & IMAGE_FILE_DLL) == IMAGE_FILE_DLL)
            Info.entry = 0;
    }

    // Enumerate all PE sections
    Info.sections.clear();
    int sectionCount = (int)GetPE32DataFromMappedFile(FileMapVA, 0, UE_SECTIONNUMBER);

    for(int i = 0; i < sectionCount; i++)
    {
        MODSECTIONINFO curSection;
        memset(&curSection, 0, sizeof(MODSECTIONINFO));

        curSection.addr = GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONVIRTUALOFFSET) + Info.base;
        curSection.size = GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONVIRTUALSIZE);
        const char* sectionName = (const char*)GetPE32DataFromMappedFile(FileMapVA, i, UE_SECTIONNAME);

        // Escape section name when needed
        strcpy_s(curSection.name, StringUtils::Escape(sectionName).c_str());

        // Add entry to the vector
        Info.sections.push_back(curSection);
    }
}

bool ModLoad(uint Base, uint Size, const char* FullPath)
{
    // Handle a new module being loaded
    if(!Base || !Size || !FullPath)
        return false;

    // Copy the module path in the struct
    MODINFO info;
    strcpy_s(info.path, FullPath);

    // Break the module path into a directory and file name
    char file[MAX_MODULE_SIZE];
    {
        char dir[MAX_PATH];
        memset(dir, 0, sizeof(dir));

        // Dir <- lowercase(file path)
        strcpy_s(dir, FullPath);
        _strlwr(dir);

        // Find the last instance of a path delimiter (slash)
        char* fileStart = strrchr(dir, '\\');

        if(fileStart)
        {
            strcpy_s(file, fileStart + 1);
            fileStart[0] = '\0';
        }
        else
            strcpy_s(file, FullPath);
    }

    // Calculate module hash from full file name
    info.hash = ModHashFromName(file);

    // Copy the extension into the module struct
    {
        char* extensionPos = strrchr(file, '.');

        if(extensionPos)
        {
            strcpy_s(info.extension, extensionPos);
            extensionPos[0] = '\0';
        }
    }

    // Copy information to struct
    strcpy_s(info.name, file);
    info.base = Base;
    info.size = Size;

    // Load module data
    bool virtualModule = strstr(FullPath, "virtual:\\") == FullPath;

    if(!virtualModule)
    {
        HANDLE fileHandle;
        DWORD loadedSize;
        HANDLE fileMap;
        ULONG_PTR fileMapVA;
        WString wszFullPath = StringUtils::Utf8ToUtf16(FullPath);

        // Load the physical module from disk
        if(StaticFileLoadW(wszFullPath.c_str(), UE_ACCESS_READ, false, &fileHandle, &loadedSize, &fileMap, &fileMapVA))
        {
            GetModuleInfo(info, fileMapVA);
            StaticFileUnloadW(wszFullPath.c_str(), false, fileHandle, loadedSize, fileMap, fileMapVA);
        }
    }
    else
    {
        // This was a virtual module -> read it remotely
        Memory<unsigned char*> data(Size);
        MemRead(Base, data(), data.size());

        // Get information from the local buffer
        GetModuleInfo(info, (ULONG_PTR)data());
    }

    // Add module to list
    EXCLUSIVE_ACQUIRE(LockModules);
    modinfo.insert(std::make_pair(Range(Base, Base + Size - 1), info));
    EXCLUSIVE_RELEASE();

    // Put labels for virtual module exports
    if(virtualModule)
    {
        if(info.entry >= Base && info.entry < Base + Size)
            LabelSet(info.entry, "EntryPoint", false);

        apienumexports(Base, [](uint base, const char* mod, const char* name, uint addr)
        {
            LabelSet(addr, name, false);
        });
    }

    SymUpdateModuleList();
    return true;
}

bool ModUnload(uint Base)
{
    EXCLUSIVE_ACQUIRE(LockModules);

    // Find the iterator index
    const auto found = modinfo.find(Range(Base, Base));

    if(found == modinfo.end())
        return false;

    // Remove it from the list
    modinfo.erase(found);
    EXCLUSIVE_RELEASE();

    // Update symbols
    SymUpdateModuleList();
    return true;
}

void ModClear()
{
    // Clean up all the modules
    EXCLUSIVE_ACQUIRE(LockModules);
    modinfo.clear();
    EXCLUSIVE_RELEASE();

    // Tell the symbol updater
    GuiSymbolUpdateModuleList(0, nullptr);
}

MODINFO* ModInfoFromAddr(uint Address)
{
    //
    // NOTE: THIS DOES _NOT_ USE LOCKS
    //
    auto found = modinfo.find(Range(Address, Address));

    // Was the module found with this address?
    if(found == modinfo.end())
        return nullptr;

    return &found->second;
}

bool ModNameFromAddr(uint Address, char* Name, bool Extension)
{
    if(!Name)
        return false;

    SHARED_ACQUIRE(LockModules);

    // Get a pointer to module information
    auto module = ModInfoFromAddr(Address);

    if(!module)
        return false;

    // Copy initial module name
    strcpy_s(Name, MAX_MODULE_SIZE, module->name);

    if(Extension)
        strcat_s(Name, MAX_MODULE_SIZE, module->extension);

    return true;
}

uint ModBaseFromAddr(uint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->base;
}

uint ModHashFromAddr(uint Address)
{
    // Returns a unique hash from a virtual address
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return Address;

    return module->hash + (Address - module->base);
}

uint ModHashFromName(const char* Module)
{
    // return MODINFO.hash (based on the name)
    if(!Module || Module[0] == '\0')
        return 0;

    return murmurhash(Module, (int)strlen(Module));
}

uint ModBaseFromName(const char* Module)
{
    if(!Module || strlen(Module) >= MAX_MODULE_SIZE)
        return 0;

    SHARED_ACQUIRE(LockModules);

    for(const auto & i : modinfo)
    {
        const auto & currentModule = i.second;
        char currentModuleName[MAX_MODULE_SIZE];
        strcpy_s(currentModuleName, currentModule.name);
        strcat_s(currentModuleName, currentModule.extension);

        // Test with and without extension
        if(!_stricmp(currentModuleName, Module) || !_stricmp(currentModule.name, Module))
            return currentModule.base;
    }

    return 0;
}

uint ModSizeFromAddr(uint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->size;
}

bool ModSectionsFromAddr(uint Address, std::vector<MODSECTIONINFO>* Sections)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return false;

    // Copy vector <-> vector
    *Sections = module->sections;
    return true;
}

uint ModEntryFromAddr(uint Address)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    return module->entry;
}

int ModPathFromAddr(duint Address, char* Path, int Size)
{
    SHARED_ACQUIRE(LockModules);

    auto module = ModInfoFromAddr(Address);

    if(!module)
        return 0;

    strcpy_s(Path, Size, module->path);
    return (int)strlen(Path);
}

int ModPathFromName(const char* Module, char* Path, int Size)
{
    return ModPathFromAddr(ModBaseFromName(Module), Path, Size);
}

void ModGetList(std::vector<MODINFO> & list)
{
    SHARED_ACQUIRE(LockModules);
    list.clear();
    for(const auto & mod : modinfo)
        list.push_back(mod.second);
}