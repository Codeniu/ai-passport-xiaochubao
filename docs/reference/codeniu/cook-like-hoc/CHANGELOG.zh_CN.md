<p align="right">
  <strong>简体中文</strong> · <a href="CHANGELOG.md">English</a>
</p>

# 小厨宝 — 更新日志

离线菜谱应用的开发记录。条目按改动落地的日期分组，项目目前还没有发布标签。
按键操作与设置项见[使用说明](USER_GUIDE.zh_CN.md)，设计与实现档案见
[`README.zh_CN.md`](README.zh_CN.md)。

## 2026-09-20 — 菜谱分类

- 在首页与菜谱列表之间新增分类页。首页短按 OK 进入的是分类列表，列表页只显示
  所选分类下的菜谱。
- 分类沿用源仓库的目录顺序，共 15 项，不设「全部」一行：

  | 分类 | 菜谱数 | 分类 | 菜谱数 |
  | --- | ---: | --- | ---: |
  | 炒菜 | 71 | 砂锅菜 | 15 |
  | 蒸菜 | 49 | 炖菜 | 14 |
  | 配料 | 40 | 烫菜 | 12 |
  | 早餐 | 35 | 卤菜 | 11 |
  | 主食 | 32 | 煮锅 | 11 |
  | 饮品 | 21 | 汤 | 6 |
  | 炸品 | 18 | 凉拌 | 4 |
  |  |  | 烤类 | 1 |

- 每行左侧是 18px 的分类名，右侧是 14px 的菜谱数量，选中条与列表页同一套样式。
- 分类页与列表页共用同一套翻页与选中逻辑，光标、翻页、行动画在两页上表现一致。
  列表页长按 OK 现在回到分类页，而不是回首页。
- 列表页页眉显示当前分类名与 `n/该分类总数` 计数，不再写 `n/340`；底部提示条改为
  「长按OK返回分类 短按OK确认」。
- 数据管线：`tools/build_cook_assets.py` 现在额外产出 `COOK_CATEGORIES[]`
  （`name` / `first` / `count`）以及状态机要读的 `COOK_CATEGORY_OFFSETS[]`、
  `COOK_CATEGORY_SIZES[]`。分类在 `COOK_RECIPES` 里本来就是连续区间，所以不需要
  额外的下标表。
- 状态机：新增 `COOK_PAGE_CATEGORY` 及 `cat_selected`、`list_category`；
  `list_selected` 变成当前分类内的下标，`detail_index` 仍是全局下标。
- 测试：主机测试覆盖进入分类页、翻页、选中分类以及从列表页返回。

  **尚未在真机验证** —— 这一版的镜像已经构建完成，但截至撰写时还没刷进设备。

## 2026-09-20 — 息屏改为真正的低功耗

- 以前的息屏只关背光，现在会先把面板熄灭，再让芯片进入 light sleep。
- 新增 `main/cook_power.{h,c}`：休眠阶段多出 `BLANK` 档（背光灭 + 面板发 Sleep In
  + 停止刷屏，CPU 仍全速运行，按键零延迟），过 `COOK_SLEEP_LIGHT_DELAY` 秒后转入
  `LIGHT` 档（反复 light sleep）。
- 进 light sleep 前必须 `iot_button_stop()` 停掉按键轮询，否则它 5 ms 一次的定时器
  会立刻把芯片唤醒；按键改由 GPIO 唤醒源接管，任意键都能唤醒设备。
- 唤醒行为不变：按任意键即恢复背光与面板。

## 2026-09-20 — 页面标题缺字

- 修掉设置页与关于页顶部的乱码。26px 标题字体是单独一份很小的子集，字符集原先写死
  在 `tools/build_cook_fonts.py` 里，没有这两个标题用到的四个字，LVGL 在真机上就画
  成方块。
- 标题字符集改为脚本生成：`tools/build_cook_assets.py` 新增 `UI_TEXT_TITLES` 常量并
  写出 `assets/fonts/cook_chars_titles.txt`，字体脚本改为读这个文件，写死的清单已删除。
- `tests/test_cook_assets.py` 新增 `check_font_coverage()`：扫描
  `main/demo_cook.c` 里 `&cook_font_NN` 的用法，推断每个字号要渲染哪些文案，再解析
  生成的 `cook_font_NN.c` 的 cmap（`FORMAT0_TINY` 连续区间与 `SPARSE_TINY` 偏移表）
  逐个码点核对。三个字号现在全部通过。

## 2026-09-20 — 第二轮真机反馈

- 去掉待机页。它本想显示时间，但板子没有可信时钟，所以到点直接息屏。休眠阶段从三级
  砍成两级，设置项里相应的那一档也一并删除。
- 修掉电量折行：标签原本只有 34px 宽，而 14px 字体下 `100%` 约需 35px。现在给
  46px（新增 `COOK_BATT_LABEL_W`）并显式改为裁剪而不是换行。
- 修掉列表项整体偏上：18px 字体的行高只有 21px，比 24px 的行矮 3px。现在每行先建
  一个等高的 slot 容器，标签在容器里 `LV_ALIGN_LEFT_MID` 对齐。
- 详情页拆成三个子页（封面+菜名 / 配料 / 步骤）。配料与步骤各自整页翻页，短按翻页、
  翻到头接着翻子页，长按直接翻子页。行首圆点与序号用 LVGL 的 `#RRGGBB ... #` 重着色
  上橙色，不给每行多加控件（LVGL 堆只有 48 KB）。菜谱正文因此不能再出现 `#`，
  这条不变式由测试守住。

## 2026-09-20 — 文档与预览

- 新增 `USER_GUIDE.md` 与 `USER_GUIDE.zh_CN.md`：简介、页面一览、逐页按键表、
  设置详解、电量显示、内容来源与许可提示、构建烧录步骤、已知限制。两个 README 与
  `docs/reference` 索引均已加互链。
- 修掉预览脚本里电量图形重叠：外框原来用 `right:` 反推，等于被自身宽度左移，正好压在
  百分比文字上。现在改成与固件一致的绝对 `left` 坐标。

## 2026-09-18 — 第一轮真机反馈

- 修掉首页选中态看不见：item 容器建出来时 `bg_opa` 是全透明，只改 `bg_color` 不生效。
  refresh 里补上 `bg_opa = LV_OPA_COVER`，选中底色用主色橙。
- 修掉一进带封面页面就重启：任务栈是 `esp_lvgl_port` 默认的 7168 字节，而 LVGL 的
  tjpgd `decoder_info()` 在栈上开 4096 字节缓冲，渲染路径叠起来越界，芯片报
  stack protection fault。涉及 JPEG 解码的任务栈现在至少 16 KB。
- 修掉切页顺序：先挂新屏再删旧屏。删掉正在显示的屏幕会让 LVGL 当前屏幕变成 NULL，
  下一次刷新就崩。
- 修正输入任务栈的注释：`xTaskCreate` 的栈深单位是 4 字节字，8192 实际是 32 KB。

## 2026-09-18 — 首个可用版本

- 应用主体：`main/cook_model.{h,c}` 是四页状态机（首页 / 随机 / 列表 / 详情），
  不依赖 LVGL 与 ESP-IDF，可在主机上测试；`main/demo_cook.c` 是 LVGL 界面。
  开机直达应用，不再注册演示菜单。
- 资源管线：`tools/build_cook_assets.py` 把 340 道菜的 Markdown 解析成 C 数据表、
  两档封面 JPEG blob 与字符清单；`tools/build_cook_fonts.py` 用 `lv_font_conv`
  生成 14/18/26px 的 Noto Sans SC 子集字体。
- 随机推荐排除配料与饮品，候选 279 道，列表仍给全部 340 道。抽取走两次遍历，不在
  8 KB 的输入任务栈上开 4096 项数组。
- 上游有四个菜名撞车，已用文件名兜底去重，340 个菜名现在唯一，并加了测试守住。
- `tests/test_cook_model.c` 与 `tests/test_cook_assets.py` 接入主机测试。

## 上游基线的已知问题

以下与本项目改动无关，仍然挂着：

- `tests/test_check_repo.py` 在 Windows 下有 5 例失败：
  `tools/check_repo.py` 用 `Path.relative_to` 生成路径，Windows 下是反斜杠，
  而用例断言的是正斜杠。
- `actionlint` 的安装脚本只支持 Linux/macOS，Windows 下跑不了校验脚本的这一步。
