/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Test online AppX provisioning enumeration
 * COPYRIGHT:   Copyright 2026 LiberNT contributors
 */

#include <apitest.h>
#include <windows.h>

typedef HRESULT (WINAPI *InitializeFn)(INT, PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *OpenSessionFn)(PCWSTR, PCWSTR, PCWSTR, UINT *);
typedef HRESULT (WINAPI *SessionFn)(UINT);
typedef HRESULT (WINAPI *QueryFn)(UINT, void **, UINT *);
typedef HRESULT (WINAPI *DeleteFn)(void *);
typedef HRESULT (WINAPI *ShutdownFn)(void);

START_TEST(ProvisionedAppx)
{
    static const WCHAR OnlineImage[] = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";
    HMODULE Module;
    InitializeFn Initialize;
    OpenSessionFn OpenSession;
    SessionFn CloseSession;
    QueryFn Query;
    DeleteFn Delete;
    ShutdownFn Shutdown;
    UINT Session, Count = 0;
    void *Packages = NULL;
    HRESULT Status;

    Module = LoadLibraryExW(L"dismapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ok(Module != NULL, "Loading system dismapi.dll failed: %lu\n", GetLastError());
    if (!Module)
        return;

    Initialize = (InitializeFn)GetProcAddress(Module, "DismInitialize");
    OpenSession = (OpenSessionFn)GetProcAddress(Module, "DismOpenSession");
    CloseSession = (SessionFn)GetProcAddress(Module, "DismCloseSession");
    Query = (QueryFn)GetProcAddress(Module, "_DismGetProvisionedAppxPackages");
    Delete = (DeleteFn)GetProcAddress(Module, "DismDelete");
    Shutdown = (ShutdownFn)GetProcAddress(Module, "DismShutdown");
    ok(Initialize && OpenSession && CloseSession && Query && Delete && Shutdown,
       "A required DISM export is missing\n");
    if (!Initialize || !OpenSession || !CloseSession || !Query || !Delete || !Shutdown)
        goto Unload;

    Status = Initialize(0, NULL, NULL);
    if (Status == E_ACCESSDENIED || Status == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED))
    {
        skip("Initializing DISM requires an elevated process\n");
        goto Unload;
    }
    ok(Status == S_OK, "DismInitialize returned %#lx\n", Status);
    if (FAILED(Status))
        goto Unload;

    Status = OpenSession(OnlineImage, NULL, NULL, &Session);
    if (Status == E_ACCESSDENIED || Status == HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED))
    {
        skip("Opening the online image requires an elevated process\n");
    }
    else
    {
        ok(Status == S_OK, "DismOpenSession returned %#lx\n", Status);
        if (SUCCEEDED(Status))
        {
            Status = Query(Session, &Packages, &Count);
            ok(Status == S_OK, "Provisioned AppX query returned %#lx\n", Status);
            if (SUCCEEDED(Status))
            {
                ok(!Count || Packages, "A nonempty package list has no buffer\n");
                trace("Online image has %u provisioned AppX packages\n", Count);
                if (Packages)
                    ok(Delete(Packages) == S_OK, "DismDelete failed\n");
            }
            ok(CloseSession(Session) == S_OK, "DismCloseSession failed\n");
        }
    }
    ok(Shutdown() == S_OK, "DismShutdown failed\n");

Unload:
    FreeLibrary(Module);
}
