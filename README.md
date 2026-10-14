# TidyTray

Windows 11 通知区整理工具。

## 依赖

- CMake ≥ 3.20
- MinGW-w64（`g++`、`make`）

## 构建

```bat
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cd build
make
```

或 Ninja：

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cd build
ninja
```

产物：`build\TidyTray.exe`
