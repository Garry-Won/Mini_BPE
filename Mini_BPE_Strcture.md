# Mini BPE 源码精读（`main.cpp` 逐行详解）

> 面向对象：零基础读者。
>
> 写法约定：`L9–27` 表示"第 9 行到第 27 行"；所有 `L` 开头的编号都能直接在 `main.cpp` 里一一对应。
>
> 文中所有"实测"数字，都是在仓库现有语料与模型上真实跑出来的（2026-11-22 复现，命令见第 9 章）。

---

## 目录

- [0. 五分钟速览](#0-五分钟速览)
- [1. 全局结构：两代程序 + 一个调度器](#1-全局结构两代程序--一个调度器)
- [2. A 层：`merge_once`（L9–27）](#2-a-层merge_oncel927)
- [3. B 层：语料级实现（L29–451）](#3-b-层语料级实现l29451)
- [4. C 层：`main` 与旧版功能（L453–665）](#4-c-层main-与旧版功能l453665)
- [5. 真实语料的完整旅程（实测）](#5-真实语料的完整旅程实测)
- [6. 变量总表](#6-变量总表)
- [7. 易错点清单](#7-易错点清单)
- [8. 自测题](#8-自测题)
- [9. 复现命令](#9-复现命令)

---

## 0. 五分钟速览

### 0.1 这个程序是什么

一个 **字节级 BPE（Byte Pair Encoding）分词器**，功能对标 GPT-2 的 `encoder.py`：

- **train**：读语料，反复找"最频繁的相邻编号对"，把这对编号替换成一个新编号，并记录规则。
- **encode**：用训练好的规则，把任意文本变成一串编号（token id）。
- **decode**：把编号串还原成原始字节，逐字节还原文。

"字节级"的意思是：**字母表不是字符，而是 256 种字节值**。所以任何文本（中文、emoji、二进制）都不会出现"未知字符"，永远不会 out-of-vocabulary。

### 0.2 编号的约定（先记这三条，后面全用它）

| 编号范围 | 含义 |
|---|---|
| `0` – `255` | 单个字节值。`vocab[97] == "a"`，`vocab[32] == " "`，`vocab[10] == "\n"` |
| `256` | 特殊 token `<\|endoftext\|>`（文档边界），**不参与合并** |
| `257` 起 | 训练出来的合并结果。第 1 条规则 → 257，第 2 条 → 258，依次递增 |

### 0.3 三条命令

```bash
./mini_bpe train  INPUT MODEL [VOCAB_SIZE]   # 训练（默认词表 10000）
./mini_bpe encode MODEL  < text > ids        # 编码：文本 → 编号
./mini_bpe decode MODEL  < ids  > text       # 解码：编号 → 文本
```

### 0.4 一句话数据流

```
bytes ──正则预分词──> pieces ──字节编号──> [97,98,...] ──反复合并──> ids ──查词表──> bytes
```

---

## 1. 全局结构：两代程序 + 一个调度器

### 1.1 三层分区

| 区域 | 行号 | 内容 | 编译条件 |
|---|---|---|---|
| **A 层** 公共工具 | L9–27 | `merge_once`：唯一的"合并一次"函数，三层共用 | 无 |
| **B 层** 语料级实现（"新"的） | L29–451 | `namespace corpus_bpe`：预分词、`Model`、训练、存盘、编码、解码、命令行 | 必须 `-DMINI_BPE_PCRE2` |
| **C 层** 调度 + 旧版实现 | L453–665 | `main()`：参数分流、旧版 encode/decode、`count`、默认演示 | 无 |

B 层被 `#ifdef MINI_BPE_PCRE2`（L29）整个包住，`#endif` 在 L451。**不加这个宏，B 层一行都编译不进去**——因为它依赖 PCRE2 正则库（仓库里已解压在 `output/pcre2-dev/`）。所以同一个 `main.cpp`，编译方式不同就是两个不同的程序。

### 1.2 函数/块速查表

| 行号 | 内容 | 一句话 |
|---|---|---|
| L1–7 | `#include` | 只用标准库 |
| L9–27 | `merge_once` | 把一对编号替换成新编号 |
| L29–31 | `#ifdef` + `PCRE2_CODE_UNIT_WIDTH 8` + `pcre2.h` | PCRE2 要求先定义码元宽度（8 位）再包头文件 |
| L33–39 | 其余头文件 | `cstdint`(定长整数)、`limits`(INT_MAX)、`memory`(unique_ptr)、`set`(有序集合)、`stdexcept`(异常)、`unordered_map/set`(哈希表) |
| L41 | `namespace corpus_bpe {` | 新版整包进命名空间 |
| L43–48 | 类型别名与常量 | `Pair`、`Frequency`、`special_id`、`first_merge_id`、`marker`、`model_header` |
| L50–56 | `PairHash` | 给 `std::pair` 写哈希函数 |
| L58–65 | `byte_ids` | 字符串 → 字节编号数组 |
| L67–90 | `struct Model` | 三张表：`vocab` / `rules` / `ranks` |
| L92–93 | `RegexCode` / `MatchData` | 带自定义删除器的智能指针（RAII） |
| L95–161 | `class Pretokenizer` | GPT-2 正则预分词 + 文档边界处理 |
| L163–192 | 训练数据结构 + `RankOrder` | `WeightedPiece` / `PairStats` / 排序规则 |
| L194–263 | `train` | 四段式：计数 → 统计 → 建索引 → 循环合并 |
| L265–275 | `parse_integer` | 严格解析非负十进制整数 |
| L277–290 | `save_model` | 写 header + 规则 |
| L292–311 | `load_model` | 读 header + 规则，用 `add_rule` 重建 |
| L313–336 | `encode_piece` | 对单个片段做 BPE 编码 |
| L338–351 | `CachedPiece` + `hash_piece` | 片段级缓存 + FNV-1a 哈希 |
| L353–385 | `encode_tokens` | 编码流水线（快路径 / 缓存 / 慢路径） |
| L387–392 | `encode` | 包装成"读 stdin、写 stdout" |
| L394–406 | `decode` | 编号 → 字节 |
| L408–448 | `run` | 命令行实现（train / encode / decode） |
| L450–451 | `}` + `#endif` | 收尾 |
| L453–464 | `main` 调度 | 参数分流总闸门 |
| L466–498 | 旧版 `decode` | 读死路径 `output/rules.txt` |
| L500–533 | 旧版 `encode` | 同上，编号从 256 起 |
| L535–597 | `count` | 跑 3 轮，把规则打到 stdout |
| L599–664 | 默认演示 | 读 stdin，跑 3 轮，把合并结果打到 stdout |

### 1.3 命令可达性矩阵（**最容易看错的地方**）

`main()` 的总闸门在 L456：

```cpp
if (mode == "train" || ((mode == "encode" || mode == "decode") && argc > 2)) {
```

`encode` / `decode` 额外要求 **`argc > 2`**（必须带模型路径）。于是：

| 输入命令 | argc | 实际走到 | 结果 |
|---|---|---|---|
| `./x train IN MODEL [VOCAB]` | 4 / 5 | L457–458 → `corpus_bpe::run` | 没 PCRE2 则 L460 报错退出 |
| `./x encode MODEL` | 3 | 同上 | 需要 PCRE2 |
| `./x decode MODEL` | 3 | 同上 | 需要 PCRE2 |
| `./x encode` | **2** | L456 不成立 → 落到 **L500 旧版 encode** | 读硬编码 `output/rules.txt` |
| `./x decode` | **2** | 落到 **L466 旧版 decode** | 读硬编码 `output/rules.txt` |
| `./x count FILE` | 3 | L535 | mode 不是那三个，L456 不命中 |
| 不带参数 / 其它 | — | L599 默认演示 | 从 stdin 读，跑 3 轮 |

> 结论：L466–533 那段旧代码**不是死代码**，但只有"不给模型路径"时才会执行。带上模型路径就永远进不去。
> 另外 `count` 和"默认演示"在 B 层**没有对应实现**——它们是早期版本的遗留物。

### 1.4 两代程序不能混用：`256` 到底是谁

| | B 层（新） | C 层（旧） |
|---|---|---|
| 第一个合并编号 | **257**（L46 `first_merge_id = special_id + 1`） | **256**（L521、L558 `int new_id = 256;`） |
| 256 号 | `<\|endoftext\|>` | 第一个合并结果 |
| 编号来源 | `Model::add_rule` 内 `vocab.size()` | 调用方手写 `new_id` |

所以旧版生成的 `output/rules.txt`（首条 `97 98` → 256）**不能当作 B 层的模型文件**：`add_rule` 的校验（L81–83）明确禁止规则里出现 256，会直接抛异常。

### 1.5 总流程图

```
                     ┌──────────────────────────────────────────────┐
 文件/ stdin ──────> │ Pretokenizer::read        (L137–160)         │
                     │  · getline 逐行                               │
                     │  · 找 "<|endoftext|>" → 切段 + emit_special   │
                     │  · 行尾补 '\n'（非 EOF 时）                    │
                     └───────────────────┬──────────────────────────┘
                                         │ document（一段不含特殊 token 的文本）
                                         v
                     ┌──────────────────────────────────────────────┐
                     │ Pretokenizer::split        (L99–116)         │
                     │  · PCRE2_ANCHORED，从 offset 连续匹配          │
                     │  · 每次 emit(指针, 长度)  → 一个 piece        │
                     └───────────────────┬──────────────────────────┘
                  ┌──────────────────────┴───────────────────────┐
                  v                                              v
      ┌───────────────────────────┐               ┌──────────────────────────────┐
      │ train 的计数回调 (L200–202)│               │ encode_tokens 的 emit (L363) │
      │  frequencies[piece]++     │               │  单字节 → 直出               │
      └───────────┬───────────────┘               │  ≤64 字节 → 查缓存           │
                  v                               │  否则 → encode_piece         │
      ┌───────────────────────────┐               └──────────────┬───────────────┘
      │ train 主循环 (L226–261)    │                              v
      │  选对 → add_rule → 差分更新 │               ┌──────────────────────────────┐
      └───────────┬───────────────┘               │ encode_piece (L313–336)      │
                  v                               │  反复合并 rank 最小的相邻对     │
      ┌───────────────────────────┐               └──────────────┬───────────────┘
      │ Model{rules → 存盘}        │                              v
      │ save_model (L277–290)     │                     ids → stdout (L389–391)
      └───────────────────────────┘
```

---

## 2. A 层：`merge_once`（L9–27）

### 2.1 签名与语义

```cpp
std::vector<int> merge_once (const std::vector<int>& ids,
                             std::pair<int, int> best_pair, int new_id)
```

**做的事**：在一串编号里，把所有"相邻且正好等于 `best_pair`"的位置，替换成 `new_id`，返回新的一串。

- `ids` 用 `const&`（只读引用）：不拷贝调用者的数据，也不允许改它。
- 返回值是**新建**的 `vector`。

### 2.2 逐行

| 行 | 细节 |
|---|---|
| L11 | `std::vector<int> merged;` 输出缓冲区。**不原地改**，因为边扫边改会把自己的下标搞乱 |
| L12 | `for (... i = 0; i < ids.size() ; )` —— 注意第三格是**空的**，`i` 在函数体里手动推进 |
| L13–15 | `matches` = "右边还有元素（`i+1 < size`，防越界）" **且** "当前 == first" **且** "下一个 == second" |
| L17–19 | 命中：写入 `new_id`，`i += 2`（**一次吃掉两个**） |
| L20–23 | 不命中：原样搬运 `ids[i]`，`i += 1` |
| L26 | 返回 `merged` |

### 2.3 两个必须记住的性质

**性质 1：从左到右，不重叠。**
`i += 2` 保证刚被合并掉的两个位置不会被重复使用。

**性质 2：一次调用会把该对"所有"出现位置都合并掉。**

例：`ids = [97,97,97]`，`best_pair = (97,97)`，`new_id = 256`

```
i=0：命中 → 输出 256，i=2
i=2：i+1 = 3 不 < 3 → 不命中 → 输出 97，i=3
结果：[256, 97]        ← 不是 [256, 256]！
```

第一个 97 已经被吃掉了，剩下那个没有"右邻居"可以配对了。

这条性质被训练（L242）和编码（L334）反复依赖：它们都假设"调一次 `merge_once` 就把这一对清干净了"。

### 2.4 变量表

| 变量 | 类型 | 作用 |
|---|---|---|
| `ids` | `const std::vector<int>&` | 输入编号序列（只读） |
| `best_pair` | `std::pair<int,int>` | 要合并的那一对 |
| `new_id` | `int` | 合并后产生的新编号 |
| `merged` | `std::vector<int>` | 输出缓冲区 |
| `i` | `std::vector<int>::size_type` | 扫描位置 |
| `matches` | `bool` | 本位置是否命中 |

### 2.5 复杂度

时间 O(n)，空间 O(n)（每次都新建一个 vector）。
在 `train` 里，每轮只对"受影响的片段"调用一次；在 `encode_piece` 里，每轮调用一次，轮数 = 该片段被合并的次数（通常 1–3 次）。

---

## 3. B 层：语料级实现（L29–451）

### 3.1 常量与类型（L41–56）

| 行 | 名字 | 含义 |
|---|---|---|
| L41 | `namespace corpus_bpe` | 把新版整包进命名空间，避免和旧代码重名 |
| L43 | `using Pair = std::pair<int,int>` | "一对编号"的简写 |
| L44 | `using Frequency = std::int64_t` | 频次用 **64 位**整数。2.2 GB 语料上的计数轻松破 21 亿，`int` 会溢出 |
| L45 | `special_id = 256` | `<\|endoftext\|>` 的固定编号 |
| L46 | `first_merge_id = 257` | 第一个可用的合并编号 |
| L47 | `marker = "<\|endoftext\|>"` | 文本里表示文档边界的字面串 |
| L48 | `model_header = "mini-bpe-gpt2-v1"` | 模型文件第一行，识别格式/版本 |
| L50–56 | `PairHash` | `std::pair` 没有默认哈希函数，自己写：两个 32 位拼成一个 64 位再交给 `std::hash`。`(a << 32) \| b` 是无碰撞的"打包" |

**为什么 0–255 正好是字节？** 一个字节有 256 种取值，所以任何文本都一定能被表示成编号序列。这就是字节级 BPE 永不出 OOV 的原因。

### 3.2 `byte_ids`（L58–65）

```cpp
for (unsigned char byte : piece)
    ids.push_back(byte);
```

注意循环变量是 `unsigned char`。如果写 `char`，在"char 是有符号"的平台上，`0x80` 以上的字节会变成负数，再转 `int` 就成了负编号。**这是处理字节时最经典的坑。**

### 3.3 `struct Model`（L67–90）——整个程序的心脏

```cpp
std::vector<std::string> vocab;                  // L68
std::vector<Pair> rules;                         // L69
std::unordered_map<Pair, int, PairHash> ranks;   // L70
```

| 成员 | 是什么 | 谁在用 |
|---|---|---|
| `vocab` | **编号 → 这段编号代表哪些字节**。`vocab[257]` 存的就是拼好的字符串 | 解码 L401；训练时做优先级比较 L184–188；`add_rule` 拼新串 L86 |
| `rules` | **合并规则的有序列表**。第 k 条（k 从 0 起）产生编号 `257+k` | 存盘 L283–285；读盘时重放 L305 |
| `ranks` | **反向索引**：(左,右) → 新编号。编码时 O(1) 判断"这一对有没有规则、优先级多高" | `encode_piece` L325 |

`rules` 和 `ranks` 存的是同一批信息，只是方向不同：训练按顺序遍历（`rules`），编码按对查询（`ranks`）。

#### 构造函数（L72–77）

```cpp
for (int byte = 0; byte < special_id; ++byte)
    vocab.emplace_back(1, static_cast<char>(byte));   // 0..255
vocab.push_back(marker);                              // 256
```

`std::string(1, c)` = "造一个长度 1、内容为 c 的字符串"。循环结束后 `vocab.size() == 257`。

#### `add_rule`（L79–89）

| 行 | 动作 |
|---|---|
| L80 | `const int id = static_cast<int>(vocab.size());` —— **新编号不是参数，而是"当前词表长度"** |
| L81–83 | 一次性大校验：左右编号都必须 `>= 0` 且 `< id`（不能引用还不存在的编号）、都不能等于 `special_id`、`id` 不能到 `int` 上限、这对规则不能在 `ranks` 里重复 |
| L86 | `vocab.push_back(vocab[pair.first] + vocab[pair.second]);` —— 新词条 = 左词字符串 + 右词字符串，**是字节拼接，不是数字运算** |
| L87 | `rules.push_back(pair);` |
| L88 | `ranks.emplace(pair, id);` |

两条不变量（后面所有推理都建立在这上面）：

1. **`vocab` 只追加、永不修改**。所以"编号 263 = `' the'`"这个含义一旦确定就永远不变。
2. **编号的数值顺序 = 训练时被选中的先后顺序**。257 比 300 先被选中，所以 257 优先级更高。编码时"找最小 rank"就靠这个。

### 3.4 `class Pretokenizer`（L92–161）

这是新版和旧版**最本质的差别**。旧版直接在整行字节上统计相邻对；新版先用 GPT-2 的正则把文本切成"片段（piece）"，**只在片段内部**做 BPE。这样 `" the"`（带前导空格）和 `"the"` 是两个不同的东西，空格位置和词边界都被保留。

#### 3.4.1 智能指针成员（L92–97）

```cpp
using RegexCode = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
using MatchData = std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;

RegexCode  regex{nullptr, &pcre2_code_free};
MatchData  match{nullptr, &pcre2_match_data_free};
```

PCRE2 是 C 库，句柄要手动 `free`。`unique_ptr<类型, 删除器类型>` 让它在析构时自动调对应函数（这叫 RAII）。`decltype(&函数)` 是"取这个函数指针的类型"。

`Pretokenizer` 析构时会自动释放，不会泄漏。

#### 3.4.2 正则（L120–121）

```cpp
R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)"
```

`R"( ... )"` 是**原始字符串字面量**：里面的反斜杠不用写两遍。等价于普通写法 `"'s|'t|...| ?\\p{L}+..."`。

逐段拆：

| 片段 | 含义 |
|---|---|
| `'s\|'t\|'re\|'ve\|'m\|'ll\|'d` | 英文缩写的后半截（GPT-2 把 `'s` 单独成片，不跟着前面的单词） |
| ` ?\p{L}+` | **可选的一个空格** + 一串 Unicode 字母。这就是"单词带前导空格"的来源 |
| ` ?\p{N}+` | 同上，数字 |
| ` ?[^\s\p{L}\p{N}]+` | 同上，标点/符号（非空白、非字母、非数字） |
| `\s+(?!\S)` | 一段空白，且后面没有非空白（即**文本末尾**的空白） |
| `\s+` | 兜底：任意一段空白 |

编译标志（L125）：`PCRE2_UTF`（按 UTF-8 解释）、`PCRE2_UCP`（让 `\p{L}`/`\p{N}` 认 Unicode）、`PCRE2_ANCHORED`（必须从起点开始匹配，见下）。
L130 `pcre2_jit_compile` 把它编译成机器码加速。

#### 3.4.3 `split`（L99–116）——连续切分

| 行 | 动作 |
|---|---|
| L101 | `bytes` = 文本首地址（PCRE2 要求 `PCRE2_SPTR` 类型） |
| L102 | `offset` = 已经切到哪了 |
| L103 | `while (offset < text.size())`：没切完就继续 |
| L105 | **只在第一刀做完整 UTF-8 校验**：`offset == 0` 时 options 为 0，之后加 `PCRE2_NO_UTF_CHECK` 跳过重复校验，省时间 |
| L106–107 | `pcre2_match(regex, bytes, 总长, offset, options, match, nullptr)`：从 `offset` 开始匹配 |
| L108–111 | `result < 0` → 抛异常（文本不是合法 UTF-8 会走到这里） |
| L112 | `range` = ovector，`range[0]`/`range[1]` 是匹配的起止**字节**位置 |
| L113 | `emit(text.data() + range[0], range[1] - range[0])` —— 传"指针 + 长度"，**不构造 `std::string`**，省一次堆分配 |
| L114 | `offset = range[1]`，从片段结尾继续 |

关键：**`PCRE2_ANCHORED`**（L125）+ `startoffset`（L106）保证"必须从 `offset` 开始"。于是每一刀紧接上一刀，**不留空隙也不重叠**；某个位置匹配不上就直接抛异常，绝不会悄悄跳过几个字节。

#### 3.4.4 `read`（L137–160）——逐行读 + 文档边界

两个回调参数：`emit`（产出一个普通片段）、`emit_special`（遇到特殊 token 时调用）。

| 行 | 动作 |
|---|---|
| L139 | `document`：还没切分的文本缓冲 |
| L141 | `while (std::getline(input, line))` 逐行读（`getline` 会吃掉行尾 `\n`） |
| L142 | `start`：本行内还没处理到的位置 |
| L143–144 | 在本行里反复找 `marker` |
| L145 | 把 marker **之前**那段追加进 `document` |
| L146–147 | 立刻 `split(document)` 切完，然后清空。**为什么必须切**：一个片段不能跨越特殊 token 存续 |
| L148 | `emit_special()` —— 通知调用方"这里有一个 `<\|endoftext\|>`" |
| L149 | `start` 跳过 marker |
| L151 | 本行剩下那段继续追加进 `document` |
| L152–154 | `if (!input.eof()) document.push_back('\n');` —— 补回被 `getline` 吃掉的换行；但**已到文件末尾且原文没有结尾换行时不补** |
| L156–158 | 循环结束后若不是正常 EOF，抛异常 |
| L159 | 把最后剩下的 `document` 切完 |

**L152 的技巧值得单独说**：`getline` 读到"最后一行且没有结尾换行"时会把 `eofbit` 置上；读到"中间某行"时 `eof` 还没置。所以它**用流状态反推原文有没有换行**。

四种情况（都已验证）：

| 文件内容 | 行为 | 结果 |
|---|---|---|
| `"a\nb\n"` | 两次 getline 都成功、eof 未置 → 各补 `\n` | `document = "a\nb\n"` ✓ |
| `"a\nb"`（无结尾换行） | 读 `b` 时 eof 被置 → 不补 | `document = "a\nb"` ✓ |
| 空文件 | 循环一次都不进 | 无输出 ✓ |
| 读错误（非 EOF） | L156 抛异常 | 不会静默丢数据 |

### 3.5 训练用的四个数据结构（L163–192）

| 行 | 结构 | 作用 |
|---|---|---|
| L163–166 | `WeightedPiece { std::vector<int> ids; Frequency occurrences; }` | 一个**片段**（如 `" the"` 的 `[32,116,104,101]`）+ 它在语料里出现多少次。相同片段只存一份，用 `occurrences` 记次数——这是训练能快起来的核心 |
| L168–171 | `PairStats { Frequency count; std::unordered_set<size_t> pieces; }` | 某个相邻对的**总频次** + **哪些片段包含它**（存片段下标）。有了 `pieces`，合并时只碰受影响的片段，不用全量重扫 |
| L173 | `using RankedPair = std::pair<Frequency, Pair>` | 排序键：先频次，后对 |
| L175–192 | `struct RankOrder` | 比较器：① 频次大的在前；② 频次相同，左 token 的**字符串**大的在前；③ 再比右 token 字符串；④ 最后比 `pair` 本身 |

**为什么 L184–188 比字符串而不比编号？** 要让"频次相同时谁先合并"完全确定、与语料出现的偶然顺序无关，这样同一份语料重复训练得到同一个模型（**可复现**）。

`RankOrder` 持有 `vocab` 的引用（L176），所以 `ranked` 必须是局部变量（L221），不能活得比 `vocab` 长。

### 3.6 `train`（L194–263）——四段式

#### 阶段 1：读语料、统计片段频次（L198–209）

```cpp
std::unordered_map<std::string, Frequency> frequencies;           // L199
pretokenizer.read(input, [&](const char* piece, std::size_t length) {
    ++frequencies[std::string(piece, length)];                    // L201
}, [] {});                                                        // L202
```

- L201：每来一个片段就计数。这里才把"指针 + 长度"变成 `std::string` 当 map 的键。
- **L202 的 `[] {}` 是 `emit_special`**：训练时遇到 `<\|endoftext\|>` 什么都不做。也就是说特殊 token 在训练中**被丢弃**（不产生相邻对，也不进统计）。

```cpp
for (const auto& entry : frequencies)
    if (entry.first.size() >= 2)                                  // L205
        pieces.push_back({byte_ids(entry.first), entry.second});   // L206
```

**L205 这个过滤很重要**：长度只有 1 字节的片段（单独的 `"a"`、单独的 `"\n"`）**不进入训练**。因为单字节内部没有"相邻对"，永远不可能是合并候选。它并不丢失——编码时会原样输出自己的字节编号。

#### 阶段 2：统计初始相邻对（L211–219）

```cpp
for (std::size_t i = 1; i < piece.ids.size(); ++i) {
    auto& entry = stats[{piece.ids[i - 1], piece.ids[i]}];        // L215
    entry.count += piece.occurrences;                             // L216
    entry.pieces.insert(index);                                   // L217
}
```

`count += occurrences`：如果 `" the"` 出现 500 次，那么 `(32,116)` 一次就加 500。
`pieces` 用集合去重：同一个片段里同一对出现多次，片段下标只记一次。

#### 阶段 3：建有序索引（L221–224）

```cpp
std::set<RankedPair, RankOrder> ranked{RankOrder{model.vocab}};
for (const auto& entry : stats)
    ranked.emplace(entry.second.count, entry.first);
```

`std::set` 是有序集合。**`ranked.begin()` 永远是"当前最该合并的那一对"**——一行 `begin()` 取代了"遍历找最大"的 O(n) 扫描。

#### 阶段 4：主循环（L226–261）

```cpp
while (static_cast<int>(model.vocab.size()) < target_size && !ranked.empty())
```

两个终止条件：词表达到目标大小，或没有可合并的对。

| 行 | 动作 | 为什么这样写 |
|---|---|---|
| L227 | `best = ranked.begin()->second` | 取当前最高频的对 |
| L228 | `new_id = vocab.size()` | 新编号 = 当前长度（257、258…） |
| L229 | `model.add_rule(best)` | 真的把规则加进模型（词表长 1，`vocab`/`rules`/`ranks` 同时更新） |
| L231 | `members = stats.at(best).pieces` | 拿到所有含这一对的片段下标 |
| L232 | `affected(members.begin(), members.end())` | **拷贝成 vector** ← 关键，见下 |
| L234 | `deltas`（`unordered_map<Pair,Frequency>`） | 先累加"每个对的频次变化量" |
| L237–241 | 每个受影响片段：旧的每一对 `-occurrences`，并从 `pieces` 里删掉该片段下标 | 先撤掉旧贡献 |
| L242 | `piece.ids = merge_once(piece.ids, best, new_id)` | 真正合并这个片段 |
| L243–247 | 合并后：每一对 `+occurrences`，并把片段下标插回 `pieces` | 再加新贡献 |
| L249–260 | 统一结算 `deltas` | 见下 |

**为什么 L232 必须拷贝？**
`members` 是 `stats`（一个 `unordered_map`）里某个元素的内部集合的引用。循环里 L245 会 `stats[pair]` 插入**新键**，哈希表一旦 rehash，所有元素的地址都会变，`members` 就悬空了。先拷贝成 `std::vector<size_t>` 是唯一安全做法。

**为什么要有 `deltas` 这一层？**
合并一对会同时改变该片段里其它很多对的频次。如果边改边动 `ranked`（红黑树）：① 每次 erase/insert 都是 O(log n)，次数翻好几倍；② 让集合在一段"批量改动"中间处于不一致状态，容易出 bug。所以**先记账、后结算**。

结算段逐行：

```cpp
auto& entry = stats.at(delta.first);            // L250
if (entry.count > 0)                            // L251
    ranked.erase({entry.count, delta.first});   // L252 按"旧频次"删掉旧条目
entry.count += delta.second;                    // L254 更新频次
if (entry.count == 0)                           // L255
    stats.erase(delta.first);                   // L256 归零的对彻底移除
else
    ranked.emplace(entry.count, delta.first);   // L258 用"新频次"重新插入
```

注意 L251 的条件：新建的对（`count` 还是 0）不在 `ranked` 里，不能去 erase。

#### 手工推演：`"abab\nab\n"` 完整训练过程（已实测）

预分词后：`"abab"`、`"\n"`、`"ab"`、`"\n"` → 过滤（L205）后只剩

| 片段下标 | 内容 | `occurrences` |
|---|---|---|
| 0 | `[97,98,97,98]` = `abab` | 1 |
| 1 | `[97,98]` = `ab` | 1 |

阶段 2 得到的 `stats`：

| 对 | `count` | `pieces` |
|---|---|---|
| `(97,98)` | **3** | `{0,1}` |
| `(98,97)` | 1 | `{0}` |

> 为什么是 3：片段 0 里 `(97,98)` 出现在位置 0–1 和 2–3，各 +1；片段 1 里 +1。

`ranked` = `{(3,(97,98)), (1,(98,97))}`。词表初始 257。

**第 1 轮**（`new_id = 257`）

| 步骤 | 结果 |
|---|---|
| L227 取最优 | `(97,98)`，频次 3 |
| L229 `add_rule` | `vocab[257] = "ab"`，词表 258 |
| L232 `affected` | `{0, 1}` |
| 片段 0 撤旧 | `(97,98) -= 1`（位置 1）、`(98,97) -= 1`（位置 2）、`(97,98) -= 1`（位置 3）→ 共 `(97,98) -= 2` |
| 片段 0 合并 | `[97,98,97,98]` → `[257,257]` |
| 片段 0 加新 | `(257,257) += 1` |
| 片段 1 撤旧 | `(97,98) -= 1` |
| 片段 1 合并 | `[97,98]` → `[257]`（没有相邻对了） |
| 结算 | `(97,98)`：3−3=0 → 删除；`(98,97)`：1−1=0 → 删除；`(257,257)`：0+1=1 → 入 `ranked` |

**第 2 轮**（`new_id = 258`）

| 步骤 | 结果 |
|---|---|
| L227 取最优 | `(257,257)`，频次 1 |
| L229 `add_rule` | `vocab[258] = "ab"+"ab" = "abab"`，词表 259 |
| 片段 0 撤旧 | `(257,257) -= 1` |
| 片段 0 合并 | `[257,257]` → `[258]` |
| 结算 | `(257,257)` 归零 → 删除；`ranked` 空 |

**第 3 轮**：`ranked` 为空 → 循环结束。最终 `vocab = 259`，`rules = 2`。

存盘内容（实测）：

```
mini-bpe-gpt2-v1
97 98
257 257
```

注意第二条是 `257 257` 而不是 `256 256` —— 因为 256 被 `<|endoftext|>` 占了。用这个模型编码 `"ababc\nabab\nba\n"`，实测输出：

```
258 99 10 258 10 98 97 10
```

（`258`=`abab`、`99`=`c`、`10`=`\n`、`98`=`b`、`97`=`a`）

### 3.7 `parse_integer`（L265–275）

| 行 | 动作 |
|---|---|
| L266 | `find_first_not_of("0123456789")`：出现任何非数字字符就报错。**同时排除了负号和空白** |
| L270 | `std::stoll` 转 64 位整数 |
| L271 | `parsed != value.size()`（没吃完全部字符）或超出 `int` 范围 → 报错 |

用途：`decode` 读 id、`load_model` 读规则里的两个数。

### 3.8 `save_model` / `load_model`（L277–311）

**存的是规则，不是词表**——因为词表可以由"256 个字节 + 规则按序重放"完全恢复（`add_rule` 干的事）。这是 BPE 的精髓：**词表是规则的产物**。

文件格式：

```
mini-bpe-gpt2-v1
104 101
32 116
...
```

| 行 | 细节 |
|---|---|
| L282 | 先写 header |
| L283–285 | 每条规则一行：`左编号 空格 右编号 换行` |
| L286–289 | `close()` 之后再检查流状态——**写盘错误只有 close 之后才能可靠发现** |
| L295–297 | 读第一行，不等于 header 就报错 |
| L299–306 | 每行用 `istringstream` 拆；`if (!(row >> left >> right) \|\| row >> extra)` 要求"正好两个字段" |
| L305 | `add_rule(...)` 重建模型。**`add_rule` 的校验顺便就是文件合法性检查**：编号越界、引用了 256、规则重复，全会被拦下 |

### 3.9 `encode_piece`（L313–336）——对**一个片段**做 BPE 编码

```cpp
for (std::size_t i = 0; i < length; ++i)
    ids.push_back(static_cast<unsigned char>(piece[i]));   // L316–319 字节→编号

for (;;) {
    int best_id = std::numeric_limits<int>::max();
    Pair best{};
    for (std::size_t i = 1; i < ids.size(); ++i) {          // L324
        const auto rule = model.ranks.find({ids[i-1], ids[i]});   // L325
        if (rule != model.ranks.end() && rule->second < best_id) { // L326
            best = rule->first;                              // L327
            best_id = rule->second;                          // L328
        }
    }
    if (best_id == std::numeric_limits<int>::max()) break;   // L331–333
    ids = merge_once(ids, best, best_id);                    // L334
}
```

**核心思想**：每轮在所有相邻对里找 **`ranks` 值最小**的那一对。`ranks` 里存的新编号就是"训练时第几个被选中"，数值越小 = 训练时越早 = 优先级越高。合并它，重新扫描，直到没有任何一对在模型里。

这和 GPT-2 原版 `encoder.py` 的 `bpe()` 是同一套逻辑：**每轮只合并一种对，且选择 rank 最小的**。结果是唯一确定的。

注意它不按位置从左到右、也不按出现次数，只看 rank。所以 `" girl"` 的合并顺序是 `ir` → ` gir` → ` girl`（见 5.3）。

### 3.10 缓存（L338–351）

```cpp
struct CachedPiece { std::string bytes; std::vector<int> ids; };  // L338–341
```

**记住某个片段的编码结果**。自然语言片段高度重复（`" the"` 出现几十万次），每个片段只需真正算一次。

`hash_piece`（L343–351）是 **FNV-1a 64 位哈希**：

```cpp
std::uint64_t hash = 14695981039346656037ULL;   // L345 偏移基数
hash ^= static_cast<unsigned char>(piece[i]);   // L347
hash *= 1099511628211ULL;                       // L348 FNV 质数
```

两个常数是 FNV 的固定值。它直接在 `(指针, 长度)` 上算，**不构造 `std::string`**，省一次堆分配。

### 3.11 `encode_tokens`（L353–385）——编码流水线

```cpp
constexpr std::size_t cache_size = 16384;        // L356
constexpr std::size_t cacheable_length = 64;     // L357
std::vector<CachedPiece> cache(cache_size);      // L360
```

| 行 | 分支 | 说明 |
|---|---|---|
| L364–367 | **长度 1** | 直接 `emit` 那个字节值。一个字节内部没有相邻对，不可能被合并，**根本不用查模型** |
| L370–376 | **长度 ≤ 64** | 查缓存：下标 = `hash % 16384`。L372 做**严格比对**（长度相同 + 内容相同）才敢用，因为这是"直接映射"缓存，不同片段可能落到同一槽 |
| L373–374 | 未命中 | 调 `encode_piece` 算，然后存进去 |
| L377–379 | **长度 > 64** | 不进缓存，直接算到 `scratch`。理由在 L358–359 注释里：**长片段如果缓存，任意输入就能把缓存无限撑大** |
| L381–383 | 统一 | 把 `ids` 指向的那串编号一个个 `emit` |

这是**固定大小、无链表、冲突直接覆盖**的缓存：内存恒定 16384 槽，查找 O(1)，代价是偶尔被覆盖后重算。对 BPE 这种高重复场景是最划算的取舍。

### 3.12 `encode` / `decode`（L387–406）

```cpp
void encode(const Model& model, std::istream& input, std::ostream& output) {
    Pretokenizer pretokenizer;                                   // L388
    encode_tokens(model, pretokenizer, input, [&](int id) {
        output << id << (id == special_id ? '\n' : ' ');          // L390
    });
}
```

- L390：普通编号后跟**空格**，特殊 token 后跟**换行**。所以 ids 文件的行边界只对应"文档边界"，**不对应原文的换行**。原文的换行是一个普通编号 `10`。
- `decode`（L394–406）：`while (input >> token)` 用 `>>` 自动跳过空格和换行 → L397 校验格式 → L398–400 校验范围 → L401 `output << model.vocab[id]` 拼回原文。

### 3.13 `run`（L408–448）——命令行

| 行 | 说明 |
|---|---|
| L409–410 | 两个加速开关：`sync_with_stdio(false)` 关掉 C++ 流与 C stdio 的同步；`cin.tie(nullptr)` 让 `cin` 读之前不强制 flush `cout` |
| L411 | 整个函数体在 `try` 里，异常统一在 L444 接住 |
| L413–415 | `train` 要 4 或 5 个参数 |
| L416 | 不给词表大小时默认 **10000** |
| L417–419 | 目标不能小于 257（装不下"256 字节 + 特殊 token"就没意义） |
| L420–423 | 用 `std::ios::binary` 打开——**Windows 上必须**，否则 `\r\n` 会被悄悄翻译成 `\n`，语料字节就变了 |
| L424–427 | 训练 → 存盘 → 往 **stderr** 报告 `Vocabulary: 10000 / 10000, merges: 9743` |
| L429–431 | 非 train 模式必须正好 3 个参数 |
| L432–437 | 读模型 → encode（cin→cout）或 decode |
| L438–441 | flush 之后再检查 `cout` 是否出错 |

---

## 4. C 层：`main` 与旧版功能（L453–665）

### 4.1 调度（L453–464）

| 行 | 动作 |
|---|---|
| L454 | `if (argc > 1)` |
| L455 | `mode = argv[1]` |
| L456 | 总闸门（见 1.3） |
| L457–458 | 有 PCRE2：转给 `corpus_bpe::run` |
| L459–461 | 没 PCRE2：打印 `Build with -DMINI_BPE_PCRE2 ...` 并返回 1 |
| L466 之后 | 其余所有分支 |

### 4.2 旧版 `decode`（L466–498）

| 行 | 动作 |
|---|---|
| L470–472 | `vocab` 先放 0–255 的单字节 |
| L473–477 | 打开硬编码路径 `"output/rules.txt"` |
| L479–481 | 每读一对规则就 `vocab.push_back(vocab[left] + vocab[right])`。**新编号就是 `vocab.size()`，从 256 开始** |
| L483–496 | 按**行**读空白分隔的编号 → 每行输出 `vocab[id]` 再补一个换行 |

这里的"文档"概念是"一行"，和 B 层"特殊 token 分隔"完全不同。

### 4.3 旧版 `encode`（L500–533）

| 行 | 动作 |
|---|---|
| L511–513 | 读规则进 `loaded_rules` |
| L516–520 | 每行 → 字节编号 |
| L521 | `int new_id = 256;` ← **旧编号体系的起点** |
| L522–525 | **按规则顺序**逐条 `merge_once`，每用一条 `new_id++` |
| L527–530 | 输出"编号 + 空格"，行尾换行 |

"按规则顺序逐条合并"和 B 层"反复找最小 rank"在效果上**等价**（规则顺序就是 rank 顺序）。差别：旧版每一轮都对整串做一次全扫描；新版每轮找当前最小 rank，天然按优先级推进。

### 4.4 `count`（L535–597）

| 行 | 动作 |
|---|---|
| L541 | 固定跑 **3 轮** |
| L542 | **每轮重新打开文件** |
| L549–562 | 每行 → 字节编号 → 用已有规则合并（`new_id` 又从 256 起，L558） |
| L564–570 | 统计所有相邻对，`std::map<pair,int>` |
| L573–583 | 找最大值。**`std::map` 按 key 升序遍历 + `>` 严格比较 → 同频次时"字典序最小的对"胜出** |
| L585–586 | 把 `左 右 次数` 打到 **stderr** |
| L587 | 把这一对追加进 `rules` |
| L588–590 | 没有可合并的就 `break` |
| L593–595 | 最后把规则列表打到 **stdout**（所以 `> output/rules.txt` 能直接存文件） |

实测（`output/count_input.txt` = `"abab\nab"`）：

```
stderr:  97 98 3
stderr:  256 256 1
stdout:  97 98
stdout:  256 256
```

与仓库里现成的 `output/rules.txt` 完全一致——**说明这份 rules.txt 就是它生成的**。

### 4.5 默认演示（L599–664）

| 行 | 动作 |
|---|---|
| L601–611 | 从 stdin 读**所有行**，每行一个 `doc`（一个 `vector<int>` 字节编号） |
| L615 | 跑 3 轮 |
| L618–627 | 统计所有 doc 的相邻对 |
| L631–638 | 找最大频次的对（同 4.4） |
| L641–644 | `new_id = 256 + round`，**对所有 doc 应用一次 `merge_once`** |
| L649 | 记录规则 |
| L653–655 | 规则打到 **stderr** |
| L657–662 | 合并后的编号打到 **stdout** |

实测输入 `"abab\nab\n"` → stderr 两条规则，stdout `257` / `256`，与 `output/train_ids.txt` 完全一致。

### 4.6 新旧两代对比

| 维度 | B 层（新） | C 层（旧） |
|---|---|---|
| 预分词 | GPT-2 正则（L121） | 无，直接从字节开始 |
| 文档边界 | `<\|endoftext\|>` + 行尾 `\n` | 一个输入行 |
| 首个合并编号 | 257 | 256 |
| 训练轮数 | 直到目标词表大小（上万个） | 固定 3 轮 |
| 规则保存 | header + 规则 → `save_model` | 直接两列数字 → `count` 的 stdout |
| 选对方式 | `std::set` + 严格全序（可复现） | `std::map` 遍历 + 严格大于（同频取小） |
| 编码方式 | 反复找最小 rank | 按规则顺序逐条扫 |
| 性能设计 | 片段频次聚合 + 差分更新 + 缓存 | 每次都全量重扫 |

---

## 5. 真实语料的完整旅程（实测）

### 5.1 环境与编译

```bash
# B 层（需要 PCRE2）
g++ -std=c++14 -O2 -DMINI_BPE_PCRE2 -Ioutput/pcre2-dev/usr/include \
    main.cpp -Wl,-l:libpcre2-8.so.0 -o mini_bpe

# C 层（只要标准库）
g++ -std=c++14 -O2 main.cpp -o mini_bpe_legacy
```

本文使用的模型：`output/tinystories_small_2026-11-22/tinystories.model`（header + 9743 条规则 → 词表 10000）。
语料：`data/TinyStoriesV2-GPT4-valid.txt`。

### 5.2 `train`：256 字节真实语料，12 步走完

输入 = valid.txt 前两行（一个完整小故事 242 字节 + `<\|endoftext\|>` 一行），共 **256 字节**。
命令：`./mini_bpe train snippet.txt model 300`

| 步 | 位置 | 发生了什么 |
|---|---|---|
| 1 | L454–463 | `argv[1] == "train"` → 进入 `corpus_bpe::run` |
| 2 | L413–423 | 参数检查、目标 300、binary 打开文件 |
| 3 | L424 | `train(input, 300)` |
| 4 | L196 | 构造 `Pretokenizer`：编译那条 GPT-2 正则 + JIT |
| 5 | L141 | `getline` 读第一行（242 字节的故事） |
| 6 | L143–150 | 行内找 `<\|endoftext\|>`：**没找到**，整行进 `document`，末尾补 `\n` |
| 7 | L141 | 读第二行 `"<\|endoftext\|>"` |
| 8 | L143–149 | 找到了！先把 `document`（故事）在 L146 切分，L147 清空，L148 调 `emit_special` |
| 9 | L200–202 | 片段回调：每个片段计数（`emit_special` 是空 lambda，marker 被丢弃） |
| 10 | L204–208 | 过滤掉长度 < 2 的片段（单字节、单独的 `\n`） |
| 11 | L211–224 | 统计初始相邻对 → 建 `ranked` |
| 12 | L226–261 | 主循环：取最高频对 → 加规则 → 更新受影响片段，**重复 43 次**（词表 257 → 300） |
| 13 | L425 / L282–289 | `save_model` 写 header + 43 行规则 |

第 12 步的 43 轮真实结果（`轮次: 新编号 = (左,右) 结果串`）：

```
 1: 257 = (104,101) 'h'+'e'        -> 'he'
 2: 258 = (32,116)  ' '+'t'        -> ' t'
 3: 259 = (32,115)  ' '+'s'        -> ' s'
 4: 260 = (110,100) 'n'+'d'        -> 'nd'
 5: 261 = (108,101) 'l'+'e'        -> 'le'
 6: 262 = (32,104)  ' '+'h'        -> ' h'
 7: 263 = (115,116) 's'+'t'        -> 'st'
 8: 264 = (111,117) 'o'+'u'        -> 'ou'
 9: 265 = (111,261) 'o'+'le'       -> 'ole'
10: 266 = (109,265) 'm'+'ole'      -> 'mole'
11: 267 = (102,101) 'f'+'e'        -> 'fe'
12: 268 = (258,257) ' t'+'he'      -> ' the'
13: 269 = (32,266)  ' '+'mole'     -> ' mole'
14: 270 = (32,97)   ' '+'a'        -> ' a'
15: 271 = (118,101) 'v'+'e'        -> 've'
16: 272 = (111,110) 'o'+'n'        -> 'on'
17: 273 = (105,116) 'i'+'t'        -> 'it'
18: 274 = (257,114) 'he'+'r'       -> 'her'
19: 275 = (101,100) 'e'+'d'        -> 'ed'
20: 276 = (99,97)   'c'+'a'        -> 'ca'
21: 277 = (98,101)  'b'+'e'        -> 'be'
22: 278 = (97,267)  'a'+'fe'       -> 'afe'
23: 279 = (84,257)  'T'+'he'       -> 'The'
24: 280 = (258,111) ' t'+'o'       -> ' to'
25: 281 = (259,111) ' s'+'o'       -> ' so'
26: 282 = (259,278) ' s'+'afe'     -> ' safe'
27: 283 = (262,105) ' h'+'i'       -> ' hi'
28: 284 = (262,97)  ' h'+'a'       -> ' ha'
29: 285 = (270,260) ' a'+'nd'      -> ' and'
30: 286 = (32,119)  ' '+'w'        -> ' w'
31: 287 = (32,108)  ' '+'l'        -> ' l'
32: 288 = (32,107)  ' '+'k'        -> ' k'
33: 289 = (32,274)  ' '+'her'      -> ' her'
34: 290 = (32,102)  ' '+'f'        -> ' f'
35: 291 = (32,100)  ' '+'d'        -> ' d'
36: 292 = (32,277)  ' '+'be'       -> ' be'
37: 293 = (32,279)  ' '+'The'      -> ' The'
38: 294 = (121,264) 'y'+'ou'       -> 'you'
39: 295 = (271,114) 've'+'r'       -> 'ver'
40: 296 = (295,121) 'ver'+'y'      -> 'very'
41: 297 = (117,263) 'u'+'st'       -> 'ust'
42: 298 = (116,261) 't'+'le'       -> 'tle'
43: 299 = (116,101) 't'+'e'        -> 'te'
```

**怎么读这张表**：

- 第 1 轮合并的是最高频的二元组 `he`；
- 第 9–10 轮最能说明 BPE 的"滚雪球"：先有 `ole`（用第 5 轮的 `le`），再有 `mole`（用 `ole`），再到第 13 轮 ` mole`（前导空格 + 整个词）；
- 第 12 轮 ` t` + `he` → ` the`：把"空格 + 词"合成一个 token——这就是 GPT-2 分词里单词带前导空格的原因；
- 第 23 轮 `T` + `he` → `The`：大写开头单独处理，和 ` the` 完全不同的 token。

对照 10k 词表的大模型，它的前 16 条规则是（从 `tinystories.model` 实读）：

```
1: 257 = (104,101) 'h'+'e'      -> 'he'
2: 258 = (32,116)  ' '+'t'      -> ' t'
3: 259 = (32,97)   ' '+'a'      -> ' a'
4: 260 = (32,115)  ' '+'s'      -> ' s'
5: 261 = (32,119)  ' '+'w'      -> ' w'
6: 262 = (110,100) 'n'+'d'      -> 'nd'
7: 263 = (258,257) ' t'+'he'    -> ' the'
8: 264 = (101,100) 'e'+'d'      -> 'ed'
9: 265 = (32,98)   ' '+'b'      -> ' b'
10: 266 = (258,111) ' t'+'o'    -> ' to'
11: 267 = (259,262) ' a'+'nd'   -> ' and'
12: 268 = (32,104)  ' '+'h'     -> ' h'
13: 269 = (32,102)  ' '+'f'     -> ' f'
14: 270 = (105,110) 'i'+'n'     -> 'in'
15: 271 = (32,84)   ' '+'T'     -> ' T'
16: 272 = (261,97)  ' w'+'a'    -> ' wa'
```

规律完全一致：**先高频二元组，再往上拼三元、四元，最后把"前导空格 + 词"整体吃掉。**

### 5.3 `encode`：一句真实语料 → 10 个编号

原句（真实语料，43 字节）：

> `The mole felt so safe with the little girl.`

| 步 | 位置 | 发生了什么 |
|---|---|---|
| 1 | L454–463 | `encode` + argc=3 → `corpus_bpe::run` |
| 2 | L429–432 | 读模型：header + 9743 条规则 → `Model`（`vocab` 长 10000，`ranks` 9743 项） |
| 3 | L434 | `encode(model, cin, cout)` |
| 4 | L388 | 建 `Pretokenizer` |
| 5 | L141 | `getline` 读到整句 |
| 6 | L152–154 | 没有结尾换行（eof 已置）→ 不补 `\n` |
| 7 | L159 | `split(document, emit)` |
| 8 | L103–115 | 正则连续切出 **10 个片段** |
| 9 | L363–383 | 每个片段：单字节？→ 查缓存？→ `encode_piece` |
| 10 | L321–335 | `encode_piece` 反复"找最小 rank 的对并合并" |
| 11 | L389–391 | `emit` → 输出"编号 + 空格" |

第 8 步切出的片段与第 10 步每一刀的合并过程（**推导值与程序输出逐个吻合**）：

| 片段 | 字节编号 | 合并过程（`左+右=新id`） | 结果 |
|---|---|---|---|
| `The` | 84,104,101 | 104+101=257 → 84+257=**406** | 406 |
| ` mole` | 32,109,111,108,101 | 32+109=281；108+101=303；281+111=743；743+303=**3441** | 3441 |
| ` felt` | 32,102,101,108,116 | 32+102=269；101+108=413；413+116=527；269+527=**534** | 534 |
| ` so` | 32,115,111 | 32+115=260；260+111=**389** | 389 |
| ` safe` | 32,115,97,102,101 | 32+115=260；260+97=292；102+101=732；292+732=**924** | 924 |
| ` with` | 32,119,105,116,104 | 32+119=261；105+116=274；274+104=325；261+325=**326** | 326 |
| ` the` | 32,116,104,101 | 104+101=257；32+116=258；258+257=**263** | 263 |
| ` little` | 32,108,105,116,116,108,101 | 105+116=274；32+108=277；108+101=303；274+116=374；374+303=390；277+390=**392** | 392 |
| ` girl` | 32,103,105,114,108 | 32+103=296；105+114=305；296+305=485；485+108=**488** | 488 |
| `.` | 46 | 无规则 → 原样 | 46 |

程序实际输出：

```
406 3441 534 389 924 326 263 392 488 46
```

**43 字节 → 10 个编号，约 4.3 字节/token。**

两个必须注意的细节：

1. **`The` 和 ` the` 走了不同路径**（406 vs 263），因为一个带前导空格、一个不带。这就是 L121 那条正则的意义——**空格是词的一部分**。
2. **` girl` 的合并顺序不是从左到右**：先合 `ir`，再合 ` gir`，最后 `+l`。因为 `encode_piece` 每轮挑的是**全局 rank 最小**的那一对，与位置无关。这是理解 BPE 编码的关键。

### 5.4 `decode`：还原

命令：`./mini_bpe decode model < ids.txt`

| 步 | 位置 | 动作 |
|---|---|---|
| 1 | L432 | `load_model`：0–256 基础词表 + 9743 条规则重放 |
| 2 | L436 | `decode(model, cin, cout)` |
| 3 | L396–397 | `>>` 逐个读整数（自动跳过空格/换行），`parse_integer` 校验格式 |
| 4 | L398–400 | 检查 `id < vocab.size()` |
| 5 | L401 | `output << vocab[id]` 拼字节 |

输出与原文**逐字节相同**。
整篇往返也验证过：256 字节片段 → 133 个编号 → 解码回来 `cmp` 通过（256 字节）。

### 5.5 特殊 token 的时间线（`a<|endoftext|>b\n`）

输入 3 个字符 + 换行，实际生成的 ids 文件内容是：

```
97 256
98 10
```

过程：

1. L141 `read` 读入 `a<|endoftext|>b`
2. L143 找到 marker → L145 把 `"a"` 放进 `document` → L146 `split("a")` → 输出 97 → L147 清空
3. L148 `emit_special()` → `encode_tokens` 的第四个参数（L384）`emit(special_id)` → 输出 **256 后面跟换行**（L390）
4. L151 剩下 `"b"` 追加进 `document`，L152–154 补 `\n`
5. L159 `split("b\n")` → 片段 `"b"` 和 `"\n"` → 输出 98、10

`decode` 读这四行得到的字节（`od -c` 实测）：

```
a < | e n d o f t e x t | > b \n
```

**与原文完全一致**——因为 256 号词条的内容就是 `"<|endoftext|>"` 这个字符串本身。
但注意：ids 文件里的那个换行**只是分隔符**，`decode` 用 `>>` 读时当空白跳过了；原文真正的换行是编号 10。

### 5.6 旧版四条路径的实测

| 命令 | 实测输出 | 与仓库文件对照 |
|---|---|---|
| `mini_bpe count output/count_input.txt` | stderr `97 98 3` / `256 256 1`；stdout `97 98` / `256 256` | = `output/rules.txt` |
| `mini_bpe`（stdin `abab\nab\n`） | stdout `257` / `256` | = `output/train_ids.txt` |
| `mini_bpe encode`（stdin `ababc\nabab\nba\n`） | `257 99` / `257` / `98 97` | = `output/ids.txt` |
| `mini_bpe decode`（stdin = ids.txt） | `ababc` / `abab` / `ba` | = `output/input.txt` |
| `mini_bpe encode MODEL`（无 PCRE2 编译） | `Build with -DMINI_BPE_PCRE2 ...`，exit 1 | — |

---

## 6. 变量总表

### 6.1 `merge_once`

| 变量 | 类型 | 作用 |
|---|---|---|
| `ids` | `const std::vector<int>&` | 输入编号序列（只读） |
| `best_pair` | `std::pair<int,int>` | 要合并的那一对 |
| `new_id` | `int` | 新编号 |
| `merged` | `std::vector<int>` | 输出缓冲 |
| `i` | `size_type` | 扫描位置 |
| `matches` | `bool` | 本位置是否命中 |

### 6.2 `Model`

| 变量 | 类型 | 生命周期 | 作用 |
|---|---|---|---|
| `vocab` | `vector<string>` | 全程 | 编号 → 字节串 |
| `rules` | `vector<Pair>` | 全程 | 有序合并规则（存盘内容） |
| `ranks` | `unordered_map<Pair,int,PairHash>` | 全程 | (左,右) → 新编号 |

### 6.3 `Pretokenizer`

| 变量 | 类型 | 作用 |
|---|---|---|
| `regex` | `unique_ptr<pcre2_code, 删除器>` | 编译好的正则 |
| `match` | `unique_ptr<pcre2_match_data, 删除器>` | 匹配结果容器（复用，避免每次分配） |
| `bytes`（`split`） | `const PCRE2_SPTR` | 文本首地址 |
| `offset` | `size_t` | 已切到的位置 |
| `options` | 无符号整数 | 是否为首次匹配（第一次做 UTF-8 校验） |
| `result` | `int` | 匹配是否成功 |
| `range` | `PCRE2_SIZE*` | 匹配的起止字节位置 |
| `document` | `string` | 跨行缓冲，遇到特殊 token 或文件尾才切分 |
| `line` / `start` / `pos` | `string` / `size_t` / `size_t` | 当前行、行内已处理位置、marker 位置 |

### 6.4 `train`

| 变量 | 类型 | 作用域 | 作用 |
|---|---|---|---|
| `model` | `Model` | 全程 | 正在训练的模型 |
| `pretokenizer` | `Pretokenizer` | 全程 | 预分词器 |
| `pieces` | `vector<WeightedPiece>` | 全程 | 去重后的片段表（带频次） |
| `frequencies` | `unordered_map<string,Frequency>` | 块内（L198–209） | 片段 → 出现次数，用完立即释放 |
| `stats` | `unordered_map<Pair,PairStats,PairHash>` | 全程 | 对 → (频次, 含它的片段下标集合) |
| `ranked` | `set<RankedPair,RankOrder>` | 全程 | 有序候选，`begin()` 即最优对 |
| `best` | `Pair` | 每轮 | 本轮胜出的对 |
| `new_id` | `int` | 每轮 | 新编号 = 当前词表长度 |
| `members` | `const unordered_set<size_t>&` | 每轮 | 含该对的片段下标（**只用于拷贝**） |
| `affected` | `vector<size_t>` | 每轮 | 拷贝后的下标列表 |
| `deltas` | `unordered_map<Pair,Frequency,PairHash>` | 每轮 | 本轮频次变化量 |

### 6.5 编码相关

| 变量 | 类型 | 作用 |
|---|---|---|
| `ids`（`encode_piece`） | `vector<int>` | 当前编号序列，反复被合并 |
| `best_id` / `best` | `int` / `Pair` | 本轮要合并的对与它的 rank |
| `cache` | `vector<CachedPiece>` | 16384 槽的直接映射缓存 |
| `scratch` | `vector<int>` | 长片段的临时缓冲（避免每次分配） |
| `entry` | `CachedPiece&` | 命中的缓存槽 |
| `cache_size` / `cacheable_length` | `constexpr size_t` | 16384 / 64 |

### 6.6 其他

| 模块 | 变量 | 作用 |
|---|---|---|
| `decode` | `token` / `id` | 逐个编号还原 |
| `run` | `mode` / `target_size` / `input` / `model` | 命令行分发 |
| 旧 `decode` | `vocab` / `left` / `right` / `id_line` / `id` | 重建词表并逐行输出 |
| 旧 `encode` | `loaded_rules` / `encoded` / `new_id` | 按规则顺序合并 |
| `count` | `doc_counts` / `best_pair` / `best_pair_number` / `round` | 每轮统计与选对 |
| 默认演示 | `docs` / `doc_ids` / `rules` / `round` | 一次读全、逐轮合并 |

---

## 7. 易错点清单

1. **`256` 到底是谁**。B 层：256 是 `<|endoftext|>`，257 才是第一个合并；旧版：256 就是第一个合并。混用必炸（`add_rule` 会抛异常）。
2. **ids 文件的换行 ≠ 原文的换行**。L390 只在 `special_id` 后面写换行；原文换行是编号 10。
3. **长度 < 2 的片段在训练时被丢弃**（L205），但编码时照常输出（L364–367）。训练没见过的单字节不会丢。
4. **`add_rule` 的 `id` 不是参数**，而是 `vocab.size()`（L80）。编号是"数出来的"。
5. **`encode_piece` 每轮只合并一种对，且按 rank（编号）最小挑**，不是按位置、不是按出现次数（L325–328）。
6. **L232 那个拷贝不能省**。`stats` 是 `unordered_map`，L245 插入新键可能 rehash，`members` 引用会悬空。
7. **旧版 encode/decode 读硬编码 `output/rules.txt`**（L473、L502），且只有 **argc == 2** 时才进得去（L456 的 `argc > 2` 把它们挡在门外）。
8. **`\s+(?!\S)` 那一支**（L121）专门处理"文本末尾的空白"。少了它，结尾空白可能被前面的分支吃掉，片段边界就变了。
9. **`merge_once` 不重叠**。`[97,97,97]` 用 `(97,97)` 合并得到 `[256,97]`，不是 `[256,256]`。
10. **`byte_ids` 必须用 `unsigned char`**（L61），否则高位字节会变成负编号。
11. **`std::ios::binary` 不能省**（L420），否则 Windows 上 `\r\n` 会被悄悄改写。
12. **`read` 的 `!input.eof()` 技巧**（L152）用流状态反推原文有没有结尾换行。改成无条件补 `\n`，所有文件的字节数都会变。

---

## 8. 自测题

> 先自己想，再看答案。

**Q1** 训练时为什么要把片段"去重 + 记频次"，而不是把语料里每个片段都当成一条记录？

**Q2** `ranked` 为什么用 `std::set` 而不是每次遍历 `stats` 找最大值？

**Q3** `encode_piece` 为什么不直接把整句文本丢给 `merge_once` 循环，而要先预分词？

**Q4** 如果目标词表设成 200，程序会怎样？

**Q5** 为什么 `Model` 只存 `rules` 就能恢复完整的 `vocab`？

**Q6** 一个片段长度 1 时，`encode_tokens` 为什么不查模型？（如果查了会得到什么？）

**Q7** 为什么训练时把 `<|endoftext|>` 丢弃是安全的？

**Q8** `count` 模式和 `train` 模式在"同频次时选哪个对"上规则不同，会造成什么可见差异？

---

**答案**

**A1** 因为同一个片段在语料里出现几十万次，而"相邻对频次"只和片段的**内容 + 出现次数**有关。去重后，200 万个唯一片段就能代表 20 亿字节的统计量，内存从 O(语料大小) 降到 O(唯一片段数)。

**A2** 因为 `std::set` 维护了顺序，`begin()` 就是最大值，取最优 O(1)；而每轮都遍历 `stats` 是 O(不同对数)，每轮都要做一次，训练上万轮就放大上万倍。

**A3** 因为 BPE 的语义是"不在片段之间合并"。`" the"` 和 `"the"` 必须是不同 token，否则空格信息就丢了。GPT-2 的一致性也依赖于同一个正则。

**A4** L417–419 会抛异常：`VOCAB_SIZE must be at least 257`，进程以 exit code 1 结束。

**A5** 因为 `add_rule` 里新词条 = `vocab[左] + vocab[右]`（L86），而 0–255 和 256 是固定的。只要按原顺序重放规则，就能精确重建每一层字符串。

**A6** 因为单字节内部不存在"相邻对"，任何规则都不可能在它身上生效，结果必然是它自己。查了也是同样的结果，只是白花一次哈希查找。

**A7** 因为特殊 token 本来就不该被 BPE 切碎或合并——它是一个不可分割的原子符号。训练时它不产生相邻对（`emit_special` 是空 lambda），词表里它的内容也已经固定为 L47 的字符串。

**A8** `train` 用 `RankOrder`（字符串降序）做全序，同一份语料重复训练结果完全一致；`count` 遍历 `std::map`（key 升序）+ 严格大于，同频次时取**字典序最小**的对。所以同样语料上两者产出的规则顺序/内容可能不同，写进文件的编号也不同。

---

## 9. 复现命令

### 9.1 编译

```bash
# 新版（B 层）：需要 PCRE2
g++ -std=c++14 -O2 -Wall -Wextra -pedantic \
    -DMINI_BPE_PCRE2 -Ioutput/pcre2-dev/usr/include \
    main.cpp -Wl,-l:libpcre2-8.so.0 -o mini_bpe

# 旧版（C 层）：只要标准库
g++ -std=c++14 -O2 -Wall -Wextra main.cpp -o mini_bpe_legacy
```

### 9.2 训练 + 编码 + 解码

```bash
./mini_bpe train data/TinyStoriesV2-GPT4-train.txt model.bin 10000
# stderr: Vocabulary: 10000 / 10000, merges: 9743

./mini_bpe encode model.bin < input.txt > ids.txt
./mini_bpe decode model.bin < ids.txt   > decoded.txt
cmp input.txt decoded.txt && echo ROUNDTRIP_OK
```

### 9.3 旧版四条路径

```bash
./mini_bpe_legacy count output/count_input.txt > output/rules.txt
printf 'abab\nab\n'        | ./mini_bpe_legacy            # 默认演示
printf 'ababc\nabab\nba\n' | ./mini_bpe_legacy encode       # argc==2
./mini_bpe_legacy decode < output/ids.txt                   # argc==2
```

### 9.4 阅读顺序建议

如果只想抓重点，按这个顺序读：

1. **L9–27** `merge_once` —— 先搞清"合并一次"到底做了什么。
2. **L313–336** `encode_piece` —— 编码的全部逻辑就这一个函数。
3. **L194–263** `train` —— 训练主循环，配合 3.6 的手工推演看。
4. **L137–160** `read` —— 文档边界与特殊 token，最容易忽略但影响最大。
5. **L453–464** `main` —— 搞清你敲的命令到底进了哪一段。

---

*本文档基于 `main.cpp`（665 行）与仓库 `output/` 下的真实产物整理。所有标"实测"的数字均可按第 9 章命令复现。*
