#!/usr/bin/env python3
"""
manage_tests.py
Interactivo/CLI para conservar/eliminar archivos de test en SchedulingShips/data/tests
y opcionalmente ejecutar la compilación de Arduino y el simulador de display.

Uso interactivo: python manage_tests.py
Opciones: --keep minimal|all|pattern:<glob>|filelist:<f1,f2> --run-compile --run-sim

"""
import argparse
import fnmatch
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
TEST_DIR = os.path.join(ROOT, 'data', 'tests')

PROTECTED = {'README.txt', 'index.txt', '_template_comment.txt', '.gitkeep', 'all_tests.txt'}

MINIMAL = [
    'pair_l_nn.txt','pair_l_np.txt','pair_l_nu.txt','pair_l_pn.txt','pair_l_pp.txt','pair_l_pu.txt','pair_l_un.txt','pair_l_up.txt','pair_l_uu.txt',
    'pair_r_nn.txt','pair_r_np.txt','pair_r_nu.txt','pair_r_pn.txt','pair_r_pp.txt','pair_r_pu.txt','pair_r_un.txt','pair_r_up.txt','pair_r_uu.txt',
]

DEFAULT_ALGORITHMS = ['fcfs', 'sjf', 'strn', 'edf', 'rr', 'priority']
DEFAULT_FLOWS = ['tico', 'fair', 'sign']
SHIP_TYPES = ['n', 'p', 'u']
READYMAX_ALL_PAIRS = 40

ARDUINO_CLI = r"D:\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
ARDUINO_INO = r"d:\tec\2026\i sem\sistemasoperativos\Proyecto1Operativos\SchedulingShips\SchedulingShips.ino"
PYTHON_EXE = r"D:/Program Files/Python311/python.exe"
MKLITTLEFS_EXE = r"D:\Tools\mklittlefs.exe"
DEFAULT_FS_IMAGE = os.path.join(ROOT, 'littlefs.bin')
DEFAULT_FS_OFFSET = '0x290000'
DEFAULT_FS_SIZE = '0x160000'
DEFAULT_FS_PAGE = '256'
DEFAULT_FS_BLOCK = '4096'
DEFAULT_ESP_CHIP = 'esp32c6'
DEFAULT_ESP_PORT = 'COM6'
SIMULATOR = os.path.join(ROOT, '..', '..', 'display_simulator.py')
# Above default path tries to match user's requested path; if not found, use project root path
if not os.path.isfile(SIMULATOR):
    SIMULATOR = os.path.join(os.path.dirname(ROOT), 'display_simulator.py')


def list_tests():
    # Asegurar que el directorio de tests existe
    os.makedirs(TEST_DIR, exist_ok=True)
    files = sorted([f for f in os.listdir(TEST_DIR) if os.path.isfile(os.path.join(TEST_DIR, f))])
    return files


def filter_keep(files, keep_mode, pattern=None, filelist=None):
    if keep_mode == 'all':
        return set(files)
    if keep_mode == 'minimal':
        return set([
            f for f in files
            if f in MINIMAL or f in PROTECTED or f.endswith('_all_pairs.txt')
        ])
    if keep_mode == 'pattern' and pattern:
        keep = set()
        for pat in pattern.split(','):
            pat = pat.strip()
            keep.update([f for f in files if fnmatch.fnmatch(f, pat)])
        keep.update(PROTECTED)
        return keep
    if keep_mode == 'filelist' and filelist:
        items = [x.strip() for x in filelist.split(',') if x.strip()]
        keep = set([f for f in files if f in items])
        keep.update(PROTECTED)
        return keep
    # default: nothing (except protected)
    return set(PROTECTED)


def confirm(prompt):
    try:
        r = input(prompt + ' [y/N]: ').strip().lower()
    except EOFError:
        return False
    return r == 'y' or r == 'yes'


def delete_files(to_delete):
    for fn in to_delete:
        path = os.path.join(TEST_DIR, fn)
        try:
            os.remove(path)
            print('Deleted', fn)
        except Exception as e:
            print('Failed to delete', fn, ':', e)


def run_command(cmd, shell=False):
    print('Running:', ' '.join(cmd) if isinstance(cmd, (list, tuple)) else cmd)
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, shell=shell)
        for line in proc.stdout:
            print(line, end='')
        proc.wait()
        print('Exit code:', proc.returncode)
        return proc.returncode
    except FileNotFoundError as e:
        print('Command not found:', e)
        return -1


def build_littlefs_image(mklittlefs_exe, image_path, fs_page, fs_block, fs_size):
    """Construye littlefs.bin usando SchedulingShips/data como fuente."""
    data_dir = os.path.join(ROOT, 'data')
    os.makedirs(os.path.dirname(image_path), exist_ok=True)
    cmd = [
        mklittlefs_exe,
        '-c', data_dir,
        '-p', str(fs_page),
        '-b', str(fs_block),
        '-s', str(fs_size),
        image_path,
    ]
    return run_command(cmd)


def flash_littlefs_image(
    python_exe,
    chip,
    port,
    fs_offset,
    image_path,
):
    """Flashea imagen LittleFS vía esptool."""
    cmd = [
        python_exe,
        '-m',
        'esptool',
        '--chip',
        chip,
        '--port',
        port,
        'write-flash',
        fs_offset,
        image_path,
    ]
    return run_command(cmd)


def main():
    parser = argparse.ArgumentParser(description='Gestionar tests y ejecutar compilación/simulador')
    parser.add_argument('--keep', help='keep mode: minimal|all|pattern:<glob1,glob2>|filelist:<f1,f2>', default='minimal')
    parser.add_argument('--run-compile', action='store_true', help='Ejecutar arduino-cli compile después de la limpieza')
    parser.add_argument('--run-sim', action='store_true', help='Ejecutar el simulador de display después de la limpieza')
    parser.add_argument('--run-flashfs', action='store_true', help='Construir y subir LittleFS (mklittlefs + esptool)')
    parser.add_argument('--flashfs-on-load', action='store_true', help='En modo interactivo, subir LittleFS automáticamente en cada test cargado')
    parser.add_argument('--no-flashfs-on-load', action='store_true', help='En modo interactivo, no subir LittleFS por test (anula pregunta inicial)')
    parser.add_argument('--fqbn', help='FQBN para compilación (por defecto esp32:esp32:esp32c6)', default='esp32:esp32:esp32c6')
    parser.add_argument('--esp-chip', default=DEFAULT_ESP_CHIP, help='Chip para esptool (por defecto esp32c6)')
    parser.add_argument('--esp-port', default=DEFAULT_ESP_PORT, help='Puerto serial para esptool (por defecto COM6)')
    parser.add_argument('--fs-offset', default=DEFAULT_FS_OFFSET, help='Offset de flash para LittleFS (por defecto 0x290000)')
    parser.add_argument('--fs-size', default=DEFAULT_FS_SIZE, help='Tamaño de partición LittleFS (por defecto 0x160000)')
    parser.add_argument('--fs-page', default=DEFAULT_FS_PAGE, help='Tamaño de página para mklittlefs (por defecto 256)')
    parser.add_argument('--fs-block', default=DEFAULT_FS_BLOCK, help='Tamaño de bloque para mklittlefs (por defecto 4096)')
    parser.add_argument('--fs-image', default=DEFAULT_FS_IMAGE, help='Ruta de salida de imagen LittleFS (por defecto SchedulingShips/littlefs.bin)')
    parser.add_argument('--mklittlefs', default=MKLITTLEFS_EXE, help='Ruta a mklittlefs.exe')
    parser.add_argument('--install-core', action='store_true', help='Instalar el core esp32 antes de compilar (usa arduino-cli core install)')
    parser.add_argument('--no-interactive', action='store_true', help='No entrar en modo interactivo de carga de tests')
    parser.add_argument('--generate-tests', action='store_true', help='Generar 1 archivo por combinación algoritmo+flujo; cada archivo incluye los 18 casos (9 por lado)')
    parser.add_argument('--algorithms', help='Lista separada por comas de algoritmos a generar (por defecto todos)', default=','.join(DEFAULT_ALGORITHMS))
    parser.add_argument('--flows', help='Lista separada por comas de flujos a generar (por defecto tico,fair,sign)', default=','.join(DEFAULT_FLOWS))
    parser.add_argument('--generate-pairs', action='store_true', help='Generar solo los 18 tests pares (3x3 por lado) y entrar en runner con ellos')
    parser.add_argument('--prune-others', action='store_true', help='Si se genera pares, eliminar otros tests y dejar solo los 18 (con confirmación)')
    args = parser.parse_args()

    generated = set()
    # Si se solicitó generar tests, créalos primero (antes de calcular keep/delete)
    if args.generate_tests:
        algs = [a.strip() for a in args.algorithms.split(',') if a.strip()]
        flows = [f.strip() for f in args.flows.split(',') if f.strip()]
        generated.update(generate_tests(algs, flows))
    if args.generate_pairs:
        generated.update(generate_pair_tests())

    files = list_tests()
    print('Tests en', TEST_DIR)
    print('Total archivos:', len(files))

    # prepare keep set
    keep_mode = 'minimal'
    pattern = None
    filelist = None
    if args.keep.startswith('pattern:'):
        keep_mode = 'pattern'
        pattern = args.keep[len('pattern:'):]
    elif args.keep.startswith('filelist:'):
        keep_mode = 'filelist'
        filelist = args.keep[len('filelist:'):]
    elif args.keep == 'all':
        keep_mode = 'all'
    elif args.keep == 'minimal':
        keep_mode = 'minimal'
    else:
        print('Keep mode desconocido, usando minimal')

    if generated and args.keep == 'minimal':
        keep = set([f for f in files if f in generated or f in PROTECTED])
    else:
        keep = filter_keep(files, keep_mode, pattern, filelist)
    to_delete = [f for f in files if f not in keep]

    print('\nMantenerán ({}):'.format(len(keep)))
    for f in sorted(keep):
        print('  ', f)
    print('\nEliminarán ({}):'.format(len(to_delete)))
    for f in sorted(to_delete):
        print('  ', f)

    if not to_delete:
        print('Nada que eliminar.')
    else:
        if confirm('¿Continuar y eliminar los archivos listados?'):
            delete_files(to_delete)
        else:
            print('Operación cancelada por usuario.')
            return

    interactive = not args.no_interactive

    # Si modo interactivo: entrar en loop de carga/ejecución de tests
    if args.prune_others and generated:
        existing = list_tests()
        to_remove = [f for f in existing if f not in generated and f not in PROTECTED]
        if to_remove:
            print('Archivos que se propondrán eliminar (no forman parte de los generados):')
            for f in to_remove:
                print('  ', f)
            if confirm('¿Eliminar estos archivos y dejar solo los generados?'):
                delete_files(to_remove)

    if interactive:
        flash_per_test = args.run_flashfs or args.flashfs_on_load
        ask_flash_on_start = not flash_per_test and not args.no_flashfs_on_load
        if generated:
            interactive_runner(
                sorted(list(generated)),
                run_flashfs=flash_per_test,
                ask_flashfs_on_start=ask_flash_on_start,
                mklittlefs_exe=args.mklittlefs,
                fs_image=args.fs_image,
                fs_page=args.fs_page,
                fs_block=args.fs_block,
                fs_size=args.fs_size,
                python_exe=PYTHON_EXE,
                esp_chip=args.esp_chip,
                esp_port=args.esp_port,
                fs_offset=args.fs_offset,
            )
        else:
            interactive_runner(
                sorted(list(keep)),
                run_flashfs=flash_per_test,
                ask_flashfs_on_start=ask_flash_on_start,
                mklittlefs_exe=args.mklittlefs,
                fs_image=args.fs_image,
                fs_page=args.fs_page,
                fs_block=args.fs_block,
                fs_size=args.fs_size,
                python_exe=PYTHON_EXE,
                esp_chip=args.esp_chip,
                esp_port=args.esp_port,
                fs_offset=args.fs_offset,
            )

    # Opcional: compilar
    if args.run_compile or (interactive and confirm('¿Deseas ejecutar la compilación de Arduino ahora?')):
        fqbn = args.fqbn
        if args.install_core:
            print('Instalando/actualizando índice de cores y el core esp32...')
            run_command([ARDUINO_CLI, 'core', 'update-index'])
            run_command([ARDUINO_CLI, 'core', 'install', 'esp32:esp32'])
        print('Compilando con FQBN:', fqbn)
        cmd = [ARDUINO_CLI, 'compile', '--fqbn', fqbn, ARDUINO_INO]
        run_command(cmd)

    # Opcional: simulador
    if args.run_sim or (interactive and confirm('¿Deseas ejecutar el simulador de display ahora?')):
        # try to find display_simulator.py path
        sim_path = os.path.join(os.path.dirname(ROOT), 'display_simulator.py')
        if os.path.isfile(os.path.join(ROOT, 'display_simulator.py')):
            sim_path = os.path.join(ROOT, 'display_simulator.py')
        elif os.path.isfile(SIMULATOR):
            sim_path = SIMULATOR
        print('Usando simulador:', sim_path)
        cmd = [PYTHON_EXE, sim_path]
        run_command(cmd)

    if (not interactive and args.run_flashfs) or (
        interactive and not args.run_flashfs and confirm('¿Deseas subir LittleFS al ESP32 ahora?')
    ):
        if not os.path.isfile(args.mklittlefs):
            print('No se encontró mklittlefs en:', args.mklittlefs)
        else:
            rc_build = build_littlefs_image(args.mklittlefs, args.fs_image, args.fs_page, args.fs_block, args.fs_size)
            if rc_build == 0:
                flash_littlefs_image(PYTHON_EXE, args.esp_chip, args.esp_port, args.fs_offset, args.fs_image)
            else:
                print('No se flasheó porque falló la construcción de LittleFS.')

    print('Hecho.')


def load_test_file(src_filename):
    src = os.path.join(TEST_DIR, src_filename)
    dest = os.path.join(os.path.dirname(TEST_DIR), 'channel_config.txt')
    bak = dest + '.bak'
    try:
        if os.path.isfile(dest):
            shutil.copy2(dest, bak)
        shutil.copy2(src, dest)
        print('Cargado', src_filename, '->', dest)
        return True
    except Exception as e:
        print('Error cargando test:', e)
        return False


def interactive_runner(
    test_list,
    run_flashfs=False,
    ask_flashfs_on_start=False,
    mklittlefs_exe=MKLITTLEFS_EXE,
    fs_image=DEFAULT_FS_IMAGE,
    fs_page=DEFAULT_FS_PAGE,
    fs_block=DEFAULT_FS_BLOCK,
    fs_size=DEFAULT_FS_SIZE,
    python_exe=PYTHON_EXE,
    esp_chip=DEFAULT_ESP_CHIP,
    esp_port=DEFAULT_ESP_PORT,
    fs_offset=DEFAULT_FS_OFFSET,
):
    """Recorre `test_list` (nombres de archivo) y permite cargarlos y ejecutarlos uno a uno."""
    if not test_list:
        print('Lista de tests vacía.')
        return

    if ask_flashfs_on_start:
        run_flashfs = confirm(
            f'¿Subir LittleFS al ESP ({esp_chip} en {esp_port}) en cada test cargado?'
        )

    sim_path = os.path.join(os.path.dirname(ROOT), 'display_simulator.py')
    if os.path.isfile(os.path.join(ROOT, 'display_simulator.py')):
        sim_path = os.path.join(ROOT, 'display_simulator.py')
    elif os.path.isfile(SIMULATOR):
        sim_path = SIMULATOR

    i = 0
    n = len(test_list)
    while i < n:
        name = test_list[i]
        print('\n[{}/{}] {}'.format(i+1, n, name))
        path = os.path.join(TEST_DIR, name)
        try:
            with open(path, 'r', encoding='utf-8') as fh:
                preview = ''.join(fh.readlines()[:12])
        except Exception:
            preview = '<no se pudo leer archivo>'
        print('--- preview ---')
        print(preview)
        print('---------------')

        action = input('[Enter/Y] cargar, [s] skip, [c] choose, [list] listar, [p] prev, [q] quit: ').strip().lower()
        if action in ('', 'y', 'yes', 'si', 'load', 'l'):
            # Load and optionally run simulator
            ok = load_test_file(name)
            if not ok:
                print('Error al cargar; saltando.')
                i += 1
                continue
            run_sim = input('Ejecutar simulador ahora? [Y/n]: ').strip().lower()
            if run_sim == '' or run_sim == 'y' or run_sim == 'yes':
                if not os.path.isfile(sim_path):
                    print('No se encontró el simulador en:', sim_path)
                else:
                    run_command([PYTHON_EXE, sim_path])
            if run_flashfs:
                if not os.path.isfile(mklittlefs_exe):
                    print('No se encontró mklittlefs en:', mklittlefs_exe)
                else:
                    print(f'Subiendo LittleFS a {esp_chip} en {esp_port}...')
                    rc_build = build_littlefs_image(mklittlefs_exe, fs_image, fs_page, fs_block, fs_size)
                    if rc_build == 0:
                        flash_littlefs_image(python_exe, esp_chip, esp_port, fs_offset, fs_image)
                    else:
                        print('No se flasheó porque falló la construcción de LittleFS.')
            # Preguntar seguir
            nxt = input('Siguiente test? [Enter=si, q=salir, p=quitar adelante]: ').strip().lower()
            if nxt == 'q':
                break
            elif nxt == 'p':
                # stay on same index (re-run)
                continue
            else:
                i += 1
                continue
        elif action.startswith('s'):
            i += 1
            continue
        elif action.startswith('c'):
            choice = input('Nombre (o número) del test a cargar: ').strip()
            if choice.isdigit():
                idx = int(choice)-1
                if 0 <= idx < n:
                    i = idx
                else:
                    print('Índice fuera de rango')
            else:
                if choice in test_list:
                    i = test_list.index(choice)
                else:
                    print('No existe ese archivo')
            continue
        elif action in ('list', 'ls'):
            for idx, nm in enumerate(test_list, start=1):
                print('{:3d}. {}'.format(idx, nm))
            continue
        elif action.startswith('p'):
            i = max(0, i-1)
            continue
        elif action.startswith('q'):
            break
        else:
            print('Opción no reconocida')
            continue



def generate_tests(algorithms=None, flows=None, sides=('l','r')):
    """Genera 1 archivo por combinación algoritmo+flujo.
    Cada archivo contiene los 18 casos (9 por lado) de barco detrás de barco.
    Retorna un set con los nombres de archivos creados."""
    if algorithms is None:
        algorithms = DEFAULT_ALGORITHMS
    if flows is None:
        flows = DEFAULT_FLOWS
    created = set()
    required_readymax = READYMAX_ALL_PAIRS

    os.makedirs(TEST_DIR, exist_ok=True)
    for alg in algorithms:
        for flow in flows:
            fname = f"{alg}_{flow}_all_pairs.txt"
            path = os.path.join(TEST_DIR, fname)
            content = _build_header_for(alg, flow, readymax_value=required_readymax)
            for side in sides:
                for f in SHIP_TYPES:
                    for b in SHIP_TYPES:
                        content += f"demoadd {side} {f} 1\n"
                        content += f"demoadd {side} {b} 1\n"
            # evitar sobrescribir sin confirmación
            if os.path.exists(path):
                try:
                    with open(path, 'r', encoding='utf-8') as fh:
                        existing = fh.read()
                    if existing == content:
                        created.add(fname)
                        continue
                except Exception:
                    pass
                if not confirm(f'El archivo {fname} ya existe. Sobrescribir?'):
                    continue
            try:
                with open(path, 'w', encoding='utf-8') as fh:
                    fh.write(content)
                created.add(fname)
            except Exception as e:
                print('Error creando', fname, ':', e)
    print(f'Generados/actualizados {len(created)} archivos en: {TEST_DIR}')
    return created


def _build_header_for(alg, flow, readymax_value=None):
    """Construye un header usando channel_config.txt como base y ajustando flow/alg/rr."""
    ccpath = os.path.join(os.path.dirname(TEST_DIR), 'channel_config.txt')
    lines = []
    if os.path.isfile(ccpath):
        try:
            with open(ccpath, 'r', encoding='utf-8') as fh:
                for ln in fh:
                    lines.append(ln)
                    if ln.strip().lower().startswith('democlear'):
                        break
        except Exception:
            lines = []

    if not lines:
        return (
            'listlen 200\nvisual 100\nflow {flow}\nflowlog on\nw 2\nsignms 8000\nsign r\n'
            'proxpin 22 23\nproxpollms 120\nsensor desactivate\nsensor threshold 10\nreadymax 12\n'
            'alg {alg}\n{rr_line}step n 2\nstep p 2\nstep u 50\n\ndemoclear\n'
        ).format(flow=flow, alg=alg, rr_line=('rr 9000\n' if alg == 'rr' else ''))

    out = []
    had_flow = False
    had_alg = False
    had_rr = False
    had_readymax = False
    had_democlear = False
    for ln in lines:
        stripped = ln.strip()
        low = stripped.lower()
        if low.startswith('flow '):
            out.append(f'flow {flow}\n')
            had_flow = True
            continue
        if low.startswith('alg '):
            out.append(f'alg {alg}\n')
            had_alg = True
            continue
        if low.startswith('rr '):
            had_rr = True
            if alg == 'rr':
                out.append(ln)
            continue
        if low.startswith('readymax '):
            had_readymax = True
            if readymax_value is None:
                out.append(ln)
            else:
                out.append(f'readymax {readymax_value}\n')
            continue
        if low.startswith('democlear'):
            had_democlear = True
            out.append('democlear\n')
            break
        out.append(ln)

    if not had_flow:
        out.append(f'flow {flow}\n')
    if not had_alg:
        out.append(f'alg {alg}\n')
    if alg == 'rr' and not had_rr:
        out.append('rr 9000\n')
    if readymax_value is not None and not had_readymax:
        out.append(f'readymax {readymax_value}\n')
    if not had_democlear:
        if out and out[-1].strip() != '':
            out.append('\n')
        out.append('democlear\n')
    return ''.join(out)


def generate_pair_tests(sides=('l','r')):
    """Genera solo los 18 archivos pares: para cada lado (l,r) y cada par ordenado de tipos (3x3).
    Usa el encabezado actual de `channel_config.txt` si existe, de lo contrario usa un header por defecto."""
    types = ['n','p','u']
    created = set()
    # intentar leer header actual de channel_config.txt
    ccpath = os.path.join(os.path.dirname(TEST_DIR), 'channel_config.txt')
    header = None
    if os.path.isfile(ccpath):
        try:
            with open(ccpath, 'r', encoding='utf-8') as fh:
                lines = fh.readlines()
            # tomar hasta la línea que contenga 'democlear' inclusive
            hdr_lines = []
            for ln in lines:
                hdr_lines.append(ln)
                if ln.strip().lower().startswith('democlear'):
                    break
            header = ''.join(hdr_lines)
        except Exception:
            header = None

    if header is None:
        header = ('listlen 200\nvisual 100\nflow fair\nflowlog on\nw 2\nsignms 8000\nsign r\n'
                  'proxpin 22 23\nproxpollms 120\nsensor desactivate\nsensor threshold 10\nreadymax 12\n'
                  'alg fcfs\nstep n 2\nstep p 2\nstep u 50\n\ndemoclear\n')

    os.makedirs(TEST_DIR, exist_ok=True)
    for side in sides:
        for f in types:
            for b in types:
                fname = f"pair_{side}_{f}{b}.txt"
                path = os.path.join(TEST_DIR, fname)
                content = header
                content += f"demoadd {side} {f} 1\n"
                content += f"demoadd {side} {b} 1\n"
                try:
                    with open(path, 'w', encoding='utf-8') as fh:
                        fh.write(content)
                    created.add(fname)
                except Exception as e:
                    print('Error creando', fname, ':', e)
    print(f'Generados {len(created)} archivos de pares en: {TEST_DIR}')
    return created


if __name__ == '__main__':
    main()
