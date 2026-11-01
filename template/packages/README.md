# Packages

Every folder here is one package: an ESP-IDF project on Matter Builder's
`mb_core`, with a `package.yaml` next to its `CMakeLists.txt`.

The simplest start is a copy of Matter Builder's `firmware/generic` (at the
version in `MB_VERSION` of `.github/workflows/release.yml`): its
`CMakeLists.txt` already finds `mb_core` through `MB_COMPONENTS`. Rename the
project (`project(...)`), give it a version of its own (`PROJECT_VER` and
`PROJECT_VER_NUMBER`, equal to `CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER` in
`sdkconfig.defaults`), and write your code in `main/`.

`package.yaml`:

```yaml
name: my-package            # unique within this source
app: my_package             # the ESP-IDF project name (project(...))
kind: package
format: 1                   # the package description format
title: My package
description: One sentence on what it does.
requires: [bluetooth]       # what it needs from the board; [] when nothing
network: [thread]           # thread and/or wifi
```
