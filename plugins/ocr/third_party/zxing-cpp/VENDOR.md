# Vendored ZXing-C++

The upstream source tree of [zxing-cpp](https://github.com/zxing-cpp/zxing-cpp)
is vendored at tag **v2.3.0**, with one known deviation described below:

- Version: 2.3.0 (commit `d6068bcebeb8fd9f0d35a99b00d202be86a14dbe`)
- License: Apache-2.0 (see `LICENSE` upstream)
- Vendored: 2026-10-20, via the official `v2.3.0` source tarball

## Local deviation from upstream

`core/src/libzint/*` (60 entries) are **empty files, not source**: upstream
stores those paths as symlinks into the `zint` submodule declared in
`.gitmodules`, and this checkout carries no submodule content
(`core.symlinks=false`), so the vendoring step left empty placeholders.
Everything else the readers build uses is the upstream v2.3.0 tree as-is,
`core/src/libzint/stubs.c` included.

Nothing reads the placeholders in this build: `ZXING_WRITERS=OFF` skips the
`if (ZXING_WRITERS_NEW)` block that is the only caller of
`aux_source_directory(src/libzint ...)`, so `ZXing.lib` is built from the
reader sources only. Re-enabling the writer backend on these placeholders
would link encoders that produce nothing instead of failing the build, so a
re-vendor has to restore real libzint content or keep the writers off.

The plugin consumes it as a CMake subdirectory (`add_subdirectory` with
`EXCLUDE_FROM_ALL` in the plugin's `CMakeLists.txt`, so the vendored install
rules stay out of the package prefix) and builds the `ZXing` static library
with readers only: `ZXING_WRITERS`, `ZXING_EXAMPLES`, `ZXING_C_API`,
`ZXING_EXPERIMENTAL_API`, `ZXING_BLACKBOX_TESTS`, `ZXING_UNIT_TESTS` and
`ZXING_PYTHON_MODULE` are all disabled. The library links statically into
`SeerOcrWorker.exe` with the statically linked VC++ runtime, so no additional
DLL ships with the package.

When upgrading, re-vendor the new tag in full and re-check:

- the option list above still exists in the upstream root `CMakeLists.txt`
- the reader API used by `src/barcode.cpp`
  (`ZXing::ImageView`, `ZXing::ReaderOptions`, `ZXing::ReadBarcodes`,
  `ZXing::ToString(BarcodeFormat)`, `Result::position()`)
- the C++17 floor: upstream may raise its default standard
- `Content::text()` still returns valid UTF-8. `src/output_writer.cpp` escapes
  barcode payloads byte by byte and never validates them, so the JSON "text"
  field is only RFC 8259 compliant while upstream keeps routing every payload
  through `TextDecoder::Append` (zueci, U+FFFD replacement)
