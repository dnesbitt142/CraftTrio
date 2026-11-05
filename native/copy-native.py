#!/usr/bin/env python3
"""Atomically stage a linked native image without changing its bytes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def copy_native(source, destination):
    source = source.expanduser().resolve(strict=True)
    destination = destination.expanduser().absolute()
    if source == destination.resolve():
        raise ValueError("Use a separate output file")
    source_hash = digest(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=".native-copy-", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(name)
    try:
        shutil.copy2(source, temporary)
        copied_hash = digest(temporary)
        if copied_hash != source_hash or digest(source) != source_hash:
            raise RuntimeError("Native source changed during copy")
        size = temporary.stat().st_size
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return {"method": "byte-preserving-copy", "source": str(source),
            "destination": str(destination), "bytes": size,
            "sourceSha256": source_hash, "sha256": copied_hash}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    try:
        report = copy_native(args.source, args.destination)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(1, "Native copy stopped: " + str(error) + "\n")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
