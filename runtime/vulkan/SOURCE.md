# Vulkan headers

Unmodified copies of the Khronos Vulkan headers, for the Vulkan backend (docs/renderer-roadmap.md, phase 4).
The program opens the Vulkan loader at run time, so these headers are the only Vulkan thing the build needs.

- Source: https://github.com/KhronosGroup/Vulkan-Headers, tag `v1.4.369` (`VK_HEADER_VERSION` 369), folder `include/vulkan`, plus `LICENSE.md`
- Licence: Apache-2.0 OR MIT (see `LICENSE.md` and the SPDX line in each header)
- Fetched: 2026-11-20
- SHA-256 of the files as fetched (to update them, fetch the same files from a newer tag and replace this list):

```
95ad366d23fadf701d355bc45fb8b82ae2d700239471d35d41286ac3b08ff903  LICENSE.md
a2cd9085c66776845d2524c3b0e73ccf4827ba046aa6f676c4b8ddbed47d6552  vk_platform.h
ace769f4cd4c69a846c1778be95bfcd326038c8adc7630584ba33bcadd2f70a5  vulkan.h
a6948c33bbc0422092ad5d96fe9ea9d4109214fa00f26715b23ed1971f4e7216  vulkan_core.h
bfe3581571c702ea1c08e3338ff6a8393692610ed8a98e174a33f877c3ed674a  vulkan_wayland.h
68ffd3b0481ea8be18b5d183634d97924617ac23f8c465095ef40e1de300d272  vulkan_win32.h
779035831f296a4458b826781b2bf21160966a64abf9d56aa7f87cfcefa87bcd  vulkan_xcb.h
7cd469530d0b359d47b3512f08300bf62a272bf232a6d4db1c887c4bee890141  vulkan_xlib.h
```
