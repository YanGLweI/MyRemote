; 两个向导共用的常量。
;
; 这里只放 #define，不放任何 section：Inno 一个脚本里 [Code] 只能出现一次，
; 把公共逻辑塞进 include 会让两个向导都没法再写自己的 [Code]。
; 版本号必须与 CMakeLists.txt 的 project(VERSION ...) 一致，stage.ps1 每次打包都会比对，
; 不一致就直接失败——装完发现"属性页一个版本、文件名另一个版本"是最难解释的那种事故。
#define MyAppVersion "1.0.5"
#define MyAppPublisher "MyRemote"
#define MyAppURL "https://github.com/YanGLweI/MyRemote"
#define MySetupIcon "..\client\resources\app.ico"
#define MyOutputDir "..\build\package\dist"
