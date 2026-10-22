# ReSaver++

ReSaver++ is a C++ port of **ReSaver**, part of FallrimTools by **Mark Fairchild**
(https://github.com/mdfairch/FallrimTools), licensed under the Apache License 2.0.
The save-format knowledge and the cleaning rules come from ReSaver; this port re-implements
them in C++. ReSaver++ is distributed under the same Apache License 2.0 (see LICENSE).

Third-party code in `third_party/`:
- Dear ImGui - MIT License (Omar Cornut)
- GLFW - zlib/libpng License
- LZ4 - BSD 2-Clause License (Yann Collet)
- miniz - MIT License (Rich Geldreich, Tenacious Software)
- ImGuiColorTextEdit - MIT License (BalazsJako), modified for the script viewer

The script viewer's colours and keyword classes follow the npp-papyrus Notepad++ plugin
(https://github.com/blu3mania/npp-papyrus); no code from it is included.
