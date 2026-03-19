# pdluajit API 使用指南

pdluajit 是 plugdata 的 LuaJIT 扩展对象，支持高性能 DSP 处理和 GPU 加速图形渲染。

- **脚本格式**: `.pd_luajit` 文件
- **用法**: `[pdluajit scriptname <arg1> <arg2> ...]`
- **运行时**: LuaJIT 2.1（支持 FFI、bit 库）
- **每个对象独立 Lua 状态**（不同于 pd-lua 的共享状态）

---

## 1. 入口/出口配置

在脚本顶层设置全局表：

```lua
inlets  = {SIGNAL, SIGNAL, DATA}   -- 2个信号入口 + 1个数据入口
outlets = {SIGNAL, DATA, DATA}      -- 1个信号出口 + 2个数据出口
```

| 常量 | 值 | 说明 |
|------|---|------|
| `SIGNAL` / `pd.SIGNAL` | 1 | 音频信号 |
| `DATA` / `pd.DATA` | 0 | 消息数据 |

- 最多 16 个入口/出口
- 入口/出口数量在对象创建时固定，重载脚本不会改变

---

## 2. 生命周期回调

### `initialize(args)`

对象创建时和脚本重载时调用。

```lua
function initialize(args)
    -- args = pd.args 的副本（1索引）
    -- [pdluajit myscript 440 0.5] → args = {440, 0.5}
    local freq = args[1] or 440
    pd.set_size(200, 150)
end
```

### `dsp(samplerate, blocksize)`

DSP 启动/重配置时调用。

```lua
function dsp(sr, bs)
    pd.post("采样率: " .. sr .. " 块大小: " .. bs)
end
```

### `tick()`

由 `pd.clock_set()` 设置的定时器触发。

```lua
function tick()
    -- 每 16ms 调用一次（约 60fps）
    pd.repaint()
end

pd.clock_set(16)  -- 启动定时器
```

### `resized(w, h)`

plugdata 中用户拖拽调整大小时调用。

```lua
function resized(w, h)
    gfx_w = w
    gfx_h = h
    -- 重新分配缓冲区等
end
```

### `invalidate()`

NVG 渲染上下文被重建时自动调用（VST 关闭/重开窗口、DPI 变化等）。
**必须在此函数中将所有缓存的 image handle 置 nil**，否则旧 handle 会指向错误的 GPU 资源。

```lua
function invalidate()
    my_texture = nil    -- 旧的 NVG image ID 已失效
    my_image = nil
end
```

> 如果脚本未使用 `create_image` / `create_image_rgba`，则不需要定义此函数。

---

## 3. DSP 处理

### `perform(ins, outs, n)`

每个音频块调用一次（音频线程，极度性能敏感）。

```lua
local ffi = require('ffi')

function perform(ins, outs, n)
    -- ins[1], ins[2]... = 信号入口的 t_float* 指针
    -- outs[1], outs[2]... = 信号出口的 t_float* 指针
    -- n = 采样数（块大小）
    -- 注意：通道是1索引，采样是0索引！（因为FFI已经被包裹在perform方法中）
    for i = 0, n - 1 do
        outs[1][i] = ins[1][i] * 0.5
    end
end
```

**性能技巧**:
- 用 `local` 缓存所有频繁访问的值
- 用 `ffi.copy()` / `ffi.fill()` 处理批量操作
- 避免在 perform 中创建 Lua 表或字符串（触发 GC）

---

## 4. 消息入口处理

4 级优先分发机制（入口编号为 **1 索引**）：

### 级别 1: `in_N_sel(...)` — 指定入口 + 指定选择器

```lua
function in_1_bang()           end        -- 入口1收到 bang
function in_1_float(x)        end        -- 入口1收到 float
function in_1_symbol(s)       end        -- 入口1收到 symbol
function in_1_list(atoms)     end        -- 入口1收到 list
function in_2_freq(atoms)     end        -- 入口2收到 "freq" 消息
```

### 级别 2: `in_n_sel(n, ...)` — 任意入口 + 指定选择器

```lua
function in_n_float(n, x)     end        -- 任意入口收到 float，n=入口号
function in_n_bang(n)          end        -- 任意入口收到 bang
```

### 级别 3: `in_N(sel, atoms)` — 指定入口 + 任意选择器

```lua
function in_1(sel, atoms)
    if sel == "freq" then
        freq = atoms[1]
    elseif sel == "gain" then
        gain = atoms[1]
    end
end
```

### 级别 4: `in_n(n, sel, atoms)` — 通配（万能兜底）

```lua
function in_n(n, sel, atoms)
    pd.post("入口 " .. n .. " 收到: " .. sel)
end
```

---

## 5. pd.* API

### 输出

```lua
pd.outlet(1, "bang", {})           -- 出口1发送 bang
pd.outlet(1, "float", {3.14})     -- 出口1发送 float
pd.outlet(1, "symbol", {"hello"}) -- 出口1发送 symbol
pd.outlet(1, "list", {1, 2, 3})   -- 出口1发送 list
pd.outlet(2, "freq", {440})       -- 出口2发送自定义消息
```

### 日志

```lua
pd.post("调试信息")     -- 普通消息
pd.error("出错了!")      -- 错误消息（红色高亮）
```

### 图形控制

```lua
pd.set_size(300, 200)   -- 设置组件大小（像素）
pd.repaint()            -- 请求重绘（下一帧调用 paint）
```

### 时钟

```lua
pd.clock_set(16)        -- 启动定时器，每 16ms 调用 tick()
pd.clock_set(0)         -- 停止定时器
pd.clock_unset()        -- 等同于 pd.clock_set(0)
```

> 必须先定义 `tick()` 函数才能使用 `pd.clock_set()`。

### 颜色打包

```lua
pd.rgba(r, g, b)        -- 返回 0xFFRRGGBB (alpha 默认 255)
pd.rgba(r, g, b, a)     -- 返回 0xAARRGGBB
```

用于 `create_image_rgba` / `update_image` 的 uint32 像素缓冲区。
C 底层实现，跨平台一致，无需手动位操作。

```lua
-- 预计算颜色 LUT（推荐用于高性能场景）
local lut = ffi.new('uint32_t[256]')
for i = 0, 255 do lut[i] = pd.rgba(i, 0, 255 - i) end

-- 逐像素填充
pixels[y * w + x] = pd.rgba(255, 0, 0)

-- 或直接用 LUT（热循环零开销）
pixels[y * w + x] = lut[idx]
```

### FFI 加载

```lua
local lib = pd.load_ffi("mylib.dll")  -- 从脚本目录加载动态库
```

### 只读属性

| 属性 | 类型 | 说明 |
|------|------|------|
| `pd.scriptdir` | string | 脚本所在目录的绝对路径 |
| `pd.args` | table | 创建参数（1索引） |
| `pd.samplerate` | number | 当前采样率 |
| `pd.blocksize` | integer | 当前块大小 |

---

## 6. Graphics 图形 API

定义 `paint(g)` 函数即可启用 GUI 模式。`g` 是 `Graphics` 对象。

### 颜色和状态

```lua
g:set_color(r, g, b)           -- RGB, 0-255
g:set_color(r, g, b, a)        -- RGBA, alpha 默认 255
g:stroke_width(2.0)            -- 描边宽度
g:save()                       -- 保存状态
g:restore()                    -- 恢复状态
g:reset_transform()            -- 重置变换
```

### 填充形状

```lua
g:fill_all()                         -- 填充整个背景（带边框和圆角）
g:fill_rect(x, y, w, h)             -- 填充矩形
g:fill_rounded_rect(x, y, w, h, r)  -- 填充圆角矩形
g:fill_ellipse(x, y, w, h)          -- 填充椭圆（x,y=左上角）
```

### 描边形状

```lua
g:stroke_rect(x, y, w, h)             -- 描边矩形
g:stroke_rounded_rect(x, y, w, h, r)  -- 描边圆角矩形
g:stroke_ellipse(x, y, w, h)          -- 描边椭圆
```

### 线条

```lua
g:draw_line(x1, y1, x2, y2)  -- 画直线
```

### 路径（原生贝塞尔曲线，无扁平化）

```lua
g:begin_path()
g:move_to(x, y)                         -- 移动画笔
g:line_to(x, y)                         -- 直线段
g:quad_to(cx, cy, x, y)                 -- 二次贝塞尔
g:bezier_to(c1x, c1y, c2x, c2y, x, y)  -- 三次贝塞尔
g:close_path()                           -- 闭合路径
g:fill()                                 -- 填充路径
g:stroke()                               -- 描边路径
```

### 文本

```lua
g:draw_text("文字内容", x, y)                    -- 默认宽度1000, 字号12
g:draw_text("文字内容", x, y, max_width)         -- 指定最大宽度
g:draw_text("文字内容", x, y, max_width, size)   -- 指定字号
g:draw_text(42, x, y)                            -- 数字会自动转 tostring
```

### 变换

```lua
g:translate(tx, ty)       -- 平移坐标原点
g:scale(sx, sy)           -- 缩放
g:reset_transform()       -- 重置（内部调用 restore + save）
```

### 尺寸查询

```lua
local w = g:width()       -- 当前绘制区域宽度
local h = g:height()      -- 当前绘制区域高度
```

### 图片

```lua
-- 从文件加载图片（路径相对于脚本目录）
local img = g:create_image("photo.png")

-- 绘制图片
g:draw_image(img, x, y, w, h)          -- 缩放到指定区域
g:draw_image(img, x, y, w, h, 0.5)     -- 半透明

-- 用图片作为填充纹理
g:set_fill_image_pattern(ox, oy, ex, ey, angle, img, alpha)

-- 删除图片（释放 GPU 内存）
g:delete_image(img)
```

### 动态像素纹理（高性能）

适用于频谱图、波形等需要逐像素绘制的场景：

```lua
local ffi = require('ffi')
local bor, lshift = bit.bor, bit.lshift

-- 分配像素缓冲区 (ARGB uint32)
local pixels = ffi.new('uint32_t[?]', width * height)

-- 写入像素: 0xAARRGGBB 格式
local function pack_pixel(r, g, b, a)
    return bor(lshift(a or 0xFF, 24), lshift(r, 16), lshift(g, 8), b)
end

-- 创建 GPU 纹理
local tex = g:create_image_rgba(width, height, pixels)

-- 更新纹理数据（无需重新创建）
g:update_image(tex, pixels)

-- 绘制纹理
g:draw_image(tex, 0, 0, display_w, display_h)
```

---

## 7. 鼠标交互

定义以下回调函数接收鼠标事件（坐标为组件内像素坐标）：

```lua
function mouse_down(x, y)
    -- 鼠标按下
    pd.repaint()
end

function mouse_up(x, y)
    -- 鼠标释放
end

function mouse_move(x, y)
    -- 鼠标移动（未按下）
end

function mouse_drag(x, y)
    -- 鼠标拖拽（按下状态移动）
    pd.repaint()
end
```

---

## 8. FFI 高性能技巧

### 零拷贝 DSP

```lua
local ffi = require('ffi')
local copy = ffi.copy
local fill = ffi.fill

function perform(ins, outs, n)
    -- 直接复制：比逐样本循环快 ~5x
    copy(outs[1], ins[1], n * 4)  -- 4 = sizeof(float)
end
```

### FFI 结构体

```lua
ffi.cdef[[
    typedef struct {
        double x, y, vx, vy;
        double mass;
    } Particle;
]]

local particles = ffi.new("Particle[?]", 1000)  -- 连续 C 内存
```

### 避免 GC 压力

```lua
-- 坏：每次分配新表
function in_1_bang()
    pd.outlet(1, "list", {x, y, z})  -- 每次调用创建新表
end

-- 好：复用缓存表
local out = {}
function in_1_bang()
    out[1] = x; out[2] = y; out[3] = z
    pd.outlet(1, "list", out)  -- 零分配
end
```

---

## 9. 完整示例


```lua
-- gain.pd_luajit — 简单增益
inlets  = {SIGNAL, DATA}
outlets = {SIGNAL}

local gain = 1.0

function perform(ins, outs, n)
    local g = gain
    for i = 0, n - 1 do
        outs[1][i] = ins[1][i] * g
    end
end

function in_2_float(x)
    gain = x
end
```

### 示例 GUI 对象

```lua
-- button.pd_luajit — 简单按钮
inlets  = {DATA}
outlets = {DATA}

local pressed = false

function initialize()
    pd.set_size(60, 30)
end

function paint(g)
    if pressed then
        g:set_color(255, 100, 100)
    else
        g:set_color(100, 100, 100)
    end
    g:fill_all()
    g:set_color(255, 255, 255)
    g:draw_text("BANG", 12, 8, 50, 14)
end

function mouse_down(x, y)
    pressed = true
    pd.outlet(1, "bang", {})
    pd.repaint()
end

function mouse_up(x, y)
    pressed = false
    pd.repaint()
end
```

### 自刷新动画

```lua
-- spinner.pd_luajit — 旋转动画
inlets  = {DATA}
outlets = {}

local angle = 0

function initialize()
    pd.set_size(100, 100)
end

function tick()
    angle = angle + 0.05
    pd.repaint()
end

pd.clock_set(16)  -- ~60fps

function paint(g)
    g:set_color(30, 30, 40)
    g:fill_all()

    g:save()
    g:translate(50, 50)
    g:scale(1, 1)

    local r = 30
    local x = r * math.cos(angle)
    local y = r * math.sin(angle)

    g:set_color(255, 100, 50)
    g:fill_ellipse(x - 8, y - 8, 16, 16)
    g:restore()
end
```

---

## 10. Pd 消息接口

从外部发送给 `[pdluajit]` 对象本身的消息：

| 消息 | 说明 |
|------|------|
| `load <name>` | 加载/切换脚本 |
| `reload` | 重新加载当前脚本 |
| `menu-open` | 在 plugdata 编辑器中打开脚本 |

---

## 11. 注意事项

- **paint() 中不要调用 pd.outlet()**：paint 在渲染线程执行，pd.outlet 只能在消息线程调用
- **perform() 中避免创建 Lua 对象**：音频线程的 GC 会导致卡顿
- **入口编号从 1 开始**：`in_1_bang` 对应第一个入口
- **FFI 指针从 0 开始**：`ins[1][0]` 是第一个采样点
- **重载安全**：`initialize()` 会在重载时重新调用，FFI 缓冲区会正确重新分配
- **图片句柄**：`create_image` / `create_image_rgba` 返回的句柄只在当前 paint 上下文有效，首次创建后可缓存复用
