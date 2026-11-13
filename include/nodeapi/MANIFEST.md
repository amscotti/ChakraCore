# Node-API vendored headers — manifest

Source release: **Node 22.22.3** (`node_version.h`:
`NODE_MAJOR_VERSION 22`, `NODE_MINOR_VERSION 22`,
`NODE_PATCH_VERSION 3`, `NODE_MODULE_VERSION 127`).

Source directory at vendor time:
`/Users/ascotti/.local/share/mise/installs/node/22.22.3/include/node/`.
Files below were copied verbatim; no edits were applied to the copied
bytes. Internal `#include "js_native_api_types.h"` etc. references need
no adjustment: all four files live in this directory and include each
other by bare quoted filename. License headers in the source files were
preserved by the verbatim copy.

## Files

| File | SHA-256 | Declarations at `NAPI_VERSION=8` (see generator) |
|---|---|---|
| `js_native_api.h` | `582e64ead4d5fdf663e2bd934f2fc97e8e155a7740d28995830393d19d089705` | engine surface (Table A) |
| `js_native_api_types.h` | `05e3d86e24c814f94a8b52b6a70cec11d7e91c810008253553528637e3a15b2f` | types only (no `NAPI_EXTERN` declarations) |
| `node_api.h` | `2d4560831e525b47b060ec8a0864ab73993df9e20215ce9f0fe7b24cd31af32a` | host surface (Table B) |
| `node_api_types.h` | `088766b3cf1f0cd9b292c361ad30f7a04e7cc6db35a20ca52c572c1adf9635e7` | types only (no `NAPI_EXTERN` declarations) |

Verify with:

```sh
shasum -a 256 include/nodeapi/*.h
```

## `NAPI_VERSION=8` definition point

`js_native_api_types.h:6-17`:

```c
#ifndef NAPI_VERSION
#ifdef NAPI_EXPERIMENTAL
#define NAPI_VERSION NAPI_VERSION_EXPERIMENTAL
#else
#define NAPI_VERSION 8
#endif
#endif
```

The baseline version defaults to **8** when undefined. Per plan PR-2,
provider code and test addons must define `NAPI_VERSION=8` explicitly
(e.g. `-DNAPI_VERSION=8`) rather than relying on the default, and must
not define `NAPI_EXPERIMENTAL`.

`NODE_API_NO_EXTERNAL_BUFFERS_ALLOWED` must not be defined by provider
or test code: defining it would hide
`napi_create_external_arraybuffer` (`js_native_api.h`) and
`napi_create_external_buffer` (`node_api.h`).

## License

Node.js headers are licensed under the MIT License (see Node 22.22.3
`LICENSE` at the install root; the relevant notice for these headers
is reproduced below). The vendored copies retain that notice by
reference to this manifest; no license text was removed from the
copied files.

> Node.js is licensed for use as follows:
>
> """
> Copyright Node.js contributors. All rights reserved.
>
> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to
> deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
> sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
> IN THE SOFTWARE.
> """

## Not vendored (PR-2)

`<v8.h>`, `<node.h>`, `<node_buffer.h>`, `uv.h` were **not** copied.
Only the four files listed above are vendored. Addons including those
headers are out of scope (plan §0.2).

## Regenerating the inventory

```sh
python3 tools/nodeapi/gen_inventory.py
python3 tools/nodeapi/gen_inventory.py --check
```

`--check` verifies this manifest's SHA-256 values against the headers
on disk and that `doc/node-api-inventory.md` matches a fresh
regeneration; it exits 1 otherwise.
