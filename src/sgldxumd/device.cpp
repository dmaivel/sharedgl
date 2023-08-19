#include <sgldxumd/device.hpp>
#include <sgldxumd/dxgi.hpp>

static void APIENTRY DestroyDevice(D3D10DDI_HDEVICE hDevice)
{
    /* stub */
}

EXTERN_C HRESULT APIENTRY CreateDevice(D3D10DDI_HADAPTER hAdapter, D3D10DDIARG_CREATEDEVICE *pCreateData)
{
    switch (pCreateData->Interface) {
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

    D3D10DDI_DEVICEFUNCS *pDeviceFuncs = pCreateData->pDeviceFuncs;

    /*
     * DDI
     */
    pDeviceFuncs->pfnDestroyDevice = DestroyDevice;

    /*
     * DXGI
     */
    

    /*
     * https://learn.microsoft.com/en-us/windows-hardware/drivers/display/supporting-the-dxgi-ddi
     *
     * We must pretend that our library is a software rasterizer as there is currently no km
     * driver, as everything is (mostly) handled in usermode.
     */
    return DXGI_STATUS_NO_REDIRECTION;
}