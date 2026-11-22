"""Encode UTF-8 from stdin with the GPT-2 tokenizer and write token IDs."""

import os
from pathlib import Path
import sys

import tiktoken


END_OF_TEXT = "<|endoftext|>"
CACHE_DIR = Path(__file__).resolve().parent / "output" / "gpt2_tokenizer_cache"


def load_gpt2() -> tiktoken.Encoding:
    os.environ.setdefault("TIKTOKEN_CACHE_DIR", str(CACHE_DIR))
    return tiktoken.get_encoding("gpt2")


def main() -> None:
    tokenizer = load_gpt2()
    text = sys.stdin.buffer.read().decode("utf-8")
    ids = tokenizer.encode(text, allowed_special={END_OF_TEXT})
    for start in range(0, len(ids), 8192):
        print(" ".join(map(str, ids[start : start + 8192])))


if __name__ == "__main__":
    main()
