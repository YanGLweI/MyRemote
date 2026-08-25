# MyRemote Control - 编译配置说明

## 最近更新

### 2024 - GitHub Actions CI/CD 修复

#### 主要变更：

1. **CMakeLists.txt 优化**
   - Qt6 检测改为条件化（仅 Server 需要）
   - Boost 和 Protobuf 设为可选依赖
   - OpenSSL 作为 Windows 必需依赖（用于 AES-GCM 加密）

2. **GitHub Actions 工作流**
   - 使用 MinGW Makefiles 生成器（替代 Visual Studio）
   - Chocolatey 安装 OpenSSL
   - 智能路径搜索和多位置支持
   - 修正输出路径到 `build/bin/Release/agent.exe`

3. **构建目标**
   - ✅ Client (`agent.exe`) - 启用
   - ❌ Server (`control_server.exe`) - 禁用（Qt 不可用）

#### 为什么使用 MinGW？

Windows Server 2025 runner 不预装 Visual Studio IDE。
唯一可用的 C++ 编译器是 MinGW GCC。

#### OpenSSL 安装位置搜索顺序：

1. `C:\ProgramData\chocolatey\lib\openssl`
2. `C:\Program Files\OpenSSL`
3. `C:\OpenSSL`
4. `C:\OpenSSL-Win64`
5. `C:\tools\openssl`

#### 本地构建命令：

```bash
# 安装依赖
choco install cmake mingw openssl -y

# 配置
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build --config Release
```

## 成功标准

- ✅ CMake 配置成功
- ⏳ Client 构建生成 `agent.exe` (在 `build/bin/Release/`)
- ❌ Server 构建暂时跳过
