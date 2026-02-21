# MinimalQtWindow

最小 Qt 单窗口 C++ 项目（CMake），兼容 Qt6 / Qt5。

## 构建与运行（Linux）

```bash
cd MinimalQtWindow
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/MinecraftPinger
```

![image](./image.png)

## 依赖

- CMake >= 3.16
- Qt Widgets（Qt6 或 Qt5）
- C++17 编译器
