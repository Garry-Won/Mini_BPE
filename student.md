# 全量 TinyStories 分词

你会用完整训练集训练 10,000 项词表，再用保存的模型处理完整验证集。训练学习规则，编码把文本变成 token ID，解码用同一模型把编号还原为文本。完成时要有模型、验证集编号和解码文件，并确认解码文件与原文逐字节一致。

以下命令都在**同一个 Ubuntu（WSL）终端**中按顺序运行。每一步的检查通过后再继续。

## 1. 确认输入并新建输出目录

```bash
cd "/mnt/e/SJTU_Professional Progress/AI_Infra_Learning/Project/Mini_BPE" &&
train=data/TinyStoriesV2-GPT4-train.txt &&
valid=data/TinyStoriesV2-GPT4-valid.txt &&
stat -Lc '%s bytes  %n' "$train" "$valid" &&
test -r output/pcre2-dev/usr/include/pcre2.h &&
df -h . &&
free -h &&
run="output/tinystories_full_$(date +%Y%m%d_%H%M%S)" &&
mkdir -p output &&
mkdir "$run" &&
printf '本次输出目录：%s\n' "$run"
```

本地完整文件应分别为 **2,227,753,162 字节**和 **22,502,601 字节**。确认文件名和大小，再看可用磁盘、内存以及最后打印的目录。后续命令用同一个 `$run` 保存本次产物。若中途关闭终端，重新进入项目目录后，按这里的路径恢复 `$train`、`$valid` 和 `$run` 三个变量。

## 2. 编译全量语料入口

```bash
g++ -std=c++14 -O2 -Wall -Wextra -pedantic -Werror \
    -DMINI_BPE_PCRE2 -Ioutput/pcre2-dev/usr/include \
    main.cpp -Wl,-l:libpcre2-8.so.0 -o "$run/mini_bpe_cs336" &&
test -x "$run/mini_bpe_cs336" &&
printf '语料分词器编译通过\n'
```

本机已解出的 PCRE2 头文件提供 GPT‑2 正则所需的 Unicode 支持。看到编译通过提示后进入训练。

## 3. 用完整训练集训练并保存模型

```bash
/usr/bin/time -f 'elapsed_seconds=%e\npeak_rss_kib=%M' \
    -o "$run/train.time" \
    "$run/mini_bpe_cs336" train "$train" "$run/tinystories.model.part" 10000 \
    2> "$run/train.log" &&
grep -Fx 'Vocabulary: 10000 / 10000, merges: 9743' "$run/train.log" &&
test "$(head -n 1 "$run/tinystories.model.part")" = 'mini-bpe-gpt2-v1' &&
test "$(wc -l < "$run/tinystories.model.part")" -eq 9744 &&
mv "$run/tinystories.model.part" "$run/tinystories.model" &&
cat "$run/train.time"
```

训练中消息写入日志，终端可能暂时没有输出。完成后应看到 10,000 项和 9,743 条合并。模型共有 9,744 行，其中一行是格式标识。训练日志和模型检查通过后，`.part` 临时文件才改为正式文件。记录 `train.time` 中的耗时（秒）和峰值常驻内存（KiB）。命令失败时先查看 `train.log` 和 `train.time`。

## 4. 用同一模型编码并解码完整验证集

```bash
"$run/mini_bpe_cs336" encode "$run/tinystories.model" \
    < "$valid" > "$run/valid.ids.txt.part" 2> "$run/encode.log" &&
test -s "$run/valid.ids.txt.part" &&
mv "$run/valid.ids.txt.part" "$run/valid.ids.txt" &&
"$run/mini_bpe_cs336" decode "$run/tinystories.model" \
    < "$run/valid.ids.txt" > "$run/valid.decoded.txt.part" 2> "$run/decode.log" &&
cmp "$valid" "$run/valid.decoded.txt.part" &&
mv "$run/valid.decoded.txt.part" "$run/valid.decoded.txt" &&
printf '完整验证集逐字节往返通过\n'
```

`encode` 从标准输入读原文，`decode` 从标准输入读编号。两者通过命令参数加载同一模型。`cmp` 会连同空格、换行和末尾字节一起比较。若出现错误，分别读 `encode.log` 或 `decode.log`。`cmp` 有差异时会指出第一个不同的位置。

## 5. 记录验收证据

```bash
tokens=$(wc -w < "$run/valid.ids.txt") &&
markers=$(LC_ALL=C grep -aoF '<|endoftext|>' "$valid" | wc -l) &&
special_ids=$(LC_ALL=C awk '{for (i = 1; i <= NF; ++i) if ($i == "256") ++n} END {print n + 0}' "$run/valid.ids.txt") &&
test "$markers" -gt 0 &&
test "$markers" -eq "$special_ids" &&
printf '验证集 token 数：%s\n原文结束标记数：%s\n特殊 ID 256 数：%s\n' "$tokens" "$markers" "$special_ids" &&
sha256sum "$run/tinystories.model" "$valid" "$run/valid.decoded.txt"
```

最后两个 SHA-256 值应相同。把**输出目录、训练耗时、峰值内存、验证集 token 数和逐字节比较结果**发回，并试着用一句话解释为什么训练读 train，往返验收读 valid。

如果你还要保存完整训练集的 token ID，可在通过上述验收后用同一模型执行下面的编码。先用 `df -h .` 查看磁盘空间，文本编号文件可能大于原始输入。

```bash
"$run/mini_bpe_cs336" encode "$run/tinystories.model" \
    < "$train" > "$run/train.ids.txt.part" 2> "$run/encode_train.log" &&
test -s "$run/train.ids.txt.part" &&
mv "$run/train.ids.txt.part" "$run/train.ids.txt" &&
ls -lh "$run/train.ids.txt"
```

---

# Mini BPE 编码性能测试

下面两套命令都在 Ubuntu（WSL）终端执行，使用现有 10,000 项词表和完整 TinyStories 验证集。想测当前版本的命令行耗时与内存，运行第一套。想区分纯编码和命令行耗时，并比较优化前后与 GPT-2，运行第二套。

## 1. 测当前 Mini BPE 的命令行性能

```bash
(
    set -euo pipefail
    cd "/mnt/e/SJTU_Professional Progress/AI_Infra_Learning/Project/Mini_BPE"
    valid=data/TinyStoriesV2-GPT4-valid.txt
    model=output/tinystories_full_20261122_101703/tinystories.model
    run=$(mktemp -d output/tinystories_cli_bench_XXXXXX)
    test -r "$valid"
    test -r "$model"

    g++ -std=c++14 -O2 -Wall -Wextra -pedantic -Werror \
        -DMINI_BPE_PCRE2 -Ioutput/pcre2-dev/usr/include \
        main.cpp -Wl,-l:libpcre2-8.so.0 -o "$run/mini_bpe"

    "$run/mini_bpe" encode "$model" < "$valid" > /dev/null

    for i in 1 2 3 4 5; do
        /usr/bin/time -f '%e\t%U\t%S\t%M' -a -o "$run/raw.tsv" \
            "$run/mini_bpe" encode "$model" < "$valid" > /dev/null
    done

    printf '耗时(s)\t用户CPU(s)\t系统CPU(s)\t峰值RSS(KiB)\n'
    cat "$run/raw.tsv"
    median=$(LC_ALL=C sort -nk1,1 "$run/raw.tsv" | awk 'NR==3 {print $1}')
    LC_ALL=C awk -v t="$median" -v bytes="$(stat -c %s "$valid")" \
        'BEGIN {printf "中位耗时 %.2f s，吞吐量 %.2f MiB/s\n", t, bytes / 1048576 / t}'
    printf '结果目录 %s\n' "$run"
)
```

先预热一次，再记录五次。总耗时覆盖进程启动、模型加载、输入读取、分词和编号格式化。输出送到 `/dev/null`，所以这里测的是丢弃编号输出时的命令行性能。RSS 是进程的峰值驻留内存。

## 2. 复跑纯编码与命令行的对照测试

```bash
(
    set -euo pipefail
    cd "/mnt/e/SJTU_Professional Progress/AI_Infra_Learning/Project/Mini_BPE"
    bench=output/tinystories_optimization_2026-11-22
    run=$(mktemp -d output/tinystories_compare_bench_XXXXXX)
    cp "$bench/main.before.cpp" "$run/"
    printf '结果目录 %s\n' "$run"
    .venv/bin/python -B -u "$bench/benchmark.py" "$run" 2>&1 |
        tee "$run/terminal.log"
    test -s "$run/raw.csv"
    test -s "$run/summary.json"
)
```

脚本编译当前源码和优化前快照，并调用本地 GPT-2 分词器，三端使用同一份完整验证文本。纯编码先加载输入和模型，正式测 9 轮。命令行测试测 5 轮，将编号写入 `/dev/shm`（内存文件系统）。两组命令的输出去向不同，分别比较各自的结果。看 `summary.json` 中的中位耗时和吞吐量，逐轮记录在 `raw.csv` 中。
