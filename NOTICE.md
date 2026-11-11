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

`helper/src/SKSEMenuFramework.h` (ReSaverHelper's in-game page): the SKSE Menu Framework API header, unchanged, from
https://github.com/QTR-Modding/SKSE-Menu-Framework-3-Example - MIT License, Copyright (c) 2024 Thiago Kaique.

The script viewer's colours and keyword classes follow the npp-papyrus Notepad++ plugin
(https://github.com/blu3mania/npp-papyrus); no code from it is included.

Starfield saves (.sfs): the format was worked out from the game's own files and from Starfield.exe (its save
writing and loading functions), only so that ReSaver++ can read and write saves the game accepts
(interoperability). Nexus Mods' StarfieldSaveTool (https://github.com/Nexus-Mods/StarfieldSaveTool), made for the
Vortex Starfield extension, was used as a research reference for the save container, the header and the plugin
list; no code from it is included.

ReSaverHelper (the optional in-game plugins in `helper/`, `helper-le/`, `helper-f4/`) is separate from the
ReSaver++ program, which stays under the Apache License 2.0. Licenses of the helpers:
- `helper/` (Skyrim SE/AE/VR, ReSaverHelper.dll): statically linked with CommonLibSSE NG 6.7.0
  (https://github.com/alandtse/CommonLibSSE-NG), which is licensed GPL-3.0-or-later WITH Modding Exception
  AND GPL-3.0 Linking Exception (with Corresponding Source). So ReSaverHelper for Skyrim SE/AE/VR is licensed
  under the GNU General Public License version 3 or (at your option) any later version, with the same two
  exceptions: see `helper/LICENSE` (GPL-3.0 text) and `helper/EXCEPTIONS.md`. Its complete source is in `helper/`.
  The libraries CommonLibSSE NG uses (from vcpkg: fmt, spdlog, nlohmann-json, rapidcsv, simpleini, toml11, xbyak,
  DirectXMath, DirectXTK; the OpenVR SDK for VR) keep their own MIT / BSD licenses.
- `helper-le/` (Skyrim LE): built with SKSE 1.7.3's source (himika's libSKSE copy, `helper-le/extern/skse`, under
  SKSE's own terms in `extern/skse/skse_license.txt`); no CommonLib. Apache License 2.0 like ReSaver++.
- `helper-f4/` (Fallout 4): links no CommonLibF4, F4SE source or other library (only Windows): it declares the few
  F4SE plugin API structures it uses itself and uses Address Library ids. Apache License 2.0 like ReSaver++.
