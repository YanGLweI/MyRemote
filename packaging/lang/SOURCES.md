# 向导语言文件来源

`ChineseSimplified.isl` 原样取自 Inno Setup 官方源码仓库，**一个字节都没改**，所以升级 Inno
版本时要一起换一个匹配的文件（.isl 里写着它要求的最低版本，太新的文件在旧 ISCC 上会直接报错）。

| 项 | 值 |
| --- | --- |
| 仓库 | `jrsoftware/issrc`（Inno Setup 官方源码） |
| 标签 | `is-6_7_3`（与 `winget install JRSoftware.InnoSetup` 当前给的 6.7.3 对齐） |
| 路径 | `Files/Languages/Unofficial/ChineseSimplified.isl` |
| 原始 URL | <https://raw.githubusercontent.com/jrsoftware/issrc/is-6_7_3/Files/Languages/Unofficial/ChineseSimplified.isl> |
| 大小 | 20905 字节 |
| SHA-256 | `7d544b9bb1d142cfa11f2e5d3cc8abe2e55f8e066c5124e3772675aa236e1278` |
| 许可 | Inno Setup License（<https://jrsoftware.org/files/is/license.txt>），该文件允许随安装包分发 |
| 译者 | 文件头自带署名与维护地址（Zhenghan Yang），未改动 |

放在 `Unofficial/` 目录里，意思是它**不随 Inno Setup 安装包一起发**，只存在于源码仓库；
所以本机装了 Inno 也不代表有这个文件，必须像上面那样取回来放进仓库，别人才能复现打包。
