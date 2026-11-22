# Mini BPE

Mini BPE 是一个用 C++14 实现的字节级 BPE（Byte Pair Encoding，字节对编码）分词器。它支持从 UTF-8 语料训练词表、保存和加载模型，以及通过命令行编码和解码。主流程采用 GPT-2 风格的 Unicode 预分词，使用 TinyStories 训练自己的词表和合并规则。

## 项目结构

| 路径 | 用途 |
| --- | --- |
| `README.md` | 项目入口，说明文件结构、代码职责、运行方法和实测结果 |
| [main.cpp](main.cpp) | C++ 核心实现，包含语料级训练、模型读写、编解码，以及早期教学模式 |
| [gpt2_tokenizer.py](gpt2_tokenizer.py) | GPT-2 对照入口。调用 `tiktoken`，从标准输入读取 UTF-8 文本，输出 token ID，词表缓存默认位于 `output/gpt2_tokenizer_cache/` |
| [integrate_corpora.py](integrate_corpora.py) | 早期全量语料批处理脚本。调用 `output/mini_bpe_full`，分别处理 TinyStories 和 OpenWebText，用 `cmp` 验证往返并记录 SHA-256。依赖旧二进制和两套语料，报告字段采用早期三轮合并口径。当前 TinyStories 流程见下方命令 |
| [Mini_BPE_Strcture.md](Mini_BPE_Strcture.md) | `main.cpp` 逐段精读，包含数据结构、命令分发、手工推演和边界条件 |
| [student.md](student.md) | 学生操作手册，包含全量训练、验证集往返和编码性能复测步骤 |
| [teacher.md](teacher.md) | 教学路线、阶段验收和实验记录，包含全量训练、GPT-2 对照与优化复测 |
| [.gitignore](.gitignore) | Git 忽略规则，排除根目录二进制 `mini_bpe`、`mini_bpe.exe` 和本地目录 `data/`、`output/` |
| [.vscode/launch.json](.vscode/launch.json) | 本地 VS Code 调试配置占位，当前配置列表为空 |

### 本地数据与产物

| 路径 | 内容和用途 |
| --- | --- |
| `data/TinyStoriesV2-GPT4-{train,valid}.txt` | 全量训练语料约 2.23 GB，验证语料约 22.5 MB。原始文本可从 [TinyStories 数据集](https://huggingface.co/datasets/roneneldan/TinyStories/tree/main) 获取 |
| `output/tinystories_small_2026-11-22/` | 固定小样本、模型、ID、解码文件及验收报告 `report.json` |
| `output/tinystories_full_20261122_101703/` | 全量模型 `tinystories.model`、训练日志与资源记录、验证集 ID 和解码文件 |
| `output/tinystories_benchmark_2026-11-22/` | 初次 GPT-2 对照。`benchmark.py` 构建临时基准并测量纯编码和命令行耗时，`raw.csv`、`summary.json` 保存结果 |
| `output/tinystories_optimization_2026-11-22/` | 编码优化复测。`main.before.cpp` 保存优化前源码，`benchmark.py` 轮换运行优化前、优化后与 GPT-2 三组，`summary.json` 和 `regression.log` 记录性能和回归结果 |
| `output/tinystories_cli_bench_wlb1qo/` | 命令行临时基准的二进制和原始计时表 |
| `output/full_corpus/`、`output/corpus_inventory.json` | 早期语料接入的模型规则、往返产物、日志及语料清单 |
| `output/gpt2_tokenizer_cache/`、`output/pcre2-dev/` | GPT-2 对照所用的词表缓存，以及本地解包的 PCRE2 开发文件 |
| `output/` 根目录其他文件 | 包含小样例输入、编号、规则、解码文本及本地编译的可执行文件。`output/rules.txt` 供教学版编解码读取 |
| `.venv/`、`mini_bpe`、`mini_bpe.exe` | 本地 Python 虚拟环境及 Linux、Windows 构建产物 |

## 核心代码

训练流程为 **语料 → 正则预分词 → 加权统计相邻对 → 按频率合并 → 保存模型**。使用模型时，编码按规则优先级生成 ID，解码按 ID 查词表并拼接原始字节。

| 代码 | 职责 |
| --- | --- |
| `merge_once`、`byte_ids` | 将片段转为无符号字节 ID，并对指定相邻对从左到右执行无重叠合并 |
| `Pretokenizer`、`RegexCode`、`MatchData` | 通过 PCRE2 的 Unicode 正则生成片段，管理正则和匹配资源，并在 `<\|endoftext\|>` 处分隔文档 |
| `Model`、`PairHash` | 管理词表、合并规则及规则优先级。字节 ID 为 0–255，结束标记为 256，合并 ID 从 257 开始 |
| `WeightedPiece`、`PairStats`、`RankOrder`、`train` | 对重复片段去重并加权，以相邻对到片段的索引定位受影响项，每轮增量更新频次。同频时按 token 字节串对的字典序决定先后 |
| `parse_integer`、`save_model`、`load_model` | 解析整数参数，保存带 `mini-bpe-gpt2-v1` 文件头的合并规则，并在加载时校验规则、重建词表 |
| `encode_piece`、`CachedPiece`、`hash_piece`、`encode_tokens` | 按模型优先级合并。单字节直接输出，重复短片段通过固定容量缓存复用编码结果，缓存命中时校验原始字节 |
| `encode`、`decode`、`run`、`main` | 处理命令分发、输入输出和异常。默认演示、`count INPUT` 及无模型参数的编解码属于教学模式，合并 ID 从 256 开始 |

`gpt2_tokenizer.py` 中的 `load_gpt2` 加载 GPT-2 编码，`main` 读取原文并分批写出 ID。早期的 `integrate_corpora.py` 用 `sha256` 分块计算校验值，用 `transform` 调用编解码并写入临时文件，`main` 负责训练、逐字节比较和更新报告。

## 编译与快速验证

在 Ubuntu 或 WSL 的 Ubuntu 终端中进入项目根目录，安装 `g++` 和 `libpcre2-dev` 后运行以下命令。

```bash
mkdir -p output
g++ -std=c++14 -O2 -Wall -Wextra -pedantic -DMINI_BPE_PCRE2 \
    main.cpp -lpcre2-8 -o mini_bpe

printf '%s' 'hello hello<|endoftext|>你好' > output/demo.txt
./mini_bpe train output/demo.txt output/demo.model 300
./mini_bpe encode output/demo.model < output/demo.txt > output/demo.ids
./mini_bpe decode output/demo.model < output/demo.ids > output/demo.decoded.txt
cmp output/demo.txt output/demo.decoded.txt && echo ROUNDTRIP_OK
```

`train` 的目标词表默认为 10,000，最小为 257。可合并的候选对耗尽时提前停止。全量复现时，将训练输入替换为 `data/TinyStoriesV2-GPT4-train.txt`，目标词表设为 10000，再用训练好的模型对 `data/TinyStoriesV2-GPT4-valid.txt` 编码、解码及比较。

## 实测结果

TinyStories 全量训练得到 **10,000 项词表、9,743 条合并规则**，训练用时 256.80 秒，进程峰值 RSS（常驻内存）约 28 MiB。完整验证集得到 **5,465,883 个 token**，其中 **27,630 个**为结束标记，解码通过逐字节比较。

下表来自同一份 22.5 MB 验证集上的优化复测。测试环境为 Ubuntu WSL 2、Intel Core i7-10870H，单线程固定逻辑 CPU，纯编码各测 9 轮，命令行各测 5 轮。纯编码在加载模型和输入后计时，命令行计时包含进程启动、模型加载、读取及 ID 写出。

| 指标 | Mini BPE 优化前 | Mini BPE 优化后 | GPT-2（tiktoken 0.14.0） |
| --- | ---: | ---: | ---: |
| 纯编码中位耗时 | 4.8139 s | 1.0583 s | 2.4759 s |
| 纯编码吞吐量 | 4.46 MiB/s | 20.28 MiB/s | 8.67 MiB/s |
| 命令行中位耗时 | 6.6061 s | 2.7416 s | 4.4330 s |

优化后纯编码和命令行分别达到优化前的 **4.55 倍**和 **2.41 倍**吞吐量，完整验证集的输出 ID 保持一致。GPT-2 一列使用其独立词表和 Python 对照入口。
