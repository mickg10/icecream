#!/usr/bin/env python3
"""Emit complementary environment splits with explicit project-fold exclusion."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--profiles", type=Path, required=True)
    parser.add_argument("--projects", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()

    profiles = [row["profile"] for row in read_tsv(args.profiles)]
    projects = read_tsv(args.projects)
    if len(profiles) != 4 or len(set(profiles)) != 4:
        raise SystemExit("the initial split generator requires four distinct profiles")
    pairs = [((0, 1), (2, 3)), ((0, 2), (1, 3)), ((0, 3), (1, 2))]
    columns = ["environment_split", "evaluation", "target_project", "target_corpus",
               "target_profile", "excluded_project_fold", "training_profiles", "test_profiles",
               "training_projects", "training_cells", "test_cell"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, columns, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for train_indices, test_indices in pairs:
            train_profiles = [profiles[index] for index in train_indices]
            test_profiles = [profiles[index] for index in test_indices]
            split_name = "+".join(train_profiles) + "->" + "+".join(test_profiles)
            for target in projects:
                target_fold = target["fold"]
                generic_projects = [row["project"] for row in projects if row["fold"] != target_fold]
                for evaluation, training_projects in (
                    ("same-project-cross-environment", [target["project"]]),
                    ("generic-project-and-environment-holdout", generic_projects),
                ):
                    training_cells = [f"{project}@{profile}" for project in training_projects
                                      for profile in train_profiles]
                    for target_profile in test_profiles:
                        if any(cell == f"{target['project']}@{target_profile}" for cell in training_cells):
                            raise SystemExit("training/test cell overlap")
                        writer.writerow({
                            "environment_split": split_name,
                            "evaluation": evaluation,
                            "target_project": target["project"],
                            "target_corpus": target["corpus"],
                            "target_profile": target_profile,
                            "excluded_project_fold": target_fold,
                            "training_profiles": ",".join(train_profiles),
                            "test_profiles": ",".join(test_profiles),
                            "training_projects": ",".join(training_projects),
                            "training_cells": ",".join(training_cells),
                            "test_cell": f"{target['project']}@{target_profile}",
                        })
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
