# Copyright header

## New Project: Omaha files

Use this banner on **new fork-owned** source (for example under `code/uirender/`, `code/uidesign/`, or other Omaha-authored modules):

```cpp
/*
===========================================================================
Copyright (C) yyyy Project: Omaha

This file is part of Project: Omaha source code.

Project: Omaha builds upon OpenMoHAA / ioquake3 / F.A.K.K. foundations.
Project: Omaha source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Project: Omaha source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Project: Omaha source code; if not, see COPYING.txt in the
source tree, or write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
```

Replace `yyyy` with the current year.

## Inherited / mixed upstream files

Do **not** strip Id Software, OpenMoHAA, ioquake3, or third-party copyright banners.

When you modify an inherited file:

1. Leave the existing copyright / license block intact.
2. Annotate Omaha changes (`// Added|Changed|Fixed|Removed in OPM`).
3. Optionally **add** a Project: Omaha copyright line below the existing notice if substantial new authorship warrants it — do not replace the upstream notice.

If you are unsure whether a file is fork-only, treat it as inherited.
