@echo off
setlocal
pushd "%~dp0"
python -m PyInstaller --noconfirm --windowed --onefile --noupx ^
    --distpath dist/desktop --name M365-BEC ^
    --icon assets/classic-envelope.ico --add-data "assets/classic-envelope.ico:assets" ^
    --exclude-module numpy --exclude-module pandas --exclude-module matplotlib ^
    --exclude-module PIL --exclude-module lxml analyzer.py
set "build_result=%errorlevel%"
popd
exit /b %build_result%
