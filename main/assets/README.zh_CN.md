<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 生成的小厨宝封面数据块

`covers_large.bin` 与 `covers_small.bin` 是离线菜谱应用的生成式构建输入。它们把
每道「有实拍图」的菜的封面按基线 JPEG 顺序拼接成一个文件，再由
`../CMakeLists.txt` 里的 `EMBED_FILES` 嵌入 Flash。

每张封面的偏移与长度记录在生成的表 `../generated/cook_covers.c` 中，菜谱到封面
下标的关系记录在 `../generated/cook_data.c` 中。只有二进制块本身无法定位某张封面。

## 为什么用 JPEG

ESP32-C3 没有 PSRAM。LVGL 内置的 tjpgd 解码器按 MCU 行流式输出，解码只需约 4KB
工作缓冲，而不是一整帧解码缓冲。从内存数据块解码要走 `LV_USE_FS_MEMFS`，它把单个
`lv_image_dsc_t` 指向的内存区间包装成可读文件流。这两个选项都在
`../sdkconfig.defaults` 里打开。

两档尺寸在打包阶段烘焙，而不是运行时缩放：

| 数据块 | 尺寸 | 使用位置 |
| --- | --- | --- |
| `covers_large.bin` | 200 x 150 | 随机推荐页 |
| `covers_small.bin` | 144 x 108 | 菜谱详情页 |

运行时缩放需要把一整帧解码结果放进 RAM，放不下。

## 重新生成

```bash
python3 tools/build_cook_assets.py     # 重写两个数据块与相关数据表
python3 tests/test_cook_assets.py      # 校验 JFIF 头与表边界
idf.py build
```

每一项都必须以 JFIF APP0 签名 `FF D8 FF E0 00 10 4A 46 49 46` 开头。LVGL 的
`is_jpg` 判定要求的正是这个前缀；换一个编码器写出的 JPEG 即使本身合法也可能被
拒收。`tests/test_cook_assets.py` 会强制这一点。

## 来源与许可状态

- 来源：工作区中与本仓库并列的 `CookLikeHOC` 仓库
  （`https://github.com/Gar-b-age/CookLikeHOC`）。菜谱文案与照片都来自那里，
  本项目只做裁切与缩放。
- **上游仓库没有声明任何许可。** 它没有 `LICENSE`、`COPYING` 或同类文件，
  README 里只有一段与无关网站的免责声明。照片由社区贡献，菜谱描述的是某连锁
  餐饮品牌的菜品。
- 后果：公开分发本固件、或把它做成产品出货，等于在没有获得授权的情况下再分发
  第三方文案与照片。这应当作为一项法务评审事项对待，而不是已经解决的问题。
  在上游条款得到确认、或内容被替换之前，内部评估与个人使用是安全范围。

不要手工修改这些文件。它们从菜谱照片重新生成，本身不承载独立内容。
