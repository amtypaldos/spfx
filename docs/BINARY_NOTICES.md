# SP-1 FX firmware: binary notices

This file lists the third-party software contained in SP-1 FX firmware images
(`.bin`, `.hex`, `.elf`) built from this repository, with the licence texts and
copyright notices that must accompany those images. Ship it next to every
published image (for example as a release asset, or inside the release archive).

SP-1 FX is an unofficial project. It is not affiliated with or endorsed by
Teenage Engineering or Kano. The images contain no Bluetooth radio firmware, no
Infineon/Cypress SDK code, no part of the Tape Looper release image, and no code
from softmodded/marisko.

## Scope and how this list was made

- Image checked: the integrated firmware `zephyr_sp1fx`, release **v1-rc6**
  (`sp1fx-v1-rc6.bin`, SHA-256
  `daabe9b3961c0b30d38230891b71ae7db819ff83cb3924093f0fa181b2a51597`, git tag
  `v1-rc6`). A local reproducible build (`build_firmware.sh --app sp1fx repro`)
  gives the same SHA-256 as the published asset.
- Also applies to release **2026.11.25** (`sp1fx-2026.11.25.bin`, SHA-256
  `74656b23c8982a4955c55b2e6af9d747599f4f46358577a39c6f57ab11fa0214`, tag `v2026.11.25`): its linker map lists
  the same libraries (Zephyr, nrfx, picolibc, libgcc); the changes are in this project's own code.
- Method: every input object that contributes bytes to a loadable section was
  read from the linker map (`zephyr/zephyr.map`) of that build and traced to its
  source file with `compile_commands.json`. Headers compiled into those objects
  were taken from the build's dependency records (`ninja -t deps`). Licence and
  copyright lines were read from those files in the pinned checkout, and from the
  licence files shipped in Zephyr SDK 0.17.4.
- Only what is actually linked is listed. Other images in this repository (the
  bring-up apps) link a similar set from the same projects, but they were not
  checked one by one. Re-check the map when a new image links new drivers.

| Component | Version / pin | Licence | What is in the image |
|---|---|---|---|
| SP-1 FX (this repository) | tags `v1-rc6`, `v2026.11.25` | MIT | the application: DSP engine and effects, controls, protocol, storage, drivers |
| chattock/sp1-tape-looper | `0359372e11f61d74a85e2e4449178a5c62010664` | MIT | code adapted into `sp1_codecs.c`, `controls_hw.c`, `ladder.c`, `midi_out.c`, `sync_hw.c`; the SP-1 board definition (devicetree and Kconfig) |
| timknapen/SP-1-dev | `af2c50315996dfe5c78725995dbe498790ad763c` | MIT | CS42L42 init sequence (through Tape Looper) and pin map used by the board definition |
| ericlewis/sp1-midi | `43f5eae90d999a7defd9a20d06cd1bf4612fa235` | MIT | the board devicetree is reconstructed in part from its `stem_player` board (per the Tape Looper file header) |
| Zephyr RTOS | v4.3.1, `75f67d766726351b30199f9a2bf55803d717a3be` | Apache-2.0 | kernel, Cortex-M arch code, nRF drivers (clock, GPIO, I2C/TWIM, I2S, flash, watchdog, RTC timer, pinctrl, hwinfo, USB device controller), USB device stack (`device_next`, UAC2 and CDC ACM classes), `net_buf`, heap and utilities, `samples/subsys/usb/common/sample_usbd_init.c` |
| CMSIS_6 (Core headers) | `30a859f44ef8ab4dc8f84b03ed586fd16ccf9d74` | Apache-2.0 | Cortex-M4 core header inline functions only |
| hal_nordic: nrfx and MDK | `7858281d843468fe53c829995fb63f45a227387a` (nrfx 3.14.0) | BSD-3-Clause (four MDK `system_*` files: Apache-2.0) | nrfx drivers (clock, GPIOTE, I2S, NVMC, POWER, TWIM, WDT, flag allocator), `system_nrf52840.c`, HAL and MDK header inline code |
| picolibc | 1.8.10, as shipped in Zephyr SDK 0.17.4 | BSD-3-Clause variants, Sun/SunPro notice, Cygnus notice, Apache-2.0 OR BSL-1.0 (Ryu) | string functions, `snprintf`/`vsnprintf` (tinystdio, Ryu float printing), single-precision libm (`expf`, `logf`, `powf`, `tanf`, `floorf`, `ceilf`, `roundf`, `lrintf`, ...) |
| libgcc | GCC 12.2.0, Zephyr SDK 0.17.4 | GPL-3.0-or-later WITH GCC-exception-3.1 | 64-bit integer division and soft double-precision helpers |

The older `cmsis` module (`512cc7e8...`) is fetched by the build environment but
nothing from it is compiled into this image. No file under the Nordic 5-clause
licence (`LicenseRef-Nordic-5-Clause`) is linked.

---

## 1. SP-1 FX

```
MIT License

Copyright (c) 2026 Alex Typaldos

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 2. Tape Looper (chattock/sp1-tape-looper)

Source: https://github.com/chattock/sp1-tape-looper, commit
`0359372e11f61d74a85e2e4449178a5c62010664`. Adapted code and the unmodified board
definition; see `THIRD_PARTY_NOTICES.md` for the file-by-file list.

```
MIT License

Copyright (c) 2026 chattock

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 3. SP-1-dev (timknapen/SP-1-dev)

Source: https://github.com/timknapen/SP-1-dev, commit
`af2c50315996dfe5c78725995dbe498790ad763c` (licence file checked at this commit; the wiki
pages used are pinned in `THIRD_PARTY_NOTICES.md`).

```
MIT License

Copyright (c) 2026 Tim Knapen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 4. sp1-midi (ericlewis/sp1-midi)

Source: https://github.com/ericlewis/sp1-midi, commit
`43f5eae90d999a7defd9a20d06cd1bf4612fa235`.

```
MIT License

Copyright (c) 2026 Eric Lewis

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 5. Zephyr RTOS

Source: https://github.com/zephyrproject-rtos/zephyr, tag v4.3.1, commit
`75f67d766726351b30199f9a2bf55803d717a3be`. Licence: Apache License 2.0 (full text
in Appendix A). Zephyr v4.3.1 has no `NOTICE` file. The copyright lines of the
linked source files and the headers compiled into them are in Appendix B.

## 6. CMSIS_6

Source: https://github.com/zephyrproject-rtos/CMSIS_6, commit
`30a859f44ef8ab4dc8f84b03ed586fd16ccf9d74` (`CMSIS/Core/Include`, headers only).
Licence: Apache License 2.0 (Appendix A). No `NOTICE` file.

```
Copyright (c) 2009-2023 ARM Limited. All rights reserved.
Copyright (c) 2009-2023 Arm Limited. All rights reserved.
Copyright (c) 2009-2024 Arm Limited. All rights reserved.
Copyright (c) 2017-2020 Arm Limited. All rights reserved.
```

## 7. nrfx and nRF MDK (hal_nordic)

Source: https://github.com/zephyrproject-rtos/hal_nordic, commit
`7858281d843468fe53c829995fb63f45a227387a` (`nrfx/`, nrfx 3.14.0). Licence:
BSD-3-Clause. Copyright lines of the linked files and headers:

```
Copyright (c) 2010 - 2025, Nordic Semiconductor ASA All rights reserved.
Copyright (c) 2012 - 2025, Nordic Semiconductor ASA
Copyright (c) 2014 - 2025, Nordic Semiconductor ASA
Copyright (c) 2015 - 2025, Nordic Semiconductor ASA
Copyright (c) 2016 - 2025, Nordic Semiconductor ASA
Copyright (c) 2017 - 2025, Nordic Semiconductor ASA
Copyright (c) 2018 - 2025, Nordic Semiconductor ASA
Copyright (c) 2019 - 2025, Nordic Semiconductor ASA
Copyright (c) 2020 - 2025, Nordic Semiconductor ASA
Copyright (c) 2021 - 2025, Nordic Semiconductor ASA
Copyright (c) 2022 - 2025, Nordic Semiconductor ASA
```

The nrfx drivers use this wording:

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

The MDK files (`system_nrf52840.c`, `nrf.h`, `nrf52840.h`, `nrf52840_bitfields.h`,
`nrf_erratas.h`, `compiler_abstraction.h` and the other device headers) use the
same three clauses with "Nordic Semiconductor ASA" in clause 3:

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

3. Neither the name of Nordic Semiconductor ASA nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY, AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

Four MDK files are Apache-2.0 (Appendix A): `system_nrf52.c` (compiled through
`system_nrf52840.c`), `system_nrf52.h`, `system_nrf52840.h` and
`system_nrf52_approtect.h`, each with the line
"Copyright (c) 2009-2025 ARM Limited. All rights reserved."

## 8. picolibc (Zephyr SDK 0.17.4)

picolibc 1.8.10 (https://github.com/picolibc/picolibc), prebuilt in Zephyr SDK
0.17.4 (`arm-zephyr-eabi/picolibc`, `thumb/v7e-m+fp/hard`). The upstream licence
file is `arm-zephyr-eabi/share/licenses/picolibc/COPYING.picolibc` in the SDK. The
members linked into the image and the licence of each, from that file:

| Linked members (source file) | Copyright | Licence (as in COPYING.picolibc) |
|---|---|---|
| `newlib/libc/errno/errno.c`, `tinystdio/dtox_engine.c`, `tinystdio/filestrput.c` | 2020 Keith Packard; 2023 Keith Packard | BSD3-1 |
| `tinystdio/vfprintf.c` | 2002, Alexander Popov (sasho@vip.bg); 2002, 2004, 2005 Joerg Wunsch; 2005, Helmut Wallner; 2007, Dmitry Xmelkov | BSD3-13 |
| `tinystdio/snprintf.c`, `tinystdio/vsnprintf.c` | 2002, Joerg Wunsch; 2003, Joerg Wunsch | BSD3-13 |
| `tinystdio/dtoa_ryu.c`, `ryu_log10.c`, `ryu_pow5bits.c`, `ryu_table.c`, `ryu_umul128.c` | 2018 Ulf Adams | Apache-2.0 OR BSL-1.0 (used here under Apache-2.0, Appendix A) |
| `newlib/libc/string/memcmp.c`, `string/strncpy.c` | 1994 Cygnus Support | Cygnus notice |
| `newlib/libc/string/strnlen.c` | 2002 Jeff Johnston | Default-1 |
| `newlib/libc/machine/arm/memcpy.c`, `machine/arm/strlen.S` | 2015 ARM Ltd. | BSD3-9 |
| `newlib/libc/machine/arm/memset.c`, `machine/arm/strcmp.S` | 2015 ARM Ltd; 2012-2014 ARM Ltd | BSD3-8 |
| `newlib/libm/machine/arm/sf_ceil.c`, `sf_floor.c`, `sf_round.c` | 2011, 2012 ARM Ltd | BSD3-8 |
| `newlib/libm/common/math_errf_divzerof.c`, `math_errf_invalidf.c`, `math_errf_oflowf.c`, `math_errf_uflowf.c` | 2017-2018 Arm Ltd. | BSD3-30 |
| `newlib/libm/math/sf_exp.c`, `sf_log.c`, `sf_pow.c`, `sf_tan.c`, `kf_tan.c`, `sf_rem_pio2.c`, `kf_rem_pio2.c`; `newlib/libm/common/sf_isnan.c`, `sf_lrint.c`, `sf_scalbn.c` | 1993 Sun Microsystems, Inc. | SunPro notice |

The source file for each member is inferred from the member's object name in
`libc.a` (for example `libm_math_sf_exp.c.o` is `newlib/libm/math/sf_exp.c`); the
SDK ships binaries only.

Licence texts, verbatim from COPYING.picolibc:

#### BSD3-1

```
Copyright (c) 2020 Keith Packard
Copyright (c) 2023 Keith Packard

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided
with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### BSD3-13

```
Copyright (c) 2002, Alexander Popov (sasho@vip.bg)
Copyright (c) 2002,2004,2005 Joerg Wunsch
Copyright (c) 2005, Helmut Wallner
Copyright (c) 2007, Dmitry Xmelkov
Copyright (c) 2002, Joerg Wunsch
Copyright (c) 2003, Joerg Wunsch

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in
the documentation and/or other materials provided with the
distribution.

Neither the name of the copyright holders nor the names of
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

#### Other-23 (Ryu: Apache-2.0 OR BSL-1.0)

```
Copyright 2018 Ulf Adams

The contents of this file may be used under the terms of the Apache License,
Version 2.0.

See accompanying file LICENSE-Apache or copy at
http://www.apache.org/licenses/LICENSE-2.0)

Alternatively, the contents of this file may be used under the terms of
the Boost Software License, Version 1.0.
See accompanying file LICENSE-Boost or copy at
https://www.boost.org/LICENSE_1_0.txt)

Unless required by applicable law or agreed to in writing, this software
is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied.
```

#### Other-16 (Cygnus Support notice)

```
Copyright (c) 1994 Cygnus Support.

Redistribution and use in source and binary forms are permitted
provided that the above copyright notice and this paragraph are
duplicated in all such forms and that any documentation,
and/or other materials related to such
distribution and use acknowledge that the software was developed
at Cygnus Support, Inc.  Cygnus Support, Inc. may not be used to
endorse or promote products derived from this software without
specific prior written permission.
THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
```

#### Default-1

```
Copyright (c) 2002 Jeff Johnston <jjohnstn@redhat.com>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above
copyright notice, this list of conditions and the following
disclaimer in the documentation and/or other materials provided
with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### BSD3-9

```
Copyright (c) 2015 ARM Ltd.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
Neither the name of the Linaro nor the
names of its contributors may be used to endorse or promote products
derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### BSD3-8

```
Copyright (c) 2011, 2012 ARM Ltd
Copyright (c) 2012-2014 ARM Ltd
Copyright (c) 2015 ARM Ltd

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
3. The name of the company may not be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### BSD3-30

```
Copyright (c) 2017-2018 Arm Ltd.

SPDX-License-Identifier: BSD-3-Clause

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.
3. The name of the company may not be used to endorse or promote
products derived from this software without specific prior written
permission.

THIS SOFTWARE IS PROVIDED BY ARM LTD ``AS IS'' AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL ARM LTD BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

#### Other-25 (SunPro notice)

```
Copyright (c) 1993 Sun Microsystems, Inc.

Developed at SunPro, a Sun Microsystems, Inc. business.
Permission to use, copy, modify, and distribute this
software is freely granted, provided that this notice
is preserved.
```

## 9. libgcc (GCC runtime)

From GCC 12.2.0 as shipped in Zephyr SDK 0.17.4 (`_aeabi_ldivmod`,
`_aeabi_uldivmod`, `_udivmoddi4`, `_dvmd_tls`, `_arm_addsubdf3`, `_arm_muldivdf3`,
`_arm_cmpdf2`, `_arm_truncdfsf2`). Licence: GNU GPL version 3 or later with the
GCC Runtime Library Exception, version 3.1 (texts in the SDK at
`arm-zephyr-eabi/share/licenses/gcc/COPYING3` and `COPYING.RUNTIME`). The image is
produced by an Eligible Compilation Process with GCC, so the exception permits
distributing it under terms of our choice; no notice is required. It is listed
here for completeness.

---

## Appendix A: Apache License, Version 2.0

```
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "{}"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright {yyyy} {name of copyright owner}

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

## Appendix B: Zephyr copyright lines

Copyright lines from the Zephyr v4.3.1 source files linked into the image and the
Zephyr headers compiled into them (de-duplicated). All of these files are
`SPDX-License-Identifier: Apache-2.0`.

```
Copyright (c) 1997-2014 Wind River Systems, Inc.
Copyright (c) 1997-2015, Wind River Systems, Inc.
Copyright (c) 2010-2011 Dmitry Vyukov
Copyright (c) 2010-2012, 2014 Wind River Systems, Inc.
Copyright (c) 2010-2012, 2014-2015 Wind River Systems, Inc.
Copyright (c) 2010-2014 Wind River Systems, Inc.
Copyright (c) 2010-2014, Wind River Systems, Inc.
Copyright (c) 2010-2014,2017 Wind River Systems, Inc.
Copyright (c) 2010-2016 Wind River Systems, Inc.
Copyright (c) 2010, 2012, 2014 Wind River Systems, Inc.
Copyright (c) 2011-2014 Wind River Systems, Inc.
Copyright (c) 2011-2014, Wind River Systems, Inc.
Copyright (c) 2012-2014 Wind River Systems, Inc.
Copyright (c) 2013-2014 Wind River Systems, Inc.
Copyright (c) 2013-2014, Wind River Systems, Inc.
Copyright (c) 2013-2015 Wind River Systems, Inc.
Copyright (c) 2013-2016 Wind River Systems, Inc.
Copyright (c) 2014 Wind River Systems, Inc.
Copyright (c) 2014-2015 Wind River Systems, Inc.
Copyright (c) 2014, Wind River Systems, Inc.
Copyright (c) 2015 Intel corporation
Copyright (c) 2015 Intel Corporation
Copyright (c) 2015 Intel Corporation.
Copyright (c) 2015 Wind River Systems, Inc.
Copyright (c) 2015-2016 Intel Corporation.
Copyright (c) 2015-2016, Intel Corporation.
Copyright (c) 2015-2019 Intel Corporation
Copyright (c) 2015, Wind River Systems, Inc.
Copyright (c) 2016 - 2023, Nordic Semiconductor ASA
Copyright (c) 2016 ARM Ltd.
Copyright (c) 2016 Intel Corporation
Copyright (c) 2016 Intel Corporation.
Copyright (c) 2016 Linaro Limited
Copyright (c) 2016 Nordic Semiconductor ASA
Copyright (c) 2016 Vinayak Kariappa Chettimada
Copyright (c) 2016 Wind River Systems, Inc.
Copyright (c) 2016-2017 Wind River Systems, Inc.
Copyright (c) 2016-2020 Nordic Semiconductor ASA
Copyright (c) 2016-2021 Nordic Semiconductor ASA
Copyright (c) 2016, Wind River Systems, Inc.
Copyright (c) 2017 ARM Ltd
Copyright (c) 2017 Intel Corporation
Copyright (c) 2017 Intel Corporation.
Copyright (c) 2017 Linaro Limited
Copyright (c) 2017 Linaro Limited.
Copyright (c) 2017 Nordic Semiconductor ASA
Copyright (c) 2017 PHYTEC Messtechnik GmbH
Copyright (c) 2017 Piotr Mienkowski
Copyright (c) 2017-2019 Nordic Semiconductor ASA.
Copyright (c) 2017-2024 Nordic Semiconductor ASA
Copyright (c) 2017, 2018 Intel Corporation
Copyright (c) 2017, Intel Corporation
Copyright (c) 2017, Nordic Semiconductor ASA
Copyright (c) 2017, Oticon A/S
Copyright (c) 2018 Alexander Wachter
Copyright (c) 2018 Intel Corporation
Copyright (c) 2018 Intel Corporation.
Copyright (c) 2018 Linaro Limited
Copyright (c) 2018 Linaro Limited.
Copyright (c) 2018 Nordic Semiconductor ASA
Copyright (c) 2018 Nordic Semiconductor ASA.
Copyright (c) 2018-2019 Nordic Semiconductor ASA
Copyright (c) 2018-2020 Intel Corporation
Copyright (c) 2018, 2024 Intel Corporation
Copyright (c) 2018, Intel Corporation.
Copyright (c) 2018, Nordic Semiconductor ASA
Copyright (c) 2019 BayLibre SAS
Copyright (c) 2019 Carlo Caione <ccaione@baylibre.com>
Copyright (c) 2019 Facebook.
Copyright (c) 2019 Intel Corporation
Copyright (c) 2019 Intel Corporation.
Copyright (c) 2019 Lexmark International, Inc.
Copyright (c) 2019 Nordic Semiconductor ASA
Copyright (c) 2019 Nordic Semiconductor ASA.
Copyright (c) 2019 Peter Bigot Consulting, LLC
Copyright (c) 2019 Piotr Mienkowski
Copyright (c) 2019 Stephanos Ioannidis <root@stephanos.io>
Copyright (c) 2019-2020 Nordic Semiconductor ASA
Copyright (c) 2019, Nordic Semiconductor ASA
Copyright (c) 2020 Gerson Fernando Budke <nandojve@gmail.com>
Copyright (c) 2020 Intel corporation
Copyright (c) 2020 Intel Corporation
Copyright (c) 2020 Intel Corporation.
Copyright (c) 2020 Nordic Semiconductor
Copyright (c) 2020 Nordic Semiconductor ASA
Copyright (c) 2020 Nordic Semiconductor ASA.
Copyright (c) 2020 PHYTEC Messtechnik GmbH
Copyright (c) 2020 Stephanos Ioannidis <root@stephanos.io>
Copyright (C) 2020, Intel Corporation
Copyright (c) 2020, Linaro Ltd.
Copyright (c) 2020, Nordic Semiconductor ASA
Copyright (c) 2020, Synopsys
Copyright (c) 2020, Wind River Systems, Inc.
Copyright (c) 2021 Intel Corporation
Copyright (c) 2021 Lexmark International, Inc.
Copyright (c) 2021 Nordic Semiconductor ASA
Copyright (c) 2021-2022 Nordic Semiconductor ASA
Copyright (c) 2021, Commonwealth Scientific and Industrial Research Organisation (CSIRO) ABN 41 687 119 230.
Copyright (c) 2021, Nordic Semiconductor ASA
Copyright (c) 2021,2023, Intel Corporation
Copyright (c) 2022 Carlo Caione <ccaione@baylibre.com>
Copyright (c) 2022 Emerson Electric Co.
Copyright (c) 2022 Google LLC
Copyright (c) 2022 Intel Corporation
Copyright (c) 2022 Nordic Semiconductor ASA
Copyright (c) 2022 Vestas Wind Systems A/S
Copyright (c) 2022, Andrei-Edward Popa
Copyright (c) 2023 Arm Limited
Copyright (c) 2023 Arm Limited (or its affiliates). All rights reserved.
Copyright (c) 2023 Carlo Caione <ccaione@baylibre.com>
Copyright (c) 2023 Carlo Caione, <ccaione@baylibre.com>
Copyright (c) 2023 CSIRO.
Copyright (c) 2023 Intel Corporation
Copyright (c) 2023 Meta
Copyright (c) 2023 Nordic Semiconductor ASA
Copyright (c) 2023 Nordic Semiconductor ASA.
Copyright (c) 2023-2024 Nordic Semiconductor ASA
Copyright (c) 2023, 2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
Copyright (c) 2023, Intel Corporation
Copyright (c) 2023, Meta
Copyright (C) 2023, Nordic Semiconductor ASA
Copyright (c) 2024 Intel Corporation
Copyright (c) 2024 Meta Platforms
Copyright (c) 2024 Nordic Semiconductor ASA
Copyright (c) 2024 tinyVision.ai Inc
Copyright (c) 2024, Croxel Inc
Copyright (c) 2024, Embeint Inc
Copyright (c) 2024, Nordic Semiconductor ASA
Copyright (c) 2025 Abderrahmane JARMOUNI
Copyright (c) 2025 Google LLC
Copyright (c) 2025 Nordic Semiconductor ASA
Copyright (c) 2025 Sequans Communications
Copyright (c) 2025 Siemens AG
Copyright (c) 2025 Tenstorrent AI ULC
Copyright (C) 2025, Nordic Semiconductor ASA
Copyright (c) BayLibre SAS
Copyright 2021 Google LLC
Copyright 2022 IoT.bzh
Copyright 2023, 2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
Copyright 2023,2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
Copyright 2024 NXP
Copyright 2025 Arm Limited and/or its affiliates <open-source-office@arm.com>
Copyright 2025 NXP
Copyright The Zephyr Project Contributors
```
