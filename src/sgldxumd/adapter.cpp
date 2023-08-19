#include <sgldxumd/sgldxumd.hpp>
#include <sgldxumd/device.hpp>

static int numAdapters = 0;

static const UINT64 SupportedDDIInterfaceVersions[] = {
    D3D10_0_DDI_SUPPORTED,
    D3D10_0_x_DDI_SUPPORTED,
    D3D10_0_7_DDI_SUPPORTED,

    D3D10_1_DDI_SUPPORTED,
    D3D10_1_x_DDI_SUPPORTED,
    D3D10_1_7_DDI_SUPPORTED,

    D3D11_0_DDI_SUPPORTED,
    D3D11_0_7_DDI_SUPPORTED
};

static HRESULT APIENTRY GetSupportedVersions(D3D10DDI_HADAPTER hAdapter, UINT32 *puEntries, UINT64 *pSupportedDDIInterfaceVersions)
{
    if (pSupportedDDIInterfaceVersions && *puEntries < ARRAYSIZE(SupportedDDIInterfaceVersions))
        return E_OUTOFMEMORY;

    *puEntries = ARRAYSIZE(SupportedDDIInterfaceVersions);

    if (pSupportedDDIInterfaceVersions)
        memcpy(pSupportedDDIInterfaceVersions, SupportedDDIInterfaceVersions, sizeof(SupportedDDIInterfaceVersions));

    return S_OK;
}

static HRESULT APIENTRY GetCaps(D3D10DDI_HADAPTER hAdapter, const D3D10_2DDIARG_GETCAPS *pData)
{
    memset(pData->pData, 0, pData->DataSize);
    return S_OK;
}

static HRESULT APIENTRY CloseAdapter(D3D10DDI_HADAPTER hAdapter)
{
    --numAdapters;
    return S_OK;
}

static SIZE_T APIENTRY CalcPrivateDeviceSize(D3D10DDI_HADAPTER hAdapter, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *pData)
{
    return 4;
}

static void APIENTRY SetAdapter(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    pOpenData->pAdapterFuncs->pfnCalcPrivateDeviceSize = CalcPrivateDeviceSize;
    pOpenData->pAdapterFuncs->pfnCreateDevice = CreateDevice;
    pOpenData->pAdapterFuncs->pfnCloseAdapter = CloseAdapter;
}

EXTERN_C HRESULT APIENTRY OpenAdapter10(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    switch (pOpenData->Interface) {
    case D3D10_0_DDI_INTERFACE_VERSION:
    case D3D10_0_x_DDI_INTERFACE_VERSION:
    case D3D10_0_7_DDI_INTERFACE_VERSION:
    case D3D10_1_DDI_INTERFACE_VERSION:
    case D3D10_1_x_DDI_INTERFACE_VERSION:
    case D3D10_1_7_DDI_INTERFACE_VERSION:
    case D3D11_0_DDI_INTERFACE_VERSION:
    case D3D11_0_7_DDI_INTERFACE_VERSION:
        break;
    default:
        return E_FAIL;
    }

    ++numAdapters;

    SetAdapter(pOpenData);

    return S_OK;
}

EXTERN_C HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *pOpenData)
{
    pOpenData->pAdapterFuncs_2->pfnGetSupportedVersions = GetSupportedVersions;
    pOpenData->pAdapterFuncs_2->pfnGetCaps = GetCaps;

    ++numAdapters;

    SetAdapter(pOpenData);

    return S_OK;
}