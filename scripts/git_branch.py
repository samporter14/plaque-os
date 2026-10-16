"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for development environments.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.

The Plaque readers (x4pro, x4c) get a string that tells one build from another, and
one board from the other, because it goes to the mailbox as X-Lamina-Firmware, into
/Lamina/reader.json and shows in Settings > About:

    1.6.5-x4pro+1ac8494.lamina_stageb         (a clean tree)
    1.6.5-x4pro+1ac8494.dirty.lamina_stageb   (uncommitted changes)
    1.6.5-x4c+1ac8494.lamina_stageb           (the X4 Classic)

At most 40 printable ASCII characters, which is what the Worker accepts. Only one
hyphen follows the version, so the string can never contain "-rc" (upstream's OTA check, which Plaque no longer
has, treated that as a release candidate), and the branch is cut to what room is left. A version comparison
should read the leading "1.6.5" and nothing else.
"""

import configparser
import os
import re
import subprocess
import sys

# Environments whose version string names the board and the commit. Others set it in platformio.ini.
LAMINA_ENVS = ('x4pro', 'x4c')
FIRMWARE_VERSION_MAX = 40  # the mailbox Worker's X-Lamina-Firmware limit


def warn(msg):
    print(f'WARNING [git_branch.py]: {msg}', file=sys.stderr)


def run_git_value(project_dir, args, label):
    try:
        value = subprocess.check_output(
            ['git', *args],
            text=True, stderr=subprocess.PIPE, cwd=project_dir
        ).strip()
        # Strip characters that would break a C string literal
        return ''.join(c for c in value if c not in '"\\')
    except FileNotFoundError:
        warn(f'git not found on PATH; {label} suffix will be "unknown"')
        return 'unknown'
    except subprocess.CalledProcessError as e:
        warn(
            f'git command failed (exit {e.returncode}): '
            f'{e.stderr.strip()}; {label} suffix will be "unknown"'
        )
        return 'unknown'
    except OSError as e:
        warn(
            f'OS error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'
    except Exception as e:  # pylint: disable=broad-exception-caught
        warn(
            f'Unexpected error reading git {label}: {e}; '
            f'{label} suffix will be "unknown"'
        )
        return 'unknown'


def get_git_branch(project_dir):
    branch = run_git_value(
        project_dir, ['rev-parse', '--abbrev-ref', 'HEAD'], 'branch'
    )
    # Detached HEAD has no branch name.
    if branch == 'HEAD':
        return 'detached'
    return branch


def get_git_short_sha(project_dir):
    return run_git_value(
        project_dir, ['rev-parse', '--short', 'HEAD'], 'short SHA'
    )


def get_base_version(project_dir):
    ini_path = os.path.join(project_dir, 'platformio.ini')
    if not os.path.isfile(ini_path):
        warn(f'platformio.ini not found at {ini_path}; base version will be "0.0.0"')
        return '0.0.0'
    config = configparser.ConfigParser()
    config.read(ini_path, encoding='utf-8')
    if not config.has_option('crosspoint', 'version'):
        warn('No [crosspoint] version in platformio.ini; base version will be "0.0.0"')
        return '0.0.0'
    return config.get('crosspoint', 'version')


def get_git_dirty(project_dir):
    status = run_git_value(
        project_dir, ['status', '--porcelain', '--untracked-files=no'], 'status'
    )
    return bool(status) and status != 'unknown'


def lamina_version(base_version, suffix, branch, short_sha, dirty):
    """<base>-<suffix>+<sha>[.dirty][.<branch>], at most FIRMWARE_VERSION_MAX characters."""
    sha = re.sub(r'[^A-Za-z0-9]', '', short_sha) or 'unknown'
    version = f'{base_version}-{suffix}+{sha}'
    if dirty:
        version += '.dirty'
    room = FIRMWARE_VERSION_MAX - len(version) - 1  # the dot before the branch
    name = re.sub(r'[^A-Za-z0-9_]', '_', branch)[:max(room, 0)]
    if name:
        version += '.' + name
    return version[:FIRMWARE_VERSION_MAX]


def inject_version(env):
    # Only applies to development environments and the Plaque readers; release
    # envs set the version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] not in ('default', 'sticky') + LAMINA_ENVS:
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    short_sha = get_git_short_sha(project_dir)
    if env['PIOENV'] in LAMINA_ENVS:
        version_string = lamina_version(
            base_version, env['PIOENV'], branch, short_sha, get_git_dirty(project_dir)
        )
    else:
        version_string = f'{base_version}-dev-{branch}-{short_sha}'

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'Plaque build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_):
            # Validation mode does not need SCons state; this stub only satisfies inject_version().
            return None

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
