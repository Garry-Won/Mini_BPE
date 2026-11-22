from pathlib import Path
import hashlib
import json
import subprocess


ROOT = Path(__file__).resolve().parent
BINARY = ROOT / "output" / "mini_bpe_full"
OUTPUT = ROOT / "output" / "full_corpus"
CORPORA = (
    ("tinystories", "TinyStoriesV2-GPT4-train.txt", "TinyStoriesV2-GPT4-valid.txt"),
    ("openwebtext", "owt_train.txt", "owt_valid.txt"),
)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(4 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def transform(mode, source, destination, rules):
    temporary = destination.with_suffix(destination.suffix + ".part")
    error_log = OUTPUT / (destination.name + ".stderr.log")
    try:
        with source.open("rb") as input_file, temporary.open("wb") as output_file:
            with error_log.open("wb") as errors:
                subprocess.run(
                    [str(BINARY), mode, str(rules)],
                    stdin=input_file,
                    stdout=output_file,
                    stderr=errors,
                    cwd=ROOT,
                    check=True,
                )
        temporary.replace(destination)
    except BaseException:
        temporary.unlink(missing_ok=True)
        raise


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    inventory_path = ROOT / "output" / "corpus_inventory.json"
    inventory = json.loads(inventory_path.read_text(encoding="utf-8"))
    sources = {Path(item["source"]).name: item for item in inventory["sources"]}
    report = {
        "scope": "Full training files and full validation files",
        "training_round_limit": 3,
        "input_unit": "physical line",
        "special_tokens": "endoftext marker is processed as ordinary bytes",
        "final_newline": "preserved",
        "binary_sha256": sha256(BINARY),
        "cpp_source_sha256": sha256(ROOT / "main.cpp"),
        "compiler_flags": "-std=c++14 -O2 -g -Wall -Wextra -pedantic -Werror -D_GLIBCXX_ASSERTIONS",
        "corpora": [],
    }
    for name, train_name, valid_name in CORPORA:
        train_source = ROOT / "data" / train_name
        valid_source = ROOT / "data" / valid_name
        rules = OUTPUT / (name + ".rules.txt")
        temporary_rules = rules.with_suffix(".txt.part")
        print(f"Training on full {name}: {train_source.stat().st_size} bytes", flush=True)
        try:
            with (OUTPUT / (name + ".train.log")).open("wb") as log:
                subprocess.run(
                    [str(BINARY), "train", str(train_source), str(temporary_rules)],
                    stdout=log,
                    stderr=log,
                    cwd=ROOT,
                    check=True,
                )
            temporary_rules.replace(rules)
        except BaseException:
            temporary_rules.unlink(missing_ok=True)
            raise

        ids = OUTPUT / (name + ".valid.ids.txt")
        decoded = OUTPUT / (name + ".valid.decoded.txt")
        print(f"Encoding full {name} validation file", flush=True)
        transform("encode", valid_source, ids, rules)
        print(f"Decoding full {name} validation file", flush=True)
        transform("decode", ids, decoded, rules)
        subprocess.run(["cmp", "-s", str(valid_source), str(decoded)], check=True)
        entry = {
            "name": name,
            "training_source": sources[train_name],
            "validation_source": sources[valid_name],
            "rules_file": str(rules.relative_to(ROOT)),
            "rules_sha256": sha256(rules),
            "rules": [list(map(int, row.split())) for row in rules.read_text().splitlines()],
            "validation_ids_file": str(ids.relative_to(ROOT)),
            "validation_ids_bytes": ids.stat().st_size,
            "decoded_file": str(decoded.relative_to(ROOT)),
            "decoded_bytes": decoded.stat().st_size,
            "byte_comparison_passed": True,
        }
        report["corpora"].append(entry)
        report_file = OUTPUT / "integration.json"
        temporary_report = report_file.with_suffix(".json.part")
        temporary_report.write_text(
            json.dumps(report, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        temporary_report.replace(report_file)
        print(f"Verified full {name} validation file byte for byte", flush=True)


if __name__ == "__main__":
    main()
