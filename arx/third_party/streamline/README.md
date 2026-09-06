# NVIDIA Streamline SDK (not in git)

Official SDK: https://github.com/NVIDIA-RTX/Streamline (v2.12.0).

Binaries are **not** in the GitHub source tree. Fetch them once:

```
.\scripts\fetch-streamline.ps1
```

That writes `include/`, `lib/x64/`, and `bin/x64/` here. CMake then sets `ARX_HAVE_STREAMLINE` and copies the DLLs next to `arx.exe`.

Do not commit the zip or the DLLs.
