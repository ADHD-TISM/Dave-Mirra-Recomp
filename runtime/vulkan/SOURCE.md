# Vulkan headers

Unmodified copies of the Khronos Vulkan headers, for the Vulkan backend (docs/renderer-roadmap.md, phase 4).
The program opens the Vulkan loader at run time, so these headers are the only Vulkan thing the build needs.

- Source: https://github.com/KhronosGroup/Vulkan-Headers, tag `v1.4.369` (`VK_HEADER_VERSION` 369), folder `include/vulkan`, plus `LICENSE.md`; the video-codec headers that `vulkan_core.h` includes are in `runtime/vulkan/vk_video` (folder `include/vk_video`, same tag; kept next to `vulkan_core.h` so its relative include works without an include path)
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

The files in `vk_video` (sha256sum, same fetch):

```
83bc45ed585e664ccd57f112ad0eaf2a83c12276bf5a30081ae9e742b8ffeb6c  vk_video/vulkan_video_codec_av1std.h
e37184b12c11fe5c4990133040158c0e5ecdbafc52285880276cddee6aecb8d5  vk_video/vulkan_video_codec_av1std_decode.h
c0388defe123f7e5fc02908d6ba868f58479a323da31d8e825b5fe2ac0523acb  vk_video/vulkan_video_codec_av1std_encode.h
072e863dd05e8151b9e0f5b18a4cb52acbe618079b4ace71e3b60cce1ec9ccbc  vk_video/vulkan_video_codec_h264std.h
e401c6aea14f2e5d33cc8215d6a6725a9a6659e266a4f0b371c91d2a4cbde505  vk_video/vulkan_video_codec_h264std_decode.h
30ef5fc7be25aec0fbebde8c4908d631690f2063d33c10ccea9edaa2ed0d728a  vk_video/vulkan_video_codec_h264std_encode.h
ba1668e412d3483d945fc3707e3b25b30c0c43ffd347d9b8b536a1cc622000a8  vk_video/vulkan_video_codec_h265std.h
2da3a4c697f9b0b75a11181880e082ab13553ed5d915782c841e01be4793dfb0  vk_video/vulkan_video_codec_h265std_decode.h
bbc4a232434a2268c3689a676a488a13bf1912bfa4ff9aeb3a54077905316bd1  vk_video/vulkan_video_codec_h265std_encode.h
1c33ed11eda1225ba52250221378930129b9c78c4aecc91bb368fa3d00f9cf36  vk_video/vulkan_video_codec_vp9std.h
37d5b7847a9dda12480ea6b27dfedb4c509eb61b91090b5b119a12bbb5c5b190  vk_video/vulkan_video_codec_vp9std_decode.h
3fc937e75411bf82fd8dfaa721b07b2bb53307c891a347cc010d4747ce28f971  vk_video/vulkan_video_codecs_common.h
```
