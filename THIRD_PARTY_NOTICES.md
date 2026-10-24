# Third-party software

Muon.exe is statically linked with the libraries below. Their license texts are in the `licenses` folder.

| Library | Version | License | Source |
|---|---|---|---|
| [LibRaw](https://www.libraw.org) | 0.22.2 | CDDL 1.0 (chosen from the LibRaw dual license LGPL 2.1 / CDDL 1.0) | https://github.com/LibRaw/LibRaw/tree/0.22.2 |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9 | MIT | https://github.com/ocornut/imgui/tree/v1.91.9 |

LibRaw contains code derived from dcraw by Dave Coffin and the DCB / FBDD algorithms by Jacek Gozdz (BSD-style license), see `licenses/LibRaw-COPYRIGHT.txt`.
LibRaw is used unmodified; its complete source code for the version above is available at the link in the table.

Muon uses the Segoe UI fonts that come with Windows at run time; no fonts are bundled.
Everything else (Direct3D 11, Windows Imaging Component, the file dialogs) is part of Windows.
