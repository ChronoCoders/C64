# Third-party components

This program's own source is under the MIT License (see `LICENSE`). It links one
third-party library at runtime:

## SDL2

Simple DirectMedia Layer 2 (https://www.libsdl.org/), used by the display, audio and
input host layer (`src/host.c`). SDL2 is distributed under the zlib license:

```
Simple DirectMedia Layer
Copyright (C) 1997-2024 Sam Lantinga <slouken@libsdl.org>

This software is provided 'as-is', without any express or implied
warranty.  In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

This binary release links SDL2 dynamically and does not include the SDL2 library
itself; install it through your system package manager (for example
`apt install libsdl2-2.0-0`).

The Commodore ROM images are not a component of this software and are not included;
see `ROMS.md`.
