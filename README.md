# Compás Player

Executable sources for the Compás player and bootloader on HiBy R1, R3 Pro II and R3 II 2025.

## Build

Install Git, Make, a C/C++ compiler, CMake, Python 3, patch, curl, xz and pkg-config. The optional host player also needs SDL2 development headers. Put the MIPS musl cross-toolchain on `PATH`; the verified installer can prepare it:

```sh
toolchain_dir=$(scripts/tools/toolchain/install_mips_toolchain.sh "$HOME/.cache/compas-toolchain")
export PATH="$toolchain_dir/bin:$PATH"
make -j6 target BOARD=r1
make bootloader BOARD=r1
```

Dependencies are fetched automatically when absent. The R1 and R3 Pro II bootloader builds also fetch a pinned Rockbox revision; first builds require network access.

| Board | Player | Bootloader |
| --- | --- | --- |
| `r1` (default) | `compas_player_target` | `compas_loader` |
| `r3proii` | `compas_player_target_r3proii` | `compas_loader_r3proii` |
| `r3ii_2025` | `compas_player_target_r3ii_2025` | `compas_loader_r3ii_2025` |

Set `BOARD` on each command. R1 and R3 Pro II use the combined Compás/Rockbox selector; R3 II 2025 uses the standalone loader. `make clean` removes build outputs.

Generated embedded graphics, fonts and translations remain in the compiled sources. Loose runtime themes, fonts and other firmware resources are supplied by the installed firmware; this repository does not package them or produce firmware images. Building these executables does not install or flash them.

The [website repository](https://github.com/Starnished66/compas-page/tree/docs-from-player) holds the user manual and screenshots. Development tools, tests and original resource inputs remain available in Git history before this executable-only cleanup.

## License

See [LICENSE](LICENSE), the notices under `licenses/`, and the license files in the retained dependency sources.
