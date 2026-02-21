# MinecraftPingerGtk (GTK4 + libadwaita)

这是现有 Qt 版本的 GTK4 + libadwaita 复刻版（放在 `gtk/` 子目录，不影响 Qt 工程）。

## 依赖

Fedora：

- `sudo dnf install gtk4-devel libadwaita-devel json-glib-devel gdk-pixbuf2-devel pkgconf-pkg-config`

Ubuntu/Debian（包名可能略有差异）：

- `sudo apt install libgtk-4-dev libadwaita-1-dev libjson-glib-dev libgdk-pixbuf-2.0-dev pkg-config`

## 构建与运行

在仓库根目录执行：

- `cmake -S gtk -B gtk/build`
- `cmake --build gtk/build -j`
- `./gtk/build/MinecraftPingerGtk`

