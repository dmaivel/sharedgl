<img align="right" width="22%" src="https://github.com/dmaivel/sharedgl/assets/38770072/7ad42df9-d10c-413a-9ef4-7682c06dc679">

# SharedGL ![license](https://img.shields.io/badge/license-MIT-blue)

SharedGL is an OpenGL implementation that enables 3D acceleration for Windows and Linux guests within QEMU/KVM by streaming OpenGL commands over shared memory *(and networks, for devices over LAN)*. For future plans, click [here](https://github.com/users/dmaivel/projects/2) for the roadmap.

<details>
<summary>Click to reveal: Table of contents</summary>

1. [Getting started](#getting-started)
2. [Usage](#usage)
   - [Environment variables](#environment-variables)
   - [Windows in a VM](#windows-in-a-vm)
   - [Linux](#linux)
      - [Linux in a VM](#linux-in-a-vm)
3. [Networking](#networking)
4. [Virtual machines](#virtual-machines)
5. [Supported GL versions](#supported-gl-versions)
6. [Limitations / Issues](#limitations--issues)
7. [Troubleshooting](#troubleshooting)
8. [Showcase](#showcase)

</details>

# Getting started

The following libraries are required for building the server on Linux:
- libepoxy
- SDL2

The following script builds `sglrenderer` for Linux:
```bash
git clone https://github.com/dmaivel/sharedgl.git
cd sharedgl
mkdir build
cd build
cmake ..
cmake --build . --target sglrenderer --config Release
```

If you also wish to build the client library for Linux, `libx11` is required. Build with `--target sharedgl-core`.

For detailed build instructions for Windows, visit the [Windows section](#windows-in-a-vm). The renderer/server is only supported on Linux hosts.

### Build options

These CMake options are accessible by either:
1. Using `ccmake` on `build` folder
2. Configuring with `-D...`

| **Option** | **Legal values** | **Default** | **Description** |
|-|-|-|-|
| LINUX_LIB32 | ON/OFF | OFF | Enable if you wish to build the Linux client library (libGL) as 32-bit. This does not affect the server. |

# Usage
The server must be started on the host before running any clients. Note that the server can only be ran on Linux.

```bash
usage: sglrenderer [-h] [-v] [-o] [-n] [-x] [-g MAJOR.MINOR] [-r WIDTHxHEIGHT] [-m SIZE] [-p PORT]
    
options:
    -h                 display help information
    -v                 display virtual machine arguments
    -o                 enables fps overlay on clients
    -n                 enable networking instead of shared memory
    -x                 remove shared memory file
    -g [MAJOR.MINOR]   report specific opengl version (default: 4.6)
    -r [WIDTHxHEIGHT]  set max resolution (default: 1920x1080)
    -m [SIZE]          max amount of megabytes program may allocate (default: 32mib)
    -p [PORT]          if networking is enabled, specify which port to use (default: 3000)
```

### Environment variables

Variables labeled with `host` get their values from the host/server when their override isn't set.

| **Option** | **Legal values** | **Default** | **Description** |
|-|-|-|-|
| SGL_WINED3D_DONT_VFLIP | Boolean | false | If running a DirectX application via WineD3D, ensure this variable is set to `true` in order for the application to render the framebuffer in the proper orientation. Only available for Windows clients. |
| SGL_RUN_WITH_LOW_PRIORITY | Boolean | false | On single core setups, by setting the process priority to low / `IDLE_PRIORITY_CLASS`, applications will run smoother as the kernel driver is given more CPU time. Users should only set this to `true` if the VM has only a single VCPU. Only available for Windows clients. |
| GL_VERSION_OVERRIDE | Digit.Digit | `host` | Override the OpenGL version on the client side. Available for both Windows and Linux clients. |
| GLX_VERSION_OVERRIDE | Digit.Digit | 1.4 | Override the GLX version on the client side. Only available for Linux clients. |
| GLSL_VERSION_OVERRIDE | Digit.Digit |  | Override the GLSL version on the client side. Available for both Windows and Linux clients. |
| SGL_NET_OVER_SHARED | Ip:Port | | If networking is enabled, this environment variable must exist on the guest. Available for both Windows and Linux clients. |

## Windows (in a VM)

[Windows is only supported as a guest: Click here for virtual machine configuring, which is required for the guest to see SharedGL's shared memory](#virtual-machines)

Two things must be done for the windows installation:
1. Install a compatible driver
2. Install the clients

### Kernel driver

There are two possible drivers one may use:
1. VirtIO's IVSHMEM driver (no multiclient support)
    1. Download and extract the upstream virtio win drivers, found [here](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/).
    2. Navigate into `...\virtio-win-upstream\Win10\amd64\`.
    3. Right click on `ivshmem.inf` and press `Install`.

> [!WARNING]\
> If you use the included driver, test signing must be on. Enable it by running the following command in an elevated command prompt: `bcdedit.exe -set testsigning on` and restart.

2. Included driver (multiclient support)
    1. Use the release (>= `0.4.0`) **(Windows 10 only)**
        1. Download the latest release for windows and extract the zip file.
        2. Navigate into the extracted folder.
        3. Right click on `ksgldrv.inf` and press `Install`.
    2. Compile from source (use Visual Studio Developer Command Prompt)
        1. Ensure you have installed the `WDK`, which can be found [here](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads).
        2. ```bat
           :: git clone https://github.com/dmaivel/sharedgl.git
           :: cd sharedgl
           mkdir build
           cd build
           cmake -DCMAKE_GENERATOR_PLATFORM=x64 -DWINKERNEL=ON ..
           cmake --build . --target ksgldrv --config Release
           cd ..
           xcopy .\scripts\kcertify.bat .\build\Release\kcertify.bat
           xcopy .\scripts\ksgldrv.inf .\build\Release\ksgldrv.inf
           cd build\Release
           call kcertify.bat 10_X64

           :: requires admin privs, you may right click on the file and press install instead
           pnputil -i -a ksgldrv.inf
           ```
        3. By default, this builds for Windows 10 x64 (`10_X64`). If you wish to compile for a different version or multiple versions, you must provide it through the command line like so: `kcertify.bat 10_X64,10_NI_X64`. A list of OS versions is provided on MSDN [here](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/inf2cat).

### Library / ICD

There are two ways to install the library on windows:
1. Use a release (>= `0.3.1`)
    1. Download the latest release for windows and extract the zip file.
    2. Navigate into the extracted folder and run `wininstall.bat` and allow admin privledges.
    3. The libraries should now be installed, meaning any application that uses OpenGL (32-bit and 64-bit) will use SharedGL.
2. Compile from source (use Visual Studio Developer Command Prompt)
    1. ```bat
       :: git clone https://github.com/dmaivel/sharedgl.git
       :: cd sharedgl
       mkdir build
       cd build
       :: if you get errors regarding wdk, also use -DWINKERNEL=OFF
       cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
       cmake --build . --target sharedgl-core --config Release
       cmake -DCMAKE_GENERATOR_PLATFORM=Win32 ..
       cmake --build . --target sharedgl-core --config Release
       cd ..
       xcopy .\scripts\wininstall.bat .\build\Release\wininstall.bat
       cd build\Release
       call wininstall.bat
       ```

## Linux

> [!IMPORTANT]
> The following sections discuss using the *client library*, not the *renderer/server*. If your intention is to only accelerate Windows guests, you may disregard this section as all you need to do is run the renderer, no additional libraries required (other than the dependencies).

For your OpenGL application to communicate with the server, the client library must be specified in your library path. Upon exporting, any program you run in the terminal where you inputted this command will run with the SGL binary.

```bash
$ export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/sharedgl/build
$ glxgears
$ ...
```

Some applications may require adding the library to the `LD_PRELOAD` environment variable aswell, which is done the same way as shown above.

Note that the Linux library does not need to be used in a virtual machine, allowing users to debug the library entirely on the host.

Some applications may require an explicit `libGLX`, so run `ln -s libGL.so.1 libGLX.so.0` in `build` to make a symlink.

### Linux in a VM

[Click here for virtual machine configuring, which is required for the guest to see SharedGL's shared memory](#virtual-machines)

For virtual Linux clients, an additional kernel module needs to be compiled in the virtual machine, resulting in a binary `sharedgl.ko` which needs to be loaded. Loading/installing can be done by running the provided script (`./kernel/linux/install.sh`), following compilation. If the module doesn't load on boot, it is recommended that you add `sharedgl` to your modprobe config.

```bash
# within 'sharedgl' directory
cd kernel/linux
make
```

> [!WARNING]\
> If you move the client library to the guest from the host instead of compiling it in the guest, you may encounter the `SIGILL` exception in the virtual machine as the build compiles with the native (host) architecture. To fix, either change your cpu model to `host-model`/`host-passthrough` or comment out the `-march=native` line in the cmake script (will most likely reduce performance).

# Networking

Starting from `0.5.0`, SharedGL offers a networking feature that may be used in place of shared memory. No additional drivers are required for the network feature, meaning if you wish to have a driverless experience in your virtual machine, networking is the given alternative. If the networking feature is used exclusively, the kernel drivers do not need be compiled/installed. However, installation of the ICD for either Linux or Windows is still required.
  - Start the server using `-n` (and provide a port if the default is not available through `-p PORT`)
  - Ensure the client libraries are installed
  - Ensure that the environment variable `SGL_NET_OVER_SHARED=ADDRESS:PORT` exists in the guest (`ADDRESS` being the host's IP address)

If the network feature feels too slow, you may want to modify `SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT` in `inc/network/packet.h`, which can be ranged from [1, 15360]:
```diff
/*
 * 256: safe, keeps packet size under 1400 bytes
 * 512: default
 * 15360: largest, may result in fragmentation
 */
- #define SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT 512
+ #define SGL_FIFO_UPLOAD_COMMAND_BLOCK_COUNT 15360
```

Note that changing this file will require rebuilding the client and server.

# Virtual machines

Before starting the virtual machine, you must pass a shared memory device and start the server before starting the virtual machine. This can be done within libvirt's XML editor or the command line. Before starting the virtual machine, start the server using `-v`, which will start the server and print the necessary configurations:

```bash
$ ./sglrenderer -v [OTHER PARAMETERS]
```

> [!IMPORTANT]\
> Once these configurations are in place, running the server with `-v` is not required. If you make changes to the amount of memory to reserve using the `-m` argument, you must reflect that change in the virtual machine configuration.

**libvirt:**
```xml
<!--> THIS IS A SAMPLE; ONLY USE THIS AS A GUIDE ON WHERE TO PLACE THE OUTPUT <-->
...
<devices>
    ...
    <shmem name="sharedgl_shared_memory">
        <model type="ivshmem-plain"/>
        <size unit="M">??</size>
    </shmem>
</devices>
```

**qemu:**
```bash
# THIS IS A SAMPLE; ONLY USE THIS AS A GUIDE ON WHERE TO PLACE THE OUTPUT
qemu-system-x86_64 -object memory-backend-file,size=??M,share,mem-path=/dev/shm/sharedgl_shared_memory,id=sharedgl_shared_memory
```

# Supported GL versions
This list describes the amount of functions left from each standard to implement. This excludes EXT/ARB functions. This list may be inaccurate in terms of totals and also counts stubs as implementations.

- [x] OpenGL 1
    - [x] 1.0 (~306 total)
    - [x] 1.1 (~30 total) 
    - [x] 1.2 (~4 total) 
    - [x] 1.3 (~46 total) 
    - [x] 1.4 (~47 total) 
    - [x] 1.5 (~19 total) 
- [x] OpenGL 2
    - [x] 2.0 (~93 total) 
    - [x] 2.1 (~6 total) 
- [x] OpenGL 3
    - [x] 3.0 (~84 total) 
    - [x] 3.1 (~15 total) 
    - [x] 3.2 (~19 total)
    - [x] 3.3 (~58 total)
- [x] OpenGL 4
    - [x] 4.0 (~46 total) 
    - [x] 4.1 (~89 total) 
    - [x] 4.2 (~12 total) 
    - [x] 4.3 (~44 total) 
    - [x] 4.4 (~9 total) 
    - [x] 4.5 (~122 total) 
    - [x] 4.6 (~4 total) 

# Limitations / Issues
- New GLX FB configs may cause applications using `freeglut` or `glad` to no longer run (only tested on Linux clients).
- Incomplete framebuffers when using network feature

# Troubleshooting

1. If you encounter "Entry point retrieval is broken" on applications that use GLFW, use `LD_PRELOAD`.
2. If you encounter weird crashes/faults/errors such as `IOT instruction` or `No provider of glXXX found.`:
    - Try changing the GL version (i.e `-g 2.0`)
    - Allocate more memory (i.e `-m 256`)
3. Application shows a blank window in the virtual machine?
    - Make sure the shared memory device passes through all the memory (check the size)
4. Application doesn't run in the virtual machine? (Process exists but stalls)
    - Make sure the server is running
        - If you start the server and it still won't run, shut down the VM, run `sudo ./sglrenderer -x`, start the server, start the VM
    - Make sure the drivers are installed (VirtIO IVSHMEM for Windows, custom kernel must be compiled for Linux)
5. Server reports, `err: failed to open shared memory 'sharedgl_shared_memory'`
    - This (usually) happens when the shared memory file is created before the server runs, meaning the file was created with different privileges. You may either:
        - Run the server as `sudo`
        - Shutdown the VM, run `sudo ./sglrenderer -x`, start the server, then start the VM
6. Client outputs, `glimpl_init: failed to find memory` to the terminal
    - This occurs in VMs when you do not pass a shared memory device, which is required for the clients to see the shared memory

# Showcase

<details>
<summary>Click to reveal: Running SuperTuxKart in a Windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/c302f546-2c05-4cb7-b415-8f01ad1dce7a

</details>

<details>
<summary>Click to reveal: Running DirectX sample using WineD3D in a Windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/f2ae2825-79d6-4c1b-813f-c826586642e2

</details>

<details>
<summary>Click to reveal: Running minetest in a Windows virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/26c6216d-72f7-45b4-9c4f-1734de0d1c6d

</details>

<details>
<summary>Click to reveal: Running glxgears in a Windows virtual machine (old)</summary>
    
https://github.com/dmaivel/sharedgl/assets/38770072/a774db97-807e-46b9-a453-fa2ee3f4ea84

</details>

<details>
<summary>Click to reveal: Running glxgears in a Linux virtual machine (old)</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/0d46bf46-5693-4842-a81f-2f186c396e26

</details>

<details>
<summary>Click to reveal: Running a compute shader in a Linux virtual machine (old)</summary>
    
https://github.com/dmaivel/sharedgl/assets/38770072/ded179b8-23dc-491d-ba34-4108e014f296

</details>