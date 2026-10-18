@echo off
chcp 65001 >nul
setlocal

set HERE=%~dp0
set TARGT=%~dp0..\cr-device-info.tgz

echo === check device ===
adb devices
echo.

echo === push helper ===
adb push "%HERE%grab-device-info.sh" /data/local/tmp/grab-device-info.sh
if errorlevel 1 goto fail

echo.
echo === run on device ===
adb shell su -c "cp -f /data/local/tmp/grab-device-info.sh /sdcard/grab-device-info.sh; chmod 0755 /sdcard/grab-device-info.sh"
adb shell su -c "sh /sdcard/grab-device-info.sh"

echo.
echo === pull ===
if exist "%TARGT%" del /q "%TARGT%"
adb pull /sdcard/cr-device-info.tgz "%TARGT%"
if errorlevel 1 goto fail

echo.
echo === done ===
echo package: %TARGT%
echo.
echo On Linux:
echo   tar xzf cr-device-info.tgz
echo   ./tools/adapt-new-device.sh "$(cat cr-device-info/uname_r.txt)" cr-device-info/vmlinux.btf
echo.
goto end

:fail
echo.
echo FAILED - check adb connection and root permission

:end
pause
