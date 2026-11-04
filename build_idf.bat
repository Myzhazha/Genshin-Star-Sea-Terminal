@echo off
chcp 936 >nul
set MSYSTEM=
set PYTHONUTF8=1
call "E:\Espressif\frameworks\esp-idf-v5.5.5\export.bat" >nul 2>&1
cd /d "%~dp0"
idf.py set-target esp32p4
idf.py build
if errorlevel 1 goto :eof
if not exist "固件" mkdir "固件"
copy /Y "build\bootloader\bootloader.bin" "固件\bootloader_0x2000.bin" >nul
copy /Y "build\partition_table\partition-table.bin" "固件\partition-table_0x8000.bin" >nul
copy /Y "build\esp32p4_st7102_genshin_ui.bin" "固件\esp32p4_st7102_genshin_ui_0x10000.bin" >nul
if exist "固件\esp32p4_st7102_genshin_ui_0x10000.bin" echo [OK] firmware synced
