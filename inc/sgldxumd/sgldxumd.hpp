#ifndef _SGLDXUMD_HPP_
#define _SGLDXUMD_HPP_

#define D3D10DDI_MINOR_HEADER_VERSION 2

#include <windows.h>

#ifdef _MSC_VER
#include <d3d10_1.h>
#endif

#include <d3d10umddi.h>

#ifndef STATUS_NOT_IMPLEMENTED
#define STATUS_NOT_IMPLEMENTED 0xC0000002
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000
#endif

#endif