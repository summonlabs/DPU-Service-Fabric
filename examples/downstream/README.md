# Downstream consumer

An independent CMake project that consumes the installed `DPUServiceFabric`
package with `find_package`. It is deliberately not part of the main build: it
configures, builds and runs against an installation prefix, which is what proves
the package is complete.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<install prefix>
cmake --build build
./build/dpuf_consumer
```

Set `CMAKE_BUILD_TYPE` to the configuration the library was installed with. This
is the usual requirement for a static library on MSVC, where a debug consumer
links against a debug C runtime and cannot mix with a release library; the
configuration is not silently inherited from the package.

The configure step fails if the exported target leaks compile options,
definitions or private include directories, so the repository's warning policy
cannot silently become the consumer's.
