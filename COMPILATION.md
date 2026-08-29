# Compilation Guide - MyRemote Control

This document provides detailed instructions for building MyRemote Control on Windows.

> **部分内容已过时**：本文件的若干章节（Protobuf、FFmpeg/MediaFoundation、OpenGL）写于选型变更之前。
> 当前实现是 **OpenH264 软编码 + DXGI 桌面复制（BitBlt 回退）+ 手写二进制分帧协议 + Qt Widgets 控制端**，
> 请以 [README.md](README.md) 的构建章节与代码（`CMakeLists.txt`）为准；本文件只在 Visual Studio/CMake
> 安装步骤和故障排查框架上仍然有用。

## Prerequisites Installation

### 1. Visual Studio 2022 (Required)

Download from: https://visualstudio.microsoft.com/downloads/

**Required Workloads:**
- ✅ Desktop development with C++
- ✅ C++ CMake tools for Windows

**Additional Components:**
- Windows 10/11 SDK
- MSVC v143 - VS 2022 C++ x64/x86 build tools

### 2. CMake (>= 3.16)

Download from: https://cmake.org/download/

Install and add to system PATH if not automatically added.

Verify installation:
```cmd
cmake --version
```

### 3. Qt 6.x (MSVC Version Only)

Download from: https://download.qt.io/official_releases/qt/

**Important**: Must use **Qt version that matches your MSVC toolset**
- For VS 2022: Use Qt 6.5+ MSVC2019_64 or MSVC2022_64

**Installation Steps:**
1. Run Qt Online Installer
2. Select Qt 6.x versions (Community Edition is fine)
3. Choose components:
   - ✅ Mingw (if needed)
   - ✅ MSVC 2019 64-bit OR MSVC 2022 64-bit
   - ✅ Qt Charts (for optional charts later)
   - ❌ Do NOT install MinGW if you're using MSVC

**Configure Qt in CMake:**
Edit `CMakeLists.txt` and set correct Qt path if needed:
```cmake
find_package(Qt6 REQUIRED 
    COMPONENTS Widgets Concurrent Network 
    HINTS "C:/Qt/6.7.0/msvc2022_64"
)
```

### 4. OpenSSL Development Libraries

Option A: Prebuilt binaries
- Download from: https://wiki.openssl.org/index.php/Binaries
- Extract to: `C:/OpenSSL-Win64/`

Option B: Build from source (recommended for custom configurations)
```bash
git clone https://github.com/openssl/openssl.git
cd openssl
./ Configure VC-WIN64A
nmake
nmake install
```

**Configure in CMake** (`CMakeLists.txt`):
```cmake
# For prebuilt binaries
set(OPENSSL_ROOT_DIR "C:/OpenSSL-Win64")
find_package(OpenSSL REQUIRED)

target_link_libraries(common_lib PUBLIC OpenSSL::Crypto)
```

### 5. Protocol Buffers Compiler

Download from: https://github.com/protocolbuffers/protobuf/releases

**Installation:**
1. Download prebuilt binary (e.g., `protobuf-cpp-3.25.1-visualstudio.zip`)
2. Extract to: `C:/protobuf/`
3. Add `C:/protobuf/bin` to system PATH

Verify protoc:
```cmd
protoc --version
# Should show: libprotoc 3.x.x
```

### 6. Optional Dependencies

#### FFmpeg (for H.264 decoding on server side)
- Download from: https://www.gyan.dev/ffmpeg/builds/
- Extract DLLs and headers to project

#### NVIDIA Video Codec SDK (optional, for NVENC encoder fallback)
- Download from: https://developer.nvidia.com/nvidia-video-codec-sdk

## Building Process

### Method 1: Automated Build Script (Recommended)

1. Open "Developer Command Prompt for VS 2022"
2. Navigate to project directory:
   ```cmd
   cd C:\path\to\MyRemote
   ```
3. Run build script:
   ```cmd
   build.bat
   ```

Output location: `build/bin/Debug/` and `build/bin/Release/`

### Method 2: Manual CMake Build

```cmd
# 1. Create build directory
mkdir build && cd build

# 2. Generate CMake configuration
cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DBUILD_CLIENT=ON ^
    -DBUILD_SERVER=ON

# Example with custom paths:
cmake .. ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_PREFIX_PATH="C:/Qt/6.7.0/msvc2022_64/lib/cmake" ^
    -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64"

# 3. Build Debug version
cmake --build . --config Debug

# 4. Build Release version
cmake --build . --config Release
```

### Method 3: Visual Studio IDE

1. Open Visual Studio 2022
2. File → Open → Folder
3. Select `MyRemote` project folder
4. Wait for CMake to configure
5. Build → Build All (or press Ctrl+Shift+B)

## Troubleshooting

### Issue: "fatal error LNK1181: cannot open input file 'ws2_32.lib'"

**Solution:** This library is part of Windows SDK and should be included by default.
Ensure "Desktop development with C++" workload is installed in Visual Studio.

### Issue: "Could NOT find Protobuf"

**Causes:**
- PROTOBUF_EXECUTABLE not found in PATH
- CMake can't locate Protobuf

**Solutions:**
```cmd
# Add to PATH
setx PATH "%PATH%;C:\protobuf\bin"

# Or specify in CMake command
cmake .. -DProtobuf_PROTOC_EXECUTABLE=C:/protobuf/bin/protoc.exe
```

### Issue: "Could NOT find Qt6"

**Cause:** Qt path not correctly specified or version mismatch

**Solutions:**
```cmd
# Find exact Qt installation path
dir C:\Qt /s *qt6*

# Specify during CMake generation
cmake .. -DCMAKE_PREFIX_PATH="C:\Qt\6.7.0\msvc2022_64"

# Verify Qt components exist
dir "C:\Qt\6.7.0\msvc2022_64\lib" | findstr Widgets
```

### Issue: "Could NOT find OpenSSL"

**Solution:**
```cmd
# Set explicit path
cmake .. -DOPENSSL_ROOT_DIR="C:/OpenSSL-Win64"

# Verify library exists
dir "C:/OpenSSL-Win64/lib/ssleay32.lib"
```

### Issue: Media Foundation APIs not found

**Cause:** Missing Windows SDK components

**Solution:** 
1. Reinstall Visual Studio
2. Ensure "Windows 10/11 SDK" component is checked
3. Restart IDE after installation

## Build Configuration Options

### Custom CMake Flags

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_CLIENT=ON \
    -DBUILD_SERVER=ON \
    -DCMAKE_INSTALL_PREFIX="C:/Program Files/MyRemote" \
    -DENABLE_LOGGING=ON
```

### Debug vs Release Builds

| Flag | Description |
|------|-------------|
| Debug | Full debug symbols, assertions enabled, slower execution |
| Release | Optimized for performance, no debug info |
| RelWithDebInfo | Optimized + debug symbols for profiling |
| MinSizeRel | Smallest possible binary size |

## Post-Build Setup

### Client Installation

1. Copy `agent.exe` from `build/bin/Release/` to target machine (the service
   registers whatever path the installer runs from, so copy it first)
2. To run as a Windows service, use the agent's own installer:
   ```cmd
   agent.exe --install-service
   agent.exe --service-state
   agent.exe --stop-service
   agent.exe --uninstall-service
   ```
   Do **not** use `sc create MyRemoteAgent binPath="...\agent.exe"`: without the
   `--service` argument the process cannot attach to the service control
   manager and the SCM kills it with error 1053. `--install-service` also sets
   delayed auto-start, failure actions, the `%ProgramData%\MyRemote` layout and
   removes the legacy logon task.

### Server Installation

1. Copy `control_server.exe` to desired location
2. Ensure Qt DLLs are available:
   - qt6core.dll
   - qt6widgets.dll  
   - qt6network.dll
   - Or deploy statically linked version

### Environment Variables (Optional)

Create `myremote_env.bat`:
```batch
@echo off
set PATH=%PATH%;C:\Qt\6.7.0\msvc2022_64\bin
set PATH=%PATH%;C:\protobuf\bin
set PATH=%PATH%;C:\OpenSSL-Win64\bin
cd /d C:\path\to\MyRemote\build\bin\Release
control_server.exe
```

## Continuous Integration

### GitHub Actions Example (`.github/workflows/build.yml`)

```yaml
name: Build

on: [push, pull_request]

jobs:
  build:
    runs-on: windows-latest
    
    steps:
    - uses: actions/checkout@v3
    
    - name: Install Qt
      uses: jurplel/install-qt-action@v3
      with:
        version: '6.7.0'
        cache: true
        
    - name: Configure CMake
      run: cmake -B ${{github.workspace}}/build
    
    - name: Build
      run: cmake --build ${{github.workspace}}/build --config Release
```

## Performance Tips

### Enable Link Time Code Generation (LTCG)

Add to CMakeLists.txt for better optimization:
```cmake
add_compile_options(/GL)
add_link_options(/LTCG)
```

### Enable SSE4.2 Instructions

Media Foundation encoding benefits from SSE4.2:
```cmake
target_compile_options(client_lib PRIVATE /arch:SSE4_2)
```

## Verification

After successful build, verify outputs:

```cmd
dir build\bin\Release\*.exe
# Should show: agent.exe, control_server.exe

file build\bin\Release\agent.exe
# Should show PE32 executable (console) x86-64

file build\bin\Release\control_server.exe
# Should show PE32+ executable (GUI) x86-64
```

## Known Issues

### Warning: "CMake Warning: no original contents for generated file"

**Ignore:** This is normal for Protocol Buffers generation

### Warning: "UNRESOLVED_SYMBOL" for D3D11 functions

**Solution:** Link against d3d11.lib:
Already configured in CMakeLists.txt under WIN32 section

## Support

For compilation issues:
1. Check prerequisites are properly installed
2. Review CMake output for specific error messages
3. Consult [DEVELOPMENT.md](../DEVELOPMENT.md) for code-level details
4. Open issue on GitHub repository
