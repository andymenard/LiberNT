/*
 * PROJECT:     LiberNT DISM API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Manage DISM initialization and online image sessions
 * COPYRIGHT:   Copyright 2026 LiberNT contributors
 */

#include <windows.h>

#define DISMAPI_E_DISMAPI_NOT_INITIALIZED ((HRESULT)0xc0040001)
#define DISMAPI_E_OPEN_SESSION_HANDLES ((HRESULT)0xc0040003)
#define DISMAPI_E_INVALID_DISM_SESSION ((HRESULT)0xc0040004)

typedef struct _DISM_SESSION
{
    struct _DISM_SESSION *Next;
    UINT Id;
} DISM_SESSION;

static const WCHAR OnlineImage[] = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";
static SRWLOCK SessionLock = SRWLOCK_INIT;
static DISM_SESSION *Sessions;
static UINT NextId;
static BOOL Initialized;

static HRESULT CheckElevation(void)
{
    HANDLE Token;
    TOKEN_ELEVATION Elevation;
    DWORD Length;
    HRESULT Status;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &Token))
        return HRESULT_FROM_WIN32(GetLastError());

    if (!GetTokenInformation(Token, TokenElevation, &Elevation, sizeof(Elevation), &Length))
        Status = HRESULT_FROM_WIN32(GetLastError());
    else
        Status = Elevation.TokenIsElevated ? S_OK : HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
    CloseHandle(Token);
    return Status;
}

HRESULT WINAPI DismInitialize(INT Level, PCWSTR LogPath, PCWSTR ScratchDirectory)
{
    HRESULT Status = S_OK;
    HANDLE Log;

    if (Level < 0 || Level > 2)
        return E_INVALIDARG;

    Status = CheckElevation();
    if (FAILED(Status))
        return Status;

    AcquireSRWLockExclusive(&SessionLock);
    if (Initialized)
    {
        Status = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        goto Done;
    }

    if (ScratchDirectory)
    {
        DWORD Attributes = GetFileAttributesW(ScratchDirectory);
        if (Attributes == INVALID_FILE_ATTRIBUTES)
        {
            Status = HRESULT_FROM_WIN32(GetLastError());
            goto Done;
        }
        if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            Status = HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            goto Done;
        }
    }

    if (LogPath)
    {
        Log = CreateFileW(LogPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (Log == INVALID_HANDLE_VALUE)
        {
            Status = HRESULT_FROM_WIN32(GetLastError());
            goto Done;
        }
        CloseHandle(Log);
    }

    Initialized = TRUE;

Done:
    ReleaseSRWLockExclusive(&SessionLock);
    return Status;
}

HRESULT WINAPI DismShutdown(void)
{
    HRESULT Status = S_OK;

    AcquireSRWLockExclusive(&SessionLock);
    if (!Initialized)
        Status = DISMAPI_E_DISMAPI_NOT_INITIALIZED;
    else if (Sessions)
        Status = DISMAPI_E_OPEN_SESSION_HANDLES;
    else
        Initialized = FALSE;
    ReleaseSRWLockExclusive(&SessionLock);
    return Status;
}

HRESULT WINAPI DismOpenSession(PCWSTR Image, PCWSTR WindowsDirectory,
                               PCWSTR SystemDrive, UINT *Id)
{
    HRESULT Status;
    DISM_SESSION *Session;

    if (!Image || !Id)
        return E_INVALIDARG;

    AcquireSRWLockExclusive(&SessionLock);
    if (!Initialized)
        Status = DISMAPI_E_DISMAPI_NOT_INITIALIZED;
    else if (lstrcmpW(Image, OnlineImage) || WindowsDirectory || SystemDrive)
        Status = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    else if (NextId == MAXUINT)
        Status = HRESULT_FROM_WIN32(ERROR_NO_MORE_ITEMS);
    else if (!(Session = HeapAlloc(GetProcessHeap(), 0, sizeof(*Session))))
        Status = E_OUTOFMEMORY;
    else
    {
        Session->Id = ++NextId;
        Session->Next = Sessions;
        Sessions = Session;
        *Id = Session->Id;
        Status = S_OK;
    }
    ReleaseSRWLockExclusive(&SessionLock);
    return Status;
}

HRESULT WINAPI DismCloseSession(UINT Id)
{
    HRESULT Status = DISMAPI_E_INVALID_DISM_SESSION;
    DISM_SESSION **Link, *Session;

    AcquireSRWLockExclusive(&SessionLock);
    if (!Initialized)
        Status = DISMAPI_E_DISMAPI_NOT_INITIALIZED;
    else
    {
        for (Link = &Sessions; (Session = *Link); Link = &Session->Next)
        {
            if (Session->Id != Id)
                continue;
            *Link = Session->Next;
            HeapFree(GetProcessHeap(), 0, Session);
            Status = S_OK;
            break;
        }
    }
    ReleaseSRWLockExclusive(&SessionLock);
    return Status;
}

HRESULT DismValidateSession(UINT Id)
{
    HRESULT Status = DISMAPI_E_INVALID_DISM_SESSION;
    const DISM_SESSION *Session;

    AcquireSRWLockShared(&SessionLock);
    if (!Initialized)
        Status = DISMAPI_E_DISMAPI_NOT_INITIALIZED;
    else
    {
        for (Session = Sessions; Session; Session = Session->Next)
        {
            if (Session->Id == Id)
            {
                Status = S_OK;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&SessionLock);
    return Status;
}
