/*
 * PROJECT:     LiberNT DISM API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Enumerate and remove online AppX provisioning records
 * COPYRIGHT:   Copyright 2026 LiberNT contributors
 */

#include <windows.h>
#include <wchar.h>
#include <strsafe.h>

#pragma pack(push, 4)
typedef struct _DISM_APPX_PACKAGE
{
    PWSTR PackageName;
    PWSTR DisplayName;
    PWSTR PublisherId;
    UINT MajorVersion;
    UINT MinorVersion;
    UINT Build;
    UINT Revision;
    UINT Architecture;
    PWSTR ResourceId;
    PWSTR InstallLocation;
    PWSTR Regions;
} DISM_APPX_PACKAGE;
#pragma pack(pop)

typedef struct _PROVISIONED_NAME
{
    WCHAR FullName[128];
    WCHAR FamilyName[79];
    WCHAR ResourceId[128];
} PROVISIONED_NAME;

static const WCHAR ProvisioningKey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Appx\\AppxAllUserStore\\InboxApplications";

HRESULT DismValidateSession(UINT Id);

LONG WINAPI PackageFamilyNameFromFullName(PCWSTR FullName, UINT32 *Length, PWSTR FamilyName);

static LONG FillPackage(DISM_APPX_PACKAGE *Package, PROVISIONED_NAME *Name)
{
    static const struct
    {
        PCWSTR Name;
        UINT Value;
    } Architectures[] =
    {
        {L"x86", PROCESSOR_ARCHITECTURE_INTEL},
        {L"x64", PROCESSOR_ARCHITECTURE_AMD64},
        {L"arm", PROCESSOR_ARCHITECTURE_ARM},
        {L"arm64", PROCESSOR_ARCHITECTURE_ARM64},
        {L"neutral", PROCESSOR_ARCHITECTURE_NEUTRAL},
        {L"x86a64", PROCESSOR_ARCHITECTURE_IA32_ON_ARM64}
    };
    WCHAR Identity[ARRAYSIZE(Name->FullName)];
    WCHAR *Version, *Architecture, *Resource, *Publisher, *Separator;
    UINT32 FamilyLength = ARRAYSIZE(Name->FamilyName);
    UINT Index;
    LONG Error;

    Error = PackageFamilyNameFromFullName(Name->FullName, &FamilyLength, Name->FamilyName);
    if (Error != ERROR_SUCCESS)
        return Error;

    StringCchCopyW(Identity, ARRAYSIZE(Identity), Name->FullName);
    Version = wcschr(Identity, L'_') + 1;
    Architecture = wcschr(Version, L'_') + 1;
    Resource = wcschr(Architecture, L'_') + 1;
    Publisher = wcschr(Resource, L'_') + 1;
    Architecture[-1] = Resource[-1] = Publisher[-1] = 0;

    if (swscanf(Version, L"%u.%u.%u.%u", &Package->MajorVersion, &Package->MinorVersion,
                &Package->Build, &Package->Revision) != 4)
        return ERROR_INVALID_DATA;

    for (Index = 0; Index < ARRAYSIZE(Architectures); ++Index)
    {
        if (!lstrcmpiW(Architecture, Architectures[Index].Name))
            break;
    }
    if (Index == ARRAYSIZE(Architectures))
        return ERROR_INVALID_DATA;

    Package->Architecture = Architectures[Index].Value;
    Package->PackageName = Name->FullName;
    Package->DisplayName = Name->FamilyName;
    Separator = wcschr(Name->FamilyName, L'_');
    *Separator = 0;
    Package->PublisherId = Separator + 1;
    Package->ResourceId = Name->ResourceId;
    StringCchCopyW(Name->ResourceId, ARRAYSIZE(Name->ResourceId), Resource);
    return ERROR_SUCCESS;
}

HRESULT WINAPI DismDelete(void *Buffer)
{
    if (!Buffer)
        return S_OK;

    return HeapFree(GetProcessHeap(), 0, Buffer) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

HRESULT WINAPI _DismGetProvisionedAppxPackages(UINT Session, DISM_APPX_PACKAGE **Packages, UINT *Count)
{
    DISM_APPX_PACKAGE *Result = NULL;
    PROVISIONED_NAME *Names;
    DWORD Index, Capacity, Length;
    LONG Error;
    HKEY Key;
    HRESULT Status;

    if (!Packages || !Count)
        return E_INVALIDARG;
    *Packages = NULL;
    *Count = 0;

    Status = DismValidateSession(Session);
    if (FAILED(Status))
        return Status;

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProvisioningKey, 0,
                          KEY_READ | KEY_WOW64_64KEY, &Key);
    if (Error == ERROR_FILE_NOT_FOUND || Error == ERROR_PATH_NOT_FOUND)
        return S_OK;
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);

    Error = RegQueryInfoKeyW(Key, NULL, NULL, NULL, &Capacity, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (Error != ERROR_SUCCESS || !Capacity)
        goto Done;

    if (Capacity > MAXDWORD / (sizeof(*Result) + sizeof(*Names)))
    {
        Error = ERROR_OUTOFMEMORY;
        goto Done;
    }

    Result = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Capacity * (sizeof(*Result) + sizeof(*Names)));
    if (!Result)
    {
        Error = ERROR_OUTOFMEMORY;
        goto Done;
    }

    Names = (PROVISIONED_NAME *)(Result + Capacity);
    for (Index = 0; Index < Capacity; ++Index)
    {
        Length = ARRAYSIZE(Names[Index].FullName);
        Error = RegEnumKeyExW(Key, Index, Names[Index].FullName, &Length, NULL, NULL, NULL, NULL);
        if (Error != ERROR_SUCCESS)
            break;
        Error = FillPackage(&Result[Index], &Names[Index]);
        if (Error != ERROR_SUCCESS)
            break;
    }

    if (Error == ERROR_NO_MORE_ITEMS)
        Error = ERROR_SUCCESS;

    if (Error == ERROR_SUCCESS)
    {
        *Count = Index;
        *Packages = Result;
        Result = NULL;
    }

Done:
    RegCloseKey(Key);
    DismDelete(Result);
    return HRESULT_FROM_WIN32(Error);
}

HRESULT WINAPI _DismRemoveProvisionedAppxPackage(UINT Session, PCWSTR PackageName)
{
    WCHAR FamilyName[79];
    UINT32 Length = ARRAYSIZE(FamilyName);
    HKEY Key;
    LONG Error;
    HRESULT Status;

    Status = DismValidateSession(Session);
    if (FAILED(Status))
        return Status;

    if (!PackageName || wcschr(PackageName, L'\\'))
        return E_INVALIDARG;

    Error = PackageFamilyNameFromFullName(PackageName, &Length, FamilyName);
    if (Error != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Error);

    Error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ProvisioningKey, 0,
                          KEY_WRITE | KEY_WOW64_64KEY, &Key);
    if (Error == ERROR_SUCCESS)
    {
        Error = RegDeleteKeyW(Key, PackageName);
        RegCloseKey(Key);
    }
    return HRESULT_FROM_WIN32(Error);
}
