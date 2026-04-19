<img align="right" width="8%" src="https://github.com/dmaivel/sharedgl/assets/38770072/7ad42df9-d10c-413a-9ef4-7682c06dc679">

# SharedGL ![license](https://img.shields.io/badge/license-MIT-blue)

SharedGL is an OpenGL 4.6 implementation that enables 3D acceleration for Windows and Linux guests within QEMU/KVM by streaming OpenGL commands over shared memory or sockets.

<details open>
<summary>Click to hide: Preview</summary>

![preview](https://github.com/user-attachments/assets/fbcd62ac-090d-4ad1-b37c-bd1f0432866a)

</details>

<details>
<summary>Click to reveal: Table of contents</summary>

1. [How it works](#how-it-works)
2. [Building the server](#building-the-server)
3. [Running the server](#running-the-server)
4. [Connecting the guest](#connecting-the-guest)
   - [Choosing a transport](#choosing-a-transport)
   - [Windows guest](#windows-guest)
   - [Linux guest](#linux-guest)
   - [Linux host (no VM)](#linux-host-no-vm)
   - [Environment variables](#environment-variables)
5. [Troubleshooting](#troubleshooting)
6. [Showcase](#showcase)

</details>

# How it works

SharedGL has two halves:

- Server (`sglrenderer`) which runs on the host, Linux only. Receives OpenGL commands and renders them in a window on the host.
- Client (ICD / `libGL`) which runs in the guest. Installed inside the VM so that OpenGL applications there transparently forward their calls to the server.

The two halves communicate through one of two transports:

| Transport | Speed | Setup |
|-----------|-------|-------|
| Shared memory  | Faster | Default, requires an `ivshmem` device on the VM and a kernel driver in the guest. |
| Network socket | Slower | No drivers, no VM config. It works anywhere with a network connection, so it's not restricted to just VMs. |

You pick the transport when you start the server. The rest of this README follows that order: build the server, run it, then connect a guest.

# Building the server

### Dependencies

| Name | Version |
|------|---------|
| [CMake](https://cmake.org/) | 3.15+ |
| [libepoxy](https://github.com/anholt/libepoxy) | Latest |
| [SDL2](https://www.libsdl.org/) | 2.24.0+ |

### Prebuilt tarball

If you just want to run the Linux server, you can download the Linux release tarball instead of building from source.

### Build

```bash
git clone https://github.com/dmaivel/sharedgl.git
cd sharedgl
mkdir build && cd build
cmake ..
cmake --build . --target sglrenderer --config Release
```

The server can only be built and run on a Linux host.

If you also want the Linux client library (`libGL`) for use on the host or to copy into a Linux guest, build the `sharedgl-core` target instead. `libx11` is required for this.

### Build options

Pass these to `cmake` with `-D...`, or edit them via `ccmake build`.

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `LINUX_LIB32` | `ON`/`OFF` | `OFF` | Build the Linux client library as 32-bit. Does not affect the server. |

# Running the server

```
usage: sglrenderer [-h] [-v] [-o] [-n] [-x] [-g MAJOR.MINOR]
                   [-r WIDTHxHEIGHT] [-m SIZE] [-p PORT]
```

| Flag | Description |
|------|-------------|
| `-h` | Show help |
| `-v` | Print sample VM configuration for the current settings |
| `-o` | Enable FPS overlay on clients |
| `-n` | Use networking instead of shared memory |
| `-x` | Remove the shared memory file (useful for cleanup) |
| `-g MAJOR.MINOR` | Report a specific OpenGL version (default: `4.6`) |
| `-r WxH` | Max resolution (default: `1920x1080`) |
| `-m SIZE` | Max memory in MiB (default: `32`) |
| `-p PORT` | Port when `-n` is used (default: `3000`) |

The server must be running on the host before you start the guest. If you extracted a Linux release tarball, run `./sglrenderer` from the extracted root.

# Connecting the guest

## Choosing a transport

Before you install anything in the guest, decide how the guest will reach the server:

- Shared memory: default, faster. You'll need to add an `ivshmem` device to the VM and install a kernel driver in the guest.
- Networking: driverless, simpler, much slower. Start the server with `-n`, and set `SGL_NETWORK_ENDPOINT=<host-ip>:<port>` as an environment variable inside the guest.

### VM configuration for shared memory

Run `sglrenderer -v` to get configuration snippets matched to your current settings. For reference:

**libvirt:**
```xml
<!-- SAMPLE, place inside <devices> -->
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
# SAMPLE
qemu-system-x86_64 \
  -object memory-backend-file,size=??M,share,mem-path=/dev/shm/sharedgl_shared_memory,id=sharedgl_shared_memory
```

## Windows guest

A Windows guest needs two things: a kernel driver (for shared memory only) and the OpenGL client library (the ICD).

### 1. Kernel driver

Pick one of the three options below.

#### Option A: Stock VirtIO IVSHMEM driver (single client only)

1. Download and extract the [upstream VirtIO Win drivers](https://fedorapeople.org/groups/virt/virtio-win/direct-downloads/upstream-virtio/).
2. Go to `...\virtio-win-upstream\Win10\amd64\`.
3. Right-click `ivshmem.inf` and choose Install.

#### Option B: Stock VirtIO IVSHMEM driver + runtime patch (multi-client)

Same driver as Option A, but patched at runtime by [ntoseye](https://github.com/dmaivel/ntoseye) to allow multiple clients. The patch must be re-applied every time the VM boots.

1. Follow Option A to install the stock driver.
2. Install and run `ntoseye` (see its README if you hit issues).
3. In the `ntoseye` console, run:
   ```
   lm ivshmem                          # prints start address
   s <start> 4883792000740a 0x3000     # prints match address
   f <match> 4883792000eb0a            # applies the patch
   continue
   quit
   ```

#### Option C: Bundled `ksgldrv` driver (multi-client, Windows 10)

Requires enabling test-signing once:

```
bcdedit.exe -set testsigning on
```

Then either install from a release or build from source:

<details>
<summary>Install from release (release 0.4.0)</summary>

1. Download and extract the latest Windows release.
2. Right-click `ksgldrv.inf` and choose Install.

</details>

<details>
<summary>Build from source (Visual Studio Developer Command Prompt)</summary>

Install the [WDK](https://learn.microsoft.com/en-us/windows-hardware/drivers/other-wdk-downloads) first, then:

```bat
:: from the sharedgl directory
mkdir build
cd build
cmake -DCMAKE_GENERATOR_PLATFORM=x64 -DWINKERNEL=ON ..
cmake --build . --target ksgldrv --config Release
cd ..
xcopy .\scripts\kcertify.bat .\build\Release\kcertify.bat
xcopy .\scripts\ksgldrv.inf .\build\Release\ksgldrv.inf
cd build\Release
call kcertify.bat 10_X64

:: requires admin, or right-click ksgldrv.inf and choose Install
pnputil -i -a ksgldrv.inf
```

The default target is `10_X64`. For other or additional Windows versions, pass them to `kcertify.bat` (e.g. `kcertify.bat 10_X64,10_NI_X64`). See the [OS version list](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/inf2cat).

</details>

### 2. Client library (ICD)

Install from a release or build from source.

<details>
<summary>Install from release (>= 0.3.1)</summary>

1. Download and extract the latest Windows release.
2. Run `wininstall.bat` as administrator.

Any OpenGL application (32-bit or 64-bit) will now route through SharedGL.

</details>

<details>
<summary>Build from source (Visual Studio Developer Command Prompt)</summary>

```bat
:: from the sharedgl directory
mkdir build
cd build
:: add -DWINKERNEL=OFF if you hit WDK errors
cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..
cmake --build . --target sharedgl-core --config Release
cmake -DCMAKE_GENERATOR_PLATFORM=Win32 ..
cmake --build . --target sharedgl-core --config Release
cd ..
xcopy .\scripts\wininstall.bat .\build\Release\wininstall.bat
cd build\Release
call wininstall.bat
```

</details>

## Linux guest

Although it's possible to use SharedGL with Linux guests, there is little reason to given first-class 3D acceleration support via virtio/virgl.

A Linux guest needs a kernel module (for shared memory only) and the client library.

### 1. Kernel module

Build inside the guest, then load with the provided installer:

```bash
cd kernel/linux
make
./install.sh
```

If the module doesn't auto-load on boot, add `sharedgl` to your modprobe config.

### 2. Client library

Build `sharedgl-core` as described in [Building the server](#building-the-server), or use the packaged client libraries from the Linux release tarball. Then point your loader at the client directory:

```bash
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/path/to/sharedgl/lib
glxgears
```

Use `lib32/` instead of `lib/` for 32-bit applications. Some applications also need `LD_PRELOAD` set to the same directory.

## Linux host (no VM)

The Linux client library also works directly on the host, useful for debugging without a VM. Build `sharedgl-core` or use the packaged tarball, set `LD_LIBRARY_PATH` as shown in the [Linux guest](#linux-guest) section, and run your application.

## Environment variables

Once the client is installed, these variables tune its behavior. `SGL_NETWORK_ENDPOINT` is required when the server is running in networking mode; the rest are optional. Variables marked `host` inherit from the server when not overridden.

Boolean env vars can only be enabled by setting them to `true`, not `1` or `on`.

| Variable | Values | Default | Platform | Description |
|----------|--------|---------|----------|-------------|
| `SGL_NETWORK_ENDPOINT` | `IP:Port` |  | Windows, Linux | Required in the guest when networking is enabled. |
| `SGL_WINED3D_DONT_VFLIP` | boolean | `false` | Windows | Set to `true` when running DirectX apps through WineD3D so the framebuffer renders right-side up. |
| `SGL_RUN_WITH_LOW_PRIORITY` | boolean | `false` | Windows | Runs the client at `IDLE_PRIORITY_CLASS`. Can improve smoothness on VMs with fewer vCPUs than host cores, or when using networking. |
| `GL_VERSION_OVERRIDE` | `D.D` | `host` | Windows, Linux | Override the reported OpenGL version. |
| `GLX_VERSION_OVERRIDE` | `D.D` | `1.4` | Linux | Override the reported GLX version. |
| `GLSL_VERSION_OVERRIDE` | `D.D` |  | Windows, Linux | Override the reported GLSL version. |

# Troubleshooting

#### Client logs `glimpl_init: failed to find memory`
The VM doesn't have a shared memory device attached. Add the `ivshmem` config from [VM configuration for shared memory](#vm-configuration-for-shared-memory), or switch to networking.

#### Server logs `err: failed to open shared memory 'sharedgl_shared_memory'
The shared memory file already exists with different permissions. Shut down the VM, run `sudo ./sglrenderer -x` to remove it, then start the server and VM again. Running the server with `sudo` is a shorter-term workaround.

#### Application stalls in the VM (process starts but hangs)
- Confirm the server is running on the host. If it is and the app still stalls, shut down the VM, run `sudo ./sglrenderer -x`, restart the server, then boot the VM.
- Confirm the driver is installed (VirtIO IVSHMEM or `ksgldrv` on Windows; `sharedgl.ko` on Linux).

#### Blank window in the VM
The shared memory device likely isn't sized to cover all the memory the server allocates. Check the `<size>` in your VM config against the server's `-m` value.

#### Crashes like `IOT instruction` or `No provider of glXXX found.`
- Try a lower reported GL version: `-g 2.0` or env vars
- Give the server more memory: `-m 256`

#### GLFW apps report "Entry point retrieval is broken"
Use `LD_PRELOAD` with the client library.

#### Linux guest crashes with `SIGILL`
The client library was built on a host with a different CPU than the guest sees.

# Showcase

<details>
<summary>Click to reveal: Running SuperTuxKart in a Windows virtual machine</summary>

https://github.com/dmaivel/sharedgl/assets/38770072/c302f546-2c05-4cb7-b415-8f01ad1dce7a

</details>

<details>
<summary>Click to reveal: Running a DirectX sample via WineD3D in a Windows virtual machine</summary>

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
