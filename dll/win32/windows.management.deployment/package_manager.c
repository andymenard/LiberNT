/*
 * PROJECT:     LiberNT Windows Management Deployment
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     WinRT package manager activation and package queries
 */

#define COBJMACROS
#define CONST_VTABLE
#include <windows.h>
#include <appmodel.h>
#include <initguid.h>
#include <activation.h>
#include <winstring.h>

#define WIDL_using_Windows_Foundation_Collections
#define WIDL_using_Windows_ApplicationModel
#include <windows.applicationmodel.h>
#define WIDL_using_Windows_Management_Deployment
#include <windows.management.deployment.h>

static const WCHAR PackageManagerName[] = L"Windows.Management.Deployment.PackageManager";


typedef __FIAsyncOperationWithProgress_2_Windows__CManagement__CDeployment__CDeploymentResult_DeploymentProgress DeploymentOperation;
typedef __x_ABI_CWindows_CFoundation_CIUriRuntimeClass DeploymentUri;
typedef __FIIterable_1_Windows__CFoundation__CUri DeploymentUris;
typedef __FIIterable_1_Windows__CManagement__CDeployment__CPackageUserInformation PackageUsers;

#define DEFINE_INSPECTABLE(Prefix, Interface, InterfaceId, RuntimeName)                       \
    static LONG Prefix##Refs = 1;                                                            \
    static ULONG WINAPI Prefix##AddRef(Interface *This)                                      \
    {                                                                                       \
        return InterlockedIncrement(&Prefix##Refs);                                          \
    }                                                                                       \
    static ULONG WINAPI Prefix##Release(Interface *This)                                     \
    {                                                                                       \
        return InterlockedDecrement(&Prefix##Refs);                                          \
    }                                                                                       \
    static HRESULT WINAPI Prefix##QueryInterface(Interface *This, REFIID Id, void **Object)  \
    {                                                                                       \
        if (!Object) return E_POINTER;                                                       \
        *Object = NULL;                                                                      \
        if (!IsEqualIID(Id, &InterfaceId) && !IsEqualIID(Id, &IID_IUnknown) &&                 \
            !IsEqualIID(Id, &IID_IInspectable) && !IsEqualIID(Id, &IID_IAgileObject))           \
            return E_NOINTERFACE;                                                            \
        *Object = This;                                                                      \
        Prefix##AddRef(This);                                                                \
        return S_OK;                                                                        \
    }                                                                                       \
    static HRESULT WINAPI Prefix##GetIids(Interface *This, ULONG *Count, IID **Ids)            \
    {                                                                                       \
        if (!Count || !Ids) return E_POINTER;                                                \
        *Count = 0;                                                                         \
        *Ids = CoTaskMemAlloc(sizeof(**Ids));                                                 \
        if (!*Ids) return E_OUTOFMEMORY;                                                     \
        **Ids = InterfaceId;                                                                \
        *Count = 1;                                                                         \
        return S_OK;                                                                        \
    }                                                                                       \
    static HRESULT WINAPI Prefix##GetRuntimeClassName(Interface *This, HSTRING *Name)         \
    {                                                                                       \
        return WindowsCreateString(RuntimeName, ARRAYSIZE(RuntimeName) - 1, Name);            \
    }                                                                                       \
    static HRESULT WINAPI Prefix##GetTrustLevel(Interface *This, TrustLevel *Level)            \
    {                                                                                       \
        if (!Level) return E_POINTER;                                                        \
        *Level = BaseTrust;                                                                 \
        return S_OK;                                                                        \
    }

#define INSPECTABLE_VTBL(Prefix) Prefix##QueryInterface, Prefix##AddRef, Prefix##Release, \
    Prefix##GetIids, Prefix##GetRuntimeClassName, Prefix##GetTrustLevel

DEFINE_INSPECTABLE(Factory, IActivationFactory, IID_IActivationFactory, PackageManagerName)
static IPackageManager Manager;
static IPackageManager2 Manager2;
static LONG ManagerRefs = 1;

static ULONG WINAPI ManagerAddRef(IPackageManager *This)
{
    return InterlockedIncrement(&ManagerRefs);
}

static ULONG WINAPI ManagerRelease(IPackageManager *This)
{
    return InterlockedDecrement(&ManagerRefs);
}

static HRESULT WINAPI ManagerQueryInterface(IPackageManager *This, REFIID Id, void **Object)
{
    if (!Object) return E_POINTER;
    *Object = NULL;
    if (IsEqualIID(Id, &IID_IPackageManager2))
        *Object = &Manager2;
    else if (IsEqualIID(Id, &IID_IPackageManager) || IsEqualIID(Id, &IID_IUnknown) ||
             IsEqualIID(Id, &IID_IInspectable) || IsEqualIID(Id, &IID_IAgileObject))
        *Object = &Manager;
    else
        return E_NOINTERFACE;
    ManagerAddRef(This);
    return S_OK;
}

static HRESULT WINAPI ManagerGetIids(IPackageManager *This, ULONG *Count, IID **Ids)
{
    if (!Count || !Ids) return E_POINTER;
    *Count = 0;
    *Ids = CoTaskMemAlloc(2 * sizeof(**Ids));
    if (!*Ids) return E_OUTOFMEMORY;
    (*Ids)[0] = IID_IPackageManager;
    (*Ids)[1] = IID_IPackageManager2;
    *Count = 2;
    return S_OK;
}

static HRESULT WINAPI ManagerGetRuntimeClassName(IPackageManager *This, HSTRING *Name)
{
    return WindowsCreateString(PackageManagerName, ARRAYSIZE(PackageManagerName) - 1, Name);
}

static HRESULT WINAPI ManagerGetTrustLevel(IPackageManager *This, TrustLevel *Level)
{
    if (!Level) return E_POINTER;
    *Level = BaseTrust;
    return S_OK;
}
DEFINE_INSPECTABLE(Iterable, IIterable_Package, IID_IIterable_Package, L"")
DEFINE_INSPECTABLE(Iterator, IIterator_Package, IID_IIterator_Package, L"")

static HRESULT WINAPI IteratorCurrent(IIterator_Package *This, IPackage **Package)
{
    if (!Package) return E_POINTER;
    *Package = NULL;
    return E_BOUNDS;
}

static HRESULT WINAPI IteratorHasCurrent(IIterator_Package *This, boolean *HasCurrent)
{
    if (!HasCurrent) return E_POINTER;
    *HasCurrent = FALSE;
    return S_OK;
}

static HRESULT WINAPI IteratorGetMany(IIterator_Package *This, UINT32 Capacity,
                                      IPackage **Packages, UINT32 *Actual)
{
    if (!Actual || (Capacity && !Packages)) return E_POINTER;
    *Actual = 0;
    return S_OK;
}

static const IIterator_PackageVtbl EmptyIteratorVtbl =
{
    INSPECTABLE_VTBL(Iterator),
    IteratorCurrent, IteratorHasCurrent, IteratorHasCurrent, IteratorGetMany,
};
static IIterator_Package EmptyIterator = {&EmptyIteratorVtbl};

static HRESULT WINAPI IterableFirst(IIterable_Package *This, IIterator_Package **Iterator)
{
    return IteratorQueryInterface(&EmptyIterator, &IID_IIterator_Package, (void **)Iterator);
}

static const IIterable_PackageVtbl EmptyIterableVtbl =
{
    INSPECTABLE_VTBL(Iterable),
    IterableFirst,
};
static IIterable_Package EmptyIterable = {&EmptyIterableVtbl};

static HRESULT WINAPI FindPackages(IPackageManager *This, IIterable_Package **Packages)
{
    return IterableQueryInterface(&EmptyIterable, &IID_IIterable_Package, (void **)Packages);
}

static HRESULT WINAPI FindPackagesByUser(IPackageManager *This, HSTRING User, IIterable_Package **Packages)
{
    return FindPackages(This, Packages);
}

static HRESULT WINAPI FindPackagesByName(IPackageManager *This, HSTRING Name, HSTRING Publisher,
                                        IIterable_Package **Packages)
{
    if (!Packages) return E_POINTER;
    *Packages = NULL;
    if (WindowsIsStringEmpty(Name) || WindowsIsStringEmpty(Publisher)) return E_INVALIDARG;
    return FindPackages(This, Packages);
}

static HRESULT WINAPI FindPackagesByUserAndName(IPackageManager *This, HSTRING User, HSTRING Name,
                                               HSTRING Publisher, IIterable_Package **Packages)
{
    return FindPackagesByName(This, Name, Publisher, Packages);
}

static HRESULT WINAPI FindPackagesByFamily(IPackageManager *This, HSTRING Family,
                                          IIterable_Package **Packages)
{
    UINT32 Count = 0, Length = 0;
    LONG Status;

    if (!Packages) return E_POINTER;
    *Packages = NULL;
    Status = FindPackagesByPackageFamily(WindowsGetStringRawBuffer(Family, NULL),
                                        PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT,
                                        &Count, NULL, &Length, NULL, NULL);
    if (Status != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(Status);
    return FindPackages(This, Packages);
}

static HRESULT WINAPI FindPackagesByUserAndFamily(IPackageManager *This, HSTRING User,
                                                 HSTRING Family, IIterable_Package **Packages)
{
    return FindPackagesByFamily(This, Family, Packages);
}

static HRESULT WINAPI FindPackage(IPackageManager *This, HSTRING FullName, IPackage **Package)
{
    if (!Package) return E_POINTER;
    *Package = NULL;
    if (WindowsIsStringEmpty(FullName)) return E_INVALIDARG;
    return S_OK;
}

static HRESULT WINAPI FindPackageByUser(IPackageManager *This, HSTRING User, HSTRING FullName,
                                       IPackage **Package)
{
    return FindPackage(This, FullName, Package);
}

static HRESULT UnimplementedDeployment(DeploymentOperation **Operation)
{
    if (!Operation) return E_POINTER;
    *Operation = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI ChangePackageWithOptions(IPackageManager *This, DeploymentUri *Source,
                                               DeploymentUris *Dependencies, DeploymentOptions Options,
                                               DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI StagePackage(IPackageManager *This, DeploymentUri *Source,
                                   DeploymentUris *Dependencies, DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI RemovePackage(IPackageManager *This, HSTRING FullName, DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI FindUsers(IPackageManager *This, HSTRING FullName, PackageUsers **Users)
{
    if (!Users) return E_POINTER;
    *Users = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI CleanupPackage(IPackageManager *This, HSTRING FullName, HSTRING User,
                                     DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI SetPackageState(IPackageManager *This, HSTRING FullName, PackageState State)
{
    return E_NOTIMPL;
}

static const IPackageManagerVtbl ManagerVtbl =
{
    INSPECTABLE_VTBL(Manager),
    ChangePackageWithOptions, ChangePackageWithOptions, RemovePackage, StagePackage,
    ChangePackageWithOptions, FindPackages, FindPackagesByUser, FindPackagesByName,
    FindPackagesByUserAndName, FindUsers, SetPackageState, FindPackage, CleanupPackage,
    FindPackagesByFamily, FindPackagesByUserAndFamily, FindPackageByUser,
};
static IPackageManager Manager = {&ManagerVtbl};

static HRESULT WINAPI Manager2QueryInterface(IPackageManager2 *This, REFIID Id, void **Object)
{
    return ManagerQueryInterface(&Manager, Id, Object);
}

static ULONG WINAPI Manager2AddRef(IPackageManager2 *This)
{
    return ManagerAddRef(&Manager);
}

static ULONG WINAPI Manager2Release(IPackageManager2 *This)
{
    return ManagerRelease(&Manager);
}

static HRESULT WINAPI Manager2GetIids(IPackageManager2 *This, ULONG *Count, IID **Ids)
{
    return ManagerGetIids(&Manager, Count, Ids);
}

static HRESULT WINAPI Manager2GetRuntimeClassName(IPackageManager2 *This, HSTRING *Name)
{
    return ManagerGetRuntimeClassName(&Manager, Name);
}

static HRESULT WINAPI Manager2GetTrustLevel(IPackageManager2 *This, TrustLevel *Level)
{
    return ManagerGetTrustLevel(&Manager, Level);
}

static HRESULT WINAPI Manager2Remove(IPackageManager2 *This, HSTRING Name, RemovalOptions Options,
                                      DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI Manager2Stage(IPackageManager2 *This, __x_ABI_CWindows_CFoundation_CIUriRuntimeClass *Uri,
                                     __FIIterable_1_Windows__CFoundation__CUri *Dependencies,
                                     DeploymentOptions Options, DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI Manager2Register(IPackageManager2 *This, HSTRING Name,
                                        __FIIterable_1_HSTRING *Dependencies,
                                        DeploymentOptions Options, DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static HRESULT WINAPI Manager2Find(IPackageManager2 *This, PackageTypes Types, IIterable_Package **Packages)
{
    return FindPackages(&Manager, Packages);
}

static HRESULT WINAPI Manager2FindByUser(IPackageManager2 *This, HSTRING User, PackageTypes Types,
                                          IIterable_Package **Packages)
{
    return FindPackagesByUser(&Manager, User, Packages);
}

static HRESULT WINAPI Manager2FindByName(IPackageManager2 *This, HSTRING Name, HSTRING Publisher,
                                          PackageTypes Types, IIterable_Package **Packages)
{
    return FindPackagesByName(&Manager, Name, Publisher, Packages);
}

static HRESULT WINAPI Manager2FindByUserAndName(IPackageManager2 *This, HSTRING User, HSTRING Name,
                                                 HSTRING Publisher, PackageTypes Types,
                                                 IIterable_Package **Packages)
{
    return FindPackagesByUserAndName(&Manager, User, Name, Publisher, Packages);
}

static HRESULT WINAPI Manager2FindByFamily(IPackageManager2 *This, HSTRING Family, PackageTypes Types,
                                            IIterable_Package **Packages)
{
    return FindPackagesByFamily(&Manager, Family, Packages);
}

static HRESULT WINAPI Manager2FindByUserAndFamily(IPackageManager2 *This, HSTRING User, HSTRING Family,
                                                   PackageTypes Types, IIterable_Package **Packages)
{
    return FindPackagesByUserAndFamily(&Manager, User, Family, Packages);
}

static HRESULT WINAPI Manager2StageUserData(IPackageManager2 *This, HSTRING Name,
                                             DeploymentOperation **Operation)
{
    return UnimplementedDeployment(Operation);
}

static const IPackageManager2Vtbl Manager2Vtbl =
{
    INSPECTABLE_VTBL(Manager2),
    Manager2Remove, Manager2Stage, Manager2Register, Manager2Find, Manager2FindByUser,
    Manager2FindByName, Manager2FindByUserAndName, Manager2FindByFamily,
    Manager2FindByUserAndFamily, Manager2StageUserData,
};
static IPackageManager2 Manager2 = {&Manager2Vtbl};

static HRESULT WINAPI ActivateManager(IActivationFactory *This, IInspectable **Instance)
{
    return ManagerQueryInterface(&Manager, &IID_IInspectable, (void **)Instance);
}

static const IActivationFactoryVtbl FactoryVtbl = {INSPECTABLE_VTBL(Factory), ActivateManager};
static IActivationFactory Factory = {&FactoryVtbl};

HRESULT WINAPI DllGetActivationFactory(HSTRING ClassId, IActivationFactory **Result)
{
    if (!Result) return E_POINTER;
    *Result = NULL;
    if (wcscmp(WindowsGetStringRawBuffer(ClassId, NULL), PackageManagerName))
        return CLASS_E_CLASSNOTAVAILABLE;
    return FactoryQueryInterface(&Factory, &IID_IActivationFactory, (void **)Result);
}
