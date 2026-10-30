# Block mapping data

`blocks-1.21.8.json` and `legacy.json` were obtained from the PrismarineJS
**minecraft-data** project on 2026-10-30:

- https://github.com/PrismarineJS/minecraft-data/blob/master/data/pc/1.21.8/blocks.json
- https://github.com/PrismarineJS/minecraft-data/blob/master/data/pc/common/legacy.json

The project's README declares the MIT license, and notes that some underlying
data originated from wiki.vg and minecraft.gamepedia.com:
https://github.com/PrismarineJS/minecraft-data#license

Copyright belongs to the minecraft-data contributors. No claim of authorship
is made over these data files. `source/legacy_blocks.h` is generated from them
by `tools/generate-legacy.mjs`; it is not edited manually.

Block state IDs and the sign block entity protocol ID (7) were also checked
against reports produced by the official Minecraft Java 1.21.8 server's
`net.minecraft.data.Main --reports` entry point. The official JAR and reports
are test artifacts under `tests/runtime` and are not part of the SD package.
