#!/usr/bin/env python3
"""
TOS Build System
Создаёт два отдельных ISO:
  - 2kOS-B.iso — для BIOS (MBR) + limine bios-install
  - 2kOS-U.iso — для UEFI (GPT) через EFI/BOOT/BOOTX64.EFI
"""

import os
import sys
from pathlib import Path
import subprocess
import time
import shutil
import re

# -----------------------------------------------------------------------------
# Цвета
# -----------------------------------------------------------------------------
C_GREEN = "\033[0;32m"
C_YELLOW = "\033[1;33m"
C_RED = "\033[0;31m"
C_BLUE = "\033[0;34m"
C_CYAN = "\033[0;36m"
C_NC = "\033[0m"

def log_status(msg): print(f"{C_GREEN}[+]{C_NC} {msg}")
def log_warning(msg): print(f"{C_YELLOW}[!]{C_NC} {msg}")
def log_error(msg): print(f"{C_RED}[ERROR]{C_NC} {msg}")
def log_info(msg): print(f"{C_BLUE}[i]{C_NC} {msg}")
def log_build(msg): print(f"{C_CYAN}[BUILD]{C_NC} {msg}")

# -----------------------------------------------------------------------------
# Пути (будут переопределены при настройке)
# -----------------------------------------------------------------------------
SCRIPT_PATH = Path(__file__).resolve()

# Базовые пути (будут изменены при первом запуске)
PROJECT_ROOT = "/mnt/c/Users/alex/Desktop/2kOS"
TOOLS_DIR = "/mnt/c/Users/alex/Desktop/2kOS/Tools/Make"
# Директория для сборки внутри WSL (НЕ на NTFS!)
BUILD_TMP = Path("/tmp/2kOS_build")
ISO_OUT_WIN = Path(PROJECT_ROOT)  # Куда копировать готовые ISO на Windows

KERNEL_DIR = f"{PROJECT_ROOT}/Kernel"
BIN_DIR = f"{KERNEL_DIR}/Bin"

ISO_BIOS_NAME = "2kOS-B.iso"
ISO_UEFI_NAME = "2kOS-U.iso"

# -----------------------------------------------------------------------------
# Конфигурация сборки
# -----------------------------------------------------------------------------
COMPONENTS = [("Kernel", KERNEL_DIR)]
EXCLUDE_DIRS = [".git", ".svn", "__pycache__", "Build", "Bin", "Tools", "include"]

def check_paths():
    paths = [
        (PROJECT_ROOT, "Project root"),
        (KERNEL_DIR, "Kernel directory"),
        (TOOLS_DIR, "Tools/Make directory"),
    ]
    ok = True
    for path, name in paths:
        p = Path(path)
        if not p.exists():
            log_error(f"{name} does not exist: {p}")
            ok = False
    return ok

def find_makefiles(root_dir):
    makefiles = []
    root = Path(root_dir).resolve()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        if "Makefile" in filenames:
            makefiles.append(Path(dirpath) / "Makefile")
        if "makefile" in filenames:
            makefiles.append(Path(dirpath) / "makefile")
    return sorted(makefiles)

def find_all_makefiles():
    all_makefiles = []
    for name, path in COMPONENTS:
        if not Path(path).exists():
            log_warning(f"Component '{name}' not found: {path}")
            continue
        makefiles = find_makefiles(path)
        if makefiles:
            all_makefiles.extend(makefiles)
    return all_makefiles

def build_makefile(makefile_path, verbose=False):
    build_dir = makefile_path.parent
    rel_path = build_dir.relative_to(PROJECT_ROOT) if PROJECT_ROOT in str(build_dir) else build_dir
    log_build(f"Building: {rel_path}")
    os.chdir(build_dir)
    cmd = ["make", f"-j{os.cpu_count()}"]
    if verbose:
        cmd.append("V=1")
    try:
        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        duration = int(time.time() - start_time)
        if result.returncode == 0:
            log_status(f"  ✓ Build successful in {duration}s")
            return True
        else:
            log_error(f"  ✗ Build failed in {duration}s")
            if result.stderr:
                print(result.stderr)
            return False
    except Exception as e:
        log_error(f"  ✗ Build exception: {e}")
        return False

def prepare_iso_root_bios(iso_root):
    """Подготавливает корневую директорию для BIOS ISO."""
    if iso_root.exists():
        shutil.rmtree(iso_root)
    iso_root.mkdir(parents=True, exist_ok=True)

    tools_dir = Path(TOOLS_DIR)
    
    # Проверяем наличие всех файлов
    required_files = [
        "limine-bios.sys",
        "limine-bios-cd.bin",
        "limine.conf"
    ]

    for f in required_files:
        src = tools_dir / f
        if not src.exists():
            log_error(f"Required file not found: {f}")
            log_error(f"Expected at: {src}")
            return False
        # Копируем с сохранением прав (важно!)
        shutil.copy2(src, iso_root / f)
        log_info(f"Copied {f} ({src.stat().st_size} bytes)")

    # Копируем ядро
    kernel_src = Path(PROJECT_ROOT) / "Kernel" / "Bin" / "kernel"
    if not kernel_src.exists():
        kernel_src = Path(PROJECT_ROOT) / "Kernel" / "mkernel"
    if not kernel_src.exists():
        log_error(f"Kernel binary not found at: {kernel_src}")
        return False
    shutil.copy2(kernel_src, iso_root / "kernel")
    
    return True

def prepare_iso_root_uefi(iso_root):
    """Подготавливает корневую директорию для UEFI ISO с использованием limine-uefi-cd.bin."""
    if iso_root.exists():
        shutil.rmtree(iso_root)
    iso_root.mkdir(parents=True, exist_ok=True)

    tools_dir = Path(TOOLS_DIR)
    
    # 1. Кладем конфиг и ядро в корень ISO
    shutil.copy(tools_dir / "limine.conf", iso_root / "limine.conf")
    
    kernel_src = Path(PROJECT_ROOT) / "Kernel" / "Bin" / "kernel"
    if not kernel_src.exists():
        kernel_src = Path(PROJECT_ROOT) / "Kernel" / "mkernel"
    shutil.copy(kernel_src, iso_root / "kernel")

    # 2. Создаем структуру EFI/BOOT в корне ISO (для чтения файлов ОС после загрузки)
    efi_boot_dir = iso_root / "EFI" / "BOOT"
    efi_boot_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(tools_dir / "EFI" / "BOOT" / "BOOTX64.EFI", efi_boot_dir / "BOOTX64.EFI")

    # 3. КРИТИЧНО ДЛЯ XORRISO: копируем бинарный образ EFI дискеты от Limine
    # Этот файл обязательно должен поставляться с Limine (находится в дистрибутиве)
    uefi_cd_bin = tools_dir / "limine-uefi-cd.bin"
    if not uefi_cd_bin.exists():
        log_error("limine-uefi-cd.bin not found! Please place it in your tools directory.")
        return False
        
    boot_dir = iso_root / "boot"
    boot_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy(uefi_cd_bin, boot_dir / "limine-uefi-cd.bin")

    return True

def create_iso_bios(iso_root, iso_path):
    """Создает BIOS ISO только через xorriso."""
    log_status("Creating BIOS ISO with xorriso only...")
    try:
        tools_dir = Path(TOOLS_DIR)
        
        # Правильная команда xorriso для создания гибридного ISO
        cmd = [
            "xorriso", "-as", "mkisofs",
            "-b", "limine-bios-cd.bin",        # Загрузочный образ
            "-no-emul-boot",                   # Без эмуляции
            "-boot-load-size", "4",            # 4 сектора
            "-boot-info-table",                # Таблица загрузки
            "-R", "-r", "-J",                  # Rock Ridge + Joliet
            "-V", "2KOS_BIOS",                 # Метка тома
            str(iso_root),                     # Корень ISO
            "-o", str(iso_path)                # Выходной файл
        ]
        
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode != 0:
            log_error(f"xorriso failed: {result.stderr}")
            return False
            
        subprocess.run([
            Path(TOOLS_DIR) / "../limine", "bios-install", "--force", str(iso_path)
        ], check=True)
            
        log_status(f"BIOS ISO created: {iso_path}")
        
        # Проверяем размер
        size = iso_path.stat().st_size / 1024 / 1024
        log_info(f"ISO size: {size:.2f} MB")
        
        return True
        
    except Exception as e:
        log_error(f"Failed: {e}")
        return False

def create_iso_uefi(iso_root, iso_path):
    """Создаёт чисто UEFI ISO (без BIOS/MBR)."""
    log_status("Creating UEFI ISO...")
    try:
        # Для чисто UEFI используем правильный синтаксис mkisofs
        result = subprocess.run([
            "xorriso", "-as", "mkisofs",
            "-R", "-r", "-J",                    # Расширения файловой системы
            "--efi-boot", "boot/limine-uefi-cd.bin",  # EFI образ
            "-efi-boot-part",                    # Создать ESP раздел
            "--efi-boot-image",                  # Образ как FAT
            "-no-emul-boot",                     # Без эмуляции
            "-V", "2KOS_UEFI",                   # Метка тома
            str(iso_root),                       # Корень ISO
            "-o", str(iso_path)                  # Выходной файл
        ], capture_output=True, text=True)

        if result.returncode != 0:
            log_error(f"xorriso failed: {result.stderr}")
            return False

        log_status(f"UEFI ISO created: {iso_path}")
        return True
    except Exception as e:
        log_error(f"Failed to create UEFI ISO: {e}")
        return False

def copy_iso_to_windows(src_iso, dst_dir):
    """Копирует готовый ISO из WSL во временную папку, затем на Windows."""
    dst_iso = dst_dir / src_iso.name
    log_info(f"Copying ISO to Windows: {dst_iso}")
    shutil.copy2(src_iso, dst_iso)
    log_status(f"ISO ready: {dst_iso}")

def build_all(verbose=False):
    log_status("Starting build process...")
    start_time = time.time()

    makefiles = find_all_makefiles()
    if not makefiles:
        log_error("No Makefiles found!")
        return False

    log_info(f"Total Makefiles found: {len(makefiles)}")
    print()

    makefiles.sort(key=lambda x: len(x.parent.parents))

    success_count = 0
    failed = []

    for makefile in makefiles:
        if build_makefile(makefile, verbose):
            success_count += 1
        else:
            failed.append(makefile)

    duration = int(time.time() - start_time)
    print()
    log_status(f"Build finished in {duration}s")
    log_info(f"Successful: {success_count}/{len(makefiles)}")

    if failed:
        log_warning(f"  Failed: {len(failed)}")
        for f in failed:
            rel_path = f.parent.relative_to(PROJECT_ROOT) if PROJECT_ROOT in str(f.parent) else f.parent
            log_warning(f"    - {rel_path}")
        return False

    return True


def run_build():
    """Основная точка входа: сборка ядра + два ISO (BIOS и UEFI) + копирование на Windows."""
    log_status("=== 2kOS Build System (Split ISO) ===")
    log_info(f"Project root: {PROJECT_ROOT}")
    log_info(f"Tools dir:    {TOOLS_DIR}")
    print()

    # 1. Проверка путей
    if not check_paths():
        log_error("Path check failed.")
        sys.exit(1)

    # 2. Сборка ядра (и всех компонентов)
    if not build_all(verbose=False):
        log_error("Build failed — ISO will not be created.")
        sys.exit(1)

    print()
    log_status("Build successful. Creating ISO images...")

    # Временная папка для сборки ISO внутри WSL (не на NTFS)
    BUILD_TMP.mkdir(parents=True, exist_ok=True)

    # --- BIOS ISO (для Rufus + BIOS/CSM) ---
    iso_root_bios = BUILD_TMP / "iso_bios_root"
    iso_path_bios = BUILD_TMP / ISO_BIOS_NAME

    if not prepare_iso_root_bios(iso_root_bios):
        log_error("Failed to prepare BIOS ISO root.")
        sys.exit(1)

    if not create_iso_bios(iso_root_bios, iso_path_bios):
        log_error("Failed to create BIOS ISO.")
        sys.exit(1)

    copy_iso_to_windows(iso_path_bios, ISO_OUT_WIN)

    # --- UEFI ISO (для VirtualBox, QEMU, UEFI флешек) ---
    iso_root_uefi = BUILD_TMP / "iso_uefi_root"
    iso_path_uefi = BUILD_TMP / ISO_UEFI_NAME

    if not prepare_iso_root_uefi(iso_root_uefi):
        log_error("Failed to prepare UEFI ISO root.")
        sys.exit(1)

    if not create_iso_uefi(iso_root_uefi, iso_path_uefi):
        log_error("Failed to create UEFI ISO.")
        sys.exit(1)

    copy_iso_to_windows(iso_path_uefi, ISO_OUT_WIN)

    # Очистка временных папок (можно закомментировать, если хочешь смотреть содержимое)
    shutil.rmtree(BUILD_TMP)

    print()
    log_status("All done!")
    log_info(f"BIOS ISO:  {ISO_OUT_WIN / ISO_BIOS_NAME}")
    log_info(f"UEFI ISO:  {ISO_OUT_WIN / ISO_UEFI_NAME}")


if __name__ == "__main__":
    run_build()
