# Package builders (Docker)

This directory contains Docker Compose environments to build installable packages
from the current git checkout, using the target distro's packaging as a base.

Outputs are written to each builder directory's `out/` folder.

## Ubuntu 22.04 (deb)

```bash
cd package_builder/ubuntu22.04
docker compose up --build
ls -lh out/
```

## Ubuntu 24.04 (deb)

```bash
cd package_builder/ubuntu24.04
docker compose up --build
ls -lh out/
```

## Fedora 28 (rpm)

```bash
cd package_builder/fedora28
docker compose up --build
ls -lh out/
```

## Fedora (latest) (rpm)

```bash
cd package_builder/fedora-latest
docker compose up --build
ls -lh out/
```
