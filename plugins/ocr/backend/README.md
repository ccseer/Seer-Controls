# Backend Dependencies

> [!NOTE]
> **Avoid Repository Bloat**: Large dynamic runtime libraries (`*.dll`) are distributed outside Git. You can obtain all required runtime DLLs directly by extracting them from the official downloaded plugin ZIP package.

Place private dynamic runtime libraries in this directory:

- `onnxruntime.dll`: ONNX Runtime 1.22.1 DirectML build (win-x64)
- `DirectML.dll`: DirectML library corresponding to the NuGet package
- `vcruntime140.dll`
- `vcruntime140_1.dll`
- `msvcp140.dll`

Note: `SeerOcrWorker.exe` dynamically loads `onnxruntime.dll` via `LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)` so `DirectML.dll` and app-local CRT DLLs are isolated and resolved directly from this directory.
