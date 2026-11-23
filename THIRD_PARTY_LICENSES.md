# Third-Party Licenses & Attributions

**INFRA VR** builds upon several foundational open-source projects and SDKs. Their attributions and full license texts are listed below.

---

## 1. Upstream VR Projects & Inspirations

* **[Portal 2 VR (`portal2vr`)](https://github.com/Gistix/portal2vr)** by **Gistix** (with contributions from **fholger** and **vittorioromeo**) — Provided the direct architectural foundation for the Source Engine VR hook layer and DXVK D3D9-to-Vulkan stereo submission bridge. *(No standalone root license file in upstream repository; embedded DXVK components carry the zlib/libpng license and embedded Source SDK files carry the Source 1 SDK License.)*
* **[Left 4 Dead 2 VR (`l4d2vr`)](https://github.com/sd805/l4d2vr)** by **sd805** — Original Source Engine OpenVR mod (`L4D2VR`) upon which `portal2vr` was built. *(No standalone root license file in upstream repository; embedded DXVK components carry the zlib/libpng license and embedded Source SDK files carry the Source 1 SDK License.)*
* **[VirtualFortress2](https://github.com/PinkMilkProductions/VirtualFortress2)** by **PinkMilkProductions** & **[gmcl_openvr](https://github.com/Planimeter/gmcl_openvr/)** by **Planimeter** — Early Source Engine OpenVR implementation references credited by upstream.

---

## 2. DXVK (`dxvk/`) — zlib/libpng License

* **Upstream Repository:** [doitsujin/dxvk](https://github.com/doitsujin/dxvk)
* **VR Forks:** [TheIronWolfModding/dxvk](https://github.com/TheIronWolfModding/dxvk), [DrBeef/dxvk](https://github.com/DrBeef/dxvk), [fholger/dxvk](https://github.com/fholger/dxvk), [sd805/dxvk_l4d2vr](https://github.com/sd805/dxvk_l4d2vr)

```text
Copyright (c) 2017 Philip Rebohle
Copyright (c) 2019 Joshua Ashton

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
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

---

## 3. Valve OpenVR SDK (`thirdparty/openvr/`) — BSD 3-Clause License

* **Upstream Repository:** [ValveSoftware/openvr](https://github.com/ValveSoftware/openvr)

```text
Copyright (c) 2015, Valve Corporation
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

---

## 4. MinHook & Hacker Disassembler Engine (`thirdparty/minhook/`) — BSD 2-Clause License

* **Upstream Repository:** [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)

```text
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.
```

---

## 5. Valve Source 1 SDK (`InfraVR/sdk/`) — Source 1 SDK License (Non-Commercial)

* **Upstream Repository:** [ValveSoftware/source-sdk-2013](https://github.com/ValveSoftware/source-sdk-2013)
* **Copyright:** Copyright (c) Valve Corporation. All rights reserved.
* Distributed free of charge (non-commercial use only). See [`InfraVR/sdk/LICENSE.txt`](InfraVR/sdk/LICENSE.txt) and [`InfraVR/sdk/thirdpartylegalnotices.txt`](InfraVR/sdk/thirdpartylegalnotices.txt) for full license terms and required third-party notices.

---

## 6. Adobe Source Code Pro (`dxvk/src/dxvk/hud/dxvk_hud_font.cpp`) — SIL Open Font License 1.1

* **Upstream Repository:** [adobe-fonts/source-code-pro](https://github.com/adobe-fonts/source-code-pro)
* **Notice:** Embedded HUD glyph bitmap and font metrics generated from *Source Code Pro* by Adobe Systems Inc. (Copyright 2010–2022 Adobe), licensed under the **SIL Open Font License Version 1.1 (OFL-1.1)** (<https://scripts.sil.org/OFL>). Permission is granted to use, study, copy, merge, embed, modify, redistribute, and sell modified and unmodified copies of the Font Software, provided it is not sold by itself and all copies carry this copyright notice and the OFL license text.

---

## 7. Khronos Vulkan & SPIR-V Headers (`dxvk/include/vulkan/`, `dxvk/include/spirv/`)

* **Vulkan Headers (`dxvk/include/vulkan/`):** Copyright 2014–2021 The Khronos Group Inc. — **Apache License, Version 2.0** (`SPDX-License-Identifier: Apache-2.0`).
* **SPIR-V Headers (`dxvk/include/spirv/spirv.hpp`):**
```text
Copyright (c) 2014-2019 The Khronos Group Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and/or associated documentation files (the "Materials"),
to deal in the Materials without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Materials, and to permit persons to whom the
Materials are furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Materials.

MODIFICATIONS TO THIS FILE MAY MEAN IT NO LONGER ACCURATELY REFLECTS KHRONOS
STANDARDS. THE UNMODIFIED, NORMATIVE VERSIONS OF KHRONOS SPECIFICATIONS AND
HEADER INFORMATION ARE LOCATED AT https://www.khronos.org/registry/

THE MATERIALS ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM,OUT OF OR IN CONNECTION WITH THE MATERIALS OR THE USE OR OTHER DEALINGS
IN THE MATERIALS.
```

---

## 8. SHA-1 in C (`dxvk/src/util/sha1/`) — Public Domain

* **Author:** Steve Reid `<steve@edmweb.com>`
* **License:** 100% Public Domain.

---

## 9. Valve Steam Audio (`thirdparty/steamaudio/`, `dist/bin/phonon.dll`) — Apache License 2.0

* **Upstream Repository:** [ValveSoftware/steam-audio](https://github.com/ValveSoftware/steam-audio)
* **Version:** 4.8.1
* **Copyright:** Copyright 2017–2023 Valve Corporation.
* **License:** **Apache License, Version 2.0** (<https://www.apache.org/licenses/LICENSE-2.0>).

---

## 10. DSOAL & OpenAL Soft (Optional `dist/dsoal/`) — GNU LGPL v2.0+

* **Upstream Repositories:** [kcat/dsoal](https://github.com/kcat/dsoal) & [kcat/openal-soft](https://github.com/kcat/openal-soft)
* **Notice:** Optional drop-in `dsound.dll` / `dsoal-aldrv.dll` binaries used only when `SpatialAudioMode=dsoal` is selected, distributed as dynamically linked DLLs under the terms of the **GNU Lesser General Public License (LGPL-2.0 or later)**. Complete source code is available at the upstream repositories above.
