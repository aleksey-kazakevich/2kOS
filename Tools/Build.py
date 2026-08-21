#!/usr/bin/env python3
"""
TOS Build System
Рекурсивно собирает все компоненты проекта и создаёт ISO образ.
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
# Пути (сохраняются в скрипте)
# -----------------------------------------------------------------------------
SCRIPT_PATH = Path(__file__).resolve()

# Базовые пути (будут переопределены при настройке)
PROJECT_ROOT = "/home/alex/Рабочий стол/2kOS"
TOOLS_DIR = "/home/alex/Рабочий стол/2kOS/Tools/Make"
ISO_PATH = "/home/alex/Рабочий стол/2kOS/2kOS.iso"
# Пути внутри проекта
KERNEL_DIR = f"{PROJECT_ROOT}/Kernel"
BIN_DIR = f"{KERNEL_DIR}/Bin"

# -----------------------------------------------------------------------------
# Конфигурация сборки
# -----------------------------------------------------------------------------
# Компоненты для сборки (порядок важен!)
COMPONENTS = [
    ("Kernel", KERNEL_DIR),
]

# Исключаемые директории (не искать Makefile)
EXCLUDE_DIRS = [
    ".git",
    ".svn",
    "__pycache__",
    "Build",
    "Bin",
    "Tools",
    "include",
]

# -----------------------------------------------------------------------------

def ask_path(prompt, default):
    """Запрашивает путь у пользователя."""
    while True:
        resp = input(f"{prompt} [{default}]: ").strip()
        if not resp:
            resp = default
        p = Path(resp).resolve()
        if p.exists():
            return str(p)
        log_error(f"Path does not exist: {p}")

def save_config_in_self(new_root, new_tools):
    """Сохраняет конфигурацию в самом скрипте."""
    try:
        content = SCRIPT_PATH.read_text(encoding="utf-8")
    except Exception as e:
        log_error(f"Failed to read own script: {e}")
        sys.exit(1)

    def replace_line(content, var_name, new_value):
        pattern = rf'^{var_name}\s*=\s*(["\'])(.*?)\1\s*$'
        def repl(m):
            quote = m.group(1)
            return f'{var_name} = {quote}{new_value}{quote}'
        new_content, count = re.subn(pattern, repl, content, flags=re.MULTILINE)
        if count == 0:
            log_warning(f"Could not find '{var_name}' line to update.")
        return new_content

    # Обновляем основные пути
    content = replace_line(content, "PROJECT_ROOT", new_root.replace("\\", "/"))
    content = replace_line(content, "TOOLS_DIR", new_tools.replace("\\", "/"))
    
    # Пересчитываем производные пути
    root = Path(new_root)
    content = replace_line(content, "KERNEL_DIR", str((root / "Kernel").resolve()).replace("\\", "/"))
    content = replace_line(content, "USERSPACE_DIR", str((root / "Userspace").resolve()).replace("\\", "/"))
    content = replace_line(content, "BIN_DIR", str((root / "Bin").resolve()).replace("\\", "/"))
    content = replace_line(content, "ISO_PATH", str((root / "2kOS.iso").resolve()).replace("\\", "/"))

    try:
        SCRIPT_PATH.write_text(content, encoding="utf-8")
        log_status("Configuration saved successfully.")
    except Exception as e:
        log_error(f"Failed to write updated script: {e}")
        sys.exit(1)

def check_paths():
    """Проверяет существование всех путей."""
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
    if not ok:
        return False

    # Проверяем grub-mkrescue
    try:
        subprocess.run(["grub-mkrescue", "--version"], 
                      stdout=subprocess.DEVNULL, 
                      stderr=subprocess.DEVNULL)
    except FileNotFoundError:
        log_error("grub-mkrescue not found. Install grub2-tools.")
        return False

    return True

def find_makefiles(root_dir):
    """Рекурсивно находит все Makefile в директории."""
    makefiles = []
    root = Path(root_dir).resolve()
    
    for dirpath, dirnames, filenames in os.walk(root):
        # Пропускаем исключаемые директории
        dirnames[:] = [d for d in dirnames if d not in EXCLUDE_DIRS]
        
        # Проверяем наличие Makefile
        if "Makefile" in filenames:
            makefiles.append(Path(dirpath) / "Makefile")
            
        # Также проверяем makefile (с маленькой буквы)
        if "makefile" in filenames:
            makefiles.append(Path(dirpath) / "makefile")
    
    return sorted(makefiles)

def find_all_makefiles():
    """Находит все Makefile во всех компонентах."""
    all_makefiles = []
    
    for name, path in COMPONENTS:
        if not Path(path).exists():
            log_warning(f"Component '{name}' not found: {path}")
            continue
        
        log_info(f"Searching for Makefiles in {name}...")
        makefiles = find_makefiles(path)
        if makefiles:
            log_info(f"  Found {len(makefiles)} Makefile(s)")
            all_makefiles.extend(makefiles)
        else:
            log_warning(f"  No Makefiles found in {name}")
    
    return all_makefiles

def build_makefile(makefile_path, verbose=False):
    """Выполняет make в директории с Makefile."""
    build_dir = makefile_path.parent
    rel_path = build_dir.relative_to(PROJECT_ROOT) if PROJECT_ROOT in str(build_dir) else build_dir
    
    log_build(f"Building: {rel_path}")
    
    # Переходим в директорию
    os.chdir(build_dir)
    
    # Выполняем make
    cmd = ["make", f"-j{os.cpu_count()}"]
    if verbose:
        cmd.append("V=1")
    
    try:
        start_time = time.time()
        result = subprocess.run(cmd, capture_output=True, text=True)
        duration = int(time.time() - start_time)
        
        if result.returncode == 0:
            log_status(f"  ✓ Build successful in {duration}s")
            if verbose and result.stdout:
                print(result.stdout)
            return True
        else:
            log_error(f"  ✗ Build failed in {duration}s")
            if result.stderr:
                print(result.stderr)
            return False
    except Exception as e:
        log_error(f"  ✗ Build exception: {e}")
        return False

def create_iso():
    """Создаёт ISO образ при помощи Limine и xorriso."""
    log_status("Creating ISO image...")
    
    iso_path = Path(ISO_PATH)
    tools_dir = Path(TOOLS_DIR)
    
    if not tools_dir.exists():
        log_error(f"Tools directory not found: {tools_dir}")
        return False
    
    # Удаляем старый ISO
    if iso_path.exists():
        log_info("Removing old ISO...")
        iso_path.unlink()
    
    # Создаем временную структуру папок для сборщика ISO
    iso_root = tools_dir / "iso_root"
    if iso_root.exists():
        shutil.rmtree(iso_root)
    iso_root.mkdir(parents=True, exist_ok=True)
    
    # Копируем базовые файлы Limine из Tools/Make во временный корень
    required_limine_files = ["limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin", "limine.conf"]
    for f in required_limine_files:
        src = tools_dir / f
        if not src.exists():
            log_error(f"Required Limine file not found in Tools/Make: {f}")
            return False
        shutil.copy(src, iso_root / f)
        
    # Настраиваем EFI загрузчики
    efi_boot_dir = iso_root / "EFI" / "BOOT"
    efi_boot_dir.mkdir(parents=True, exist_ok=True)
    for efi_file in ["BOOTX64.EFI", "BOOTIA32.EFI"]:
        src_efi = tools_dir / efi_file
        if src_efi.exists():
            shutil.copy(src_efi, efi_boot_dir / efi_file)

    # Копируем ваше скомпилированное ядро
    # ПРИМЕЧАНИЕ: Если ваше ядро называется не 'mkernel', замените имя в строке ниже
    kernel_src = Path(PROJECT_ROOT) / "Kernel" / "Bin" / "kernel"
    if not kernel_src.exists():
        kernel_src = Path(PROJECT_ROOT) / "Kernel" / "mkernel"
        
    if not kernel_src.exists():
        log_error(f"Kernel binary not found at {kernel_src}. Ensure it builds successfully.")
        return False
    shutil.copy(kernel_src, iso_root / "kernel")
    
    # Собираем ISO через xorriso
    try:
        start_time = time.time()
        result = subprocess.run([
            "xorriso", "-as", "mkisofs", "-R", "-r", "-J",
            "-b", "limine-bios-cd.bin",
            "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
            "--efi-boot", "limine-uefi-cd.bin",
            "-efi-boot-part", "--efi-boot-image", "--protective-msdos-label",
            str(iso_root),
            "-o", str(iso_path)
        ], capture_output=True, text=True)
        
        duration = int(time.time() - start_time)
        
        if result.returncode == 0 and iso_path.exists():
            size_mb = iso_path.stat().st_size / 1024 / 1024
            log_status(f"ISO created successfully in {duration}s")
            log_info(f"  Path: {iso_path}")
            log_info(f"  Size: {size_mb:.1f} MB")
            
            # Финальный флаг гибридности (для USB флешек)
            try:
                limine_exe = tools_dir / "limine"
                if limine_exe.exists() and os.access(limine_exe, os.X_OK):
                    subprocess.run([str(limine_exe), "bios-install", str(iso_path)], stdout=subprocess.DEVNULL)
                else:
                    subprocess.run(["limine", "bios-install", str(iso_path)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except:
                pass
            
            # Проверяем ISO
            try:
                file_out = subprocess.check_output(["file", str(iso_path)], text=True)
                if "ISO" in file_out:
                    log_status("  ISO file is valid.")
            except:
                pass
                
            # Очищаем временную директорию
            shutil.rmtree(iso_root)
            return True
        else:
            log_error("Failed to create ISO!")
            if result.stderr:
                print(result.stderr)
            if iso_root.exists():
                shutil.rmtree(iso_root)
            return False
            
    except Exception as e:
        log_error(f"Failed to create ISO: {e}")
        if iso_root.exists():
            shutil.rmtree(iso_root)
        return False


def build_all(verbose=False):
    """Собирает все компоненты по порядку."""
    log_status("Starting build process...")
    start_time = time.time()
    
    # Находим все Makefile
    makefiles = find_all_makefiles()
    
    if not makefiles:
        log_error("No Makefiles found!")
        return False
    
    log_info(f"Total Makefiles found: {len(makefiles)}")
    print()
    
    # Сортируем по глубине (сначала корневые, потом вложенные)
    makefiles.sort(key=lambda x: len(x.parent.parents))
    
    # Собираем каждый Makefile
    success_count = 0
    failed = []
    
    for makefile in makefiles:
        if build_makefile(makefile, verbose):
            success_count += 1
        else:
            failed.append(makefile)
    
    duration = int(time.time() - start_time)
    
    # Выводим статистику
    print()
    log_status(f"Build finished in {duration}s")
    log_info(f"  Successful: {success_count}/{len(makefiles)}")
    
    if failed:
        log_warning(f"  Failed: {len(failed)}")
        for f in failed:
            rel_path = f.parent.relative_to(PROJECT_ROOT) if PROJECT_ROOT in str(f.parent) else f.parent
            log_warning(f"    - {rel_path}")
        return False
    
    return True

def run_build():
    """Основная функция сборки."""
    log_status("=== 2kOS Build System ===")
    log_info(f"Project root: {PROJECT_ROOT}")
    log_info(f"Tools dir:    {TOOLS_DIR}")
    print()
    
    # Собираем все компоненты
    if not build_all(verbose=False):
        log_error("Build failed!")
        sys.exit(1)
    
    print()
    
    # Пути
    kernel_bin = Path(BIN_DIR)                    # Kernel/Bin
    root_bin = Path(PROJECT_ROOT) / "Bin"         # /Bin
    tools_dir = Path(TOOLS_DIR)                    # Tools/Make
    
    # Создаём директории
    root_bin.mkdir(parents=True, exist_ok=True)
    tools_dir.mkdir(parents=True, exist_ok=True)
    
    log_status("Copying binaries...")
    
    # Копируем ядро: Kernel/Bin/kernel -> Bin/kernel
    kernel_src = kernel_bin / "kernel"
    kernel_root = root_bin / "kernel"
    
    if kernel_src.exists():
        shutil.copy2(kernel_src, kernel_root)
        log_info(f"  Kernel: {kernel_src} -> {kernel_root}")
    else:
        log_warning("  Kernel ELF not found in Kernel/Bin!")
        return
    
    # Копируем ядро: Bin/kernel -> Tools/Make/kernel
    kernel_tools = tools_dir / "kernel"
    if kernel_root.exists():
        shutil.copy2(kernel_root, kernel_tools)
        log_info(f"  Kernel: {kernel_root} -> {kernel_tools}")
    else:
        log_warning("  Kernel ELF not found in root Bin!")
        return
    
    print()
    
    # Создаём ISO
    if not create_iso():
        log_error("ISO creation failed!")
        sys.exit(1)
    
    log_status("=== Build complete! ===")
    log_info(f"ISO available at: {ISO_PATH}")

def main():
    """Главная функция."""
    # Проверяем конфигурацию
    if check_paths():
        run_build()
        return
    
    log_warning("Configuration paths are invalid or directories missing.")
    log_info("Please enter correct paths. The script will update itself and exit.")
    print()
    
    new_root = ask_path("Enter path to project root", PROJECT_ROOT)
    new_tools = ask_path("Enter path to Tools/Make directory", TOOLS_DIR)
    
    save_config_in_self(new_root, new_tools)
    
    print()
    log_status("Configuration updated in the script.")
    log_warning("Please re-run the script:")
    print(f"  {sys.executable} {SCRIPT_PATH}")
    sys.exit(0)

if __name__ == "__main__":
    main()
