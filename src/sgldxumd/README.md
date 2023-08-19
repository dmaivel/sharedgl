# sgldxumd

> [!WARNING]\
> This is purely experimental as is not currently fully functional. Initially this was supposed to allow for proper ICD installation, but it appears there is another way installation can be achieved (seemingly undocumented). Thus, this portion of the repository is reserved as a playground and may possibly be used for supporting DirectX in the future without the need for an API conversation layer such as WineD3D.

This portion of the code base pertains to the DirectX Usermode Driver, which is required for an OpenGL ICD. Resources used for development:
- Mesa (`mesa/src/gallium/frontends/d3d10umd`)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/loading-an-opengl-installable-client-driver
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/user-mode-display-drivers