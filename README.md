# SharedGL ![license](https://img.shields.io/badge/license-MIT-blue)  <img style="float: right;" src="media/icon.png" alt=icon width="192" height="192">

SharedGL (SGL) is an OpenGL implementation built upon shared memory, allowing for capturing graphics calls and accelerated graphics within virtual machines. SGL is designed with the intent to be compatible with a wide range of platforms (WGL, GLX, EGL), making it possible to run on virtually any guest system.

> [!IMPORTANT]\
> Functionality is currently limitted as no version has been fully implemented. Additionally, this has only been tested on a handful of demoes (including glxgears) on X11 (GLX) and Windows (WGL) as EGL support has not been implemented yet. If you encounter crashes or weird bugs, scroll down for troubleshooting.

# Getting started

The following libraries are required for building the server and client on linux:
- libepoxy
- SDL2
- libx11

## Build
The following script builds the host and client for linux.
```bash
git clone https://github.com/dmaivel/sharedgl.git
cd sharedgl
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

The following script compiles the windows client (this must be done on windows, through the `Visual Studio Developer Command Prompt / Powershell`).
```bash
git clone https://github.com/dmaivel/sharedgl.git
cd sharedgl
mkdir build
cd build
cmake ..
cmake --build . --target sharedgl-core --config Release
```

# Usage
Regardless of the where the client is being ran, the server must be started before starting any clients (and can only be ran on linux systems).

```bash
usage: sharedgl [-h] [-v] [-o] [-x] [-g MAJOR.MINOR] [-r WIDTHxHEIGHT] [-m SIZE]

options:
    -h                 display help information
    -v                 display virtual machine arguments
    -o                 enables fps overlay on clients (shows server side fps)
    -x                 remove shared memory file
    -g [MAJOR.MINOR]   report specific opengl version (default: 1.2)
    -r [WIDTHxHEIGHT]  set max resolution (default: 1920x1080)
    -m [SIZE]          max amount of megabytes program may allocate (default: 16mib)
```

## Running clients
### Linux
For your OpenGL application to communicate with the server, the client library must be specified in your library path. Upon exporting, any program you run in the terminal where you inputted this command will run with the SGL binary.

```bash
$ export LD_LIBRARY_PATH=/path/to/sharedgl/build
$ glxgears
$ ...
```

### Windows (VM)
For your OpenGL application to communicate with the server, the client library (`opengl32.dll`) must be located in the same directory as the application. Scroll down for more information regarding the setup for Windows guests.

# Virtual machines
Before you start up your virtual machine, you must pass a shared memory device and start the server before starting the virtual machine. This can be done within libvirt's XML editor or the command line. Use the `-v` command line argument when starting the server and place the output in its respective location, depending on whether you use libvirt or qemu. Scroll down for driver information.

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

> [!WARNING]\
> If you move the client library to the guest from the host instead of compiling it in the guest, you may encounter the `SIGILL` exception in the virtual machine as the build compiles with the native (host) architecture. To fix, either change your cpu model to `host-model`/`host-passthrough` or comment out the `-march=native` line in the cmake script (will most likely reduce performance).

## Linux
For virtual linux clients, an additional kernel module needs to be compiled. The compiled result, `sharedgl.ko`, needs to be moved into the guest, and loaded. There is an `install.sh` script within this directory which may be moved alongside the module for ease of use. It is recommended that you add `sharedgl` to your modprobe config following installation, otherwise it must be loaded manually on each boot.
```bash
# within 'sharedgl' directory
cd kernel
make
```

Additionally, `libGL.so.1` needs to be moved into the guest for library loading.

## Windows
For windows clients, the Windows VirtIO Drivers need to be installed, which can be found [here](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/). (Navigate to `...\virtio-win-upstream\Win10\amd64\`, right click on `ivshmem.inf`, and press `Install`).

# Limitations / Issues
- Server can only handle a single client at a time
- Missing many GL functions
- Vsync does not work
- Resizing is not handled
- Windows guests rendering is janky and prone to crashing
- Inaccurate FPS in overlay (to-do: move timings from server to client)
- GLFW cant request OpenGL profiles

# Troubleshooting
You may encounter weird crashes/faults/errors such as `IOT instruction` or `No provider of glXXX found.`. Although the code base is buggy, these are some tips to try to further attempts to get an application to work:
- Change the GL version (i.e `-g 2.0`)
- Allocate more memory (i.e `-m 256`)

# Showcase

<details>
<summary>Click to reveal: Running minetest in a windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/26c6216d-72f7-45b4-9c4f-1734de0d1c6d

</details>

<details>
<summary>Click to reveal: Running glxgears in a windows virtual machine</summary>
    
https://github.com/dmaivel/sharedgl/assets/38770072/a774db97-807e-46b9-a453-fa2ee3f4ea84

</details>

<details>
<summary>Click to reveal: Running glxgears in a linux virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/0d46bf46-5693-4842-a81f-2f186c396e26

</details>

<details>
<summary>Click to reveal: Running a compute shader in a linux virtual machine</summary>
    
https://github.com/dmaivel/sharedgl/assets/38770072/ded179b8-23dc-491d-ba34-4108e014f296

</details>