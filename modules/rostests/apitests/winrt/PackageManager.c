/*
 * PROJECT:     LiberNT API tests
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Verify PackageManager activation through Windows Runtime
 */

#include "winrt_apitest.h"
#define WIDL_using_Windows_ApplicationModel
#define WIDL_using_Windows_Management_Deployment
#include <windows.management.deployment.h>

START_TEST(PackageManager)
{
    HSTRING Class;
    IInspectable *Instance = NULL;
    IPackageManager *Manager = NULL;
    IPackageManager2 *Manager2 = NULL;
    IUnknown *Identity = NULL, *Identity2 = NULL;
    IIterable_Package *Packages = NULL;
    HRESULT hr;

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    ok(SUCCEEDED(hr), "RoInitialize returned %#lx\n", hr);
    if (FAILED(hr))
        return;

    Class = CreateHString(L"Windows.Management.Deployment.PackageManager");
    hr = RoActivateInstance(Class, &Instance);
    ok(hr == S_OK, "RoActivateInstance returned %#lx\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IInspectable_QueryInterface(Instance, &IID_IPackageManager, (void **)&Manager);
        ok(hr == S_OK, "QueryInterface(IPackageManager) returned %#lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IPackageManager_QueryInterface(Manager, &IID_IUnknown, (void **)&Identity);
            ok(hr == S_OK, "IPackageManager identity query returned %#lx\n", hr);
            IPackageManager_Release(Manager);
        }

        hr = IInspectable_QueryInterface(Instance, &IID_IPackageManager2, (void **)&Manager2);
        ok(hr == S_OK, "QueryInterface(IPackageManager2) returned %#lx\n", hr);
        if (SUCCEEDED(hr))
        {
            hr = IPackageManager2_QueryInterface(Manager2, &IID_IUnknown, (void **)&Identity2);
            ok(hr == S_OK, "IPackageManager2 identity query returned %#lx\n", hr);
            if (Identity2)
            {
                ok(Identity == Identity2, "PackageManager interfaces have different COM identities\n");
                IUnknown_Release(Identity2);
            }
            hr = IPackageManager2_FindPackagesWithPackageTypes(Manager2, PackageTypes_Main, &Packages);
            if (hr == E_ACCESSDENIED)
                skip("Enumerating packages for all users requires elevation\n");
            else
                ok(hr == S_OK && Packages, "Package enumeration returned %#lx and %p\n", hr, Packages);
            if (Packages)
                IIterable_Package_Release(Packages);
            IPackageManager2_Release(Manager2);
        }
        if (Identity)
            IUnknown_Release(Identity);
        IInspectable_Release(Instance);
    }

    WindowsDeleteString(Class);
    RoUninitialize();
}
