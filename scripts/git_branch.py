"""
PlatformIO pre-build script: inject git branch and short SHA into
CROSSPOINT_VERSION for development environments.

Results in a version string like:  1.1.0-dev-feat-kosync-xpath-05c6cf8
Release environments are unaffected; they set CROSSPOINT_VERSION in the ini.
"""

import configparser
import os
import subprocess
import sys


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


def get_jp_tag(project_dir):
    """The jp-* tag on HEAD, or '' when HEAD is not tagged.

    Exact match only: a tag two commits back describes those two commits, not
    this one, and calling an untagged build a release is how an unreleased
    binary ends up on someone's card.
    """
    try:
        out = subprocess.run(
            ['git', 'describe', '--tags', '--exact-match', '--match', 'jp-*'],
            cwd=project_dir, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return ''
    return out.stdout.strip() if out.returncode == 0 else ''


def is_working_tree_dirty(project_dir):
    """True when a TRACKED file differs from HEAD. None when it cannot be told.

    Why this exists: a binary built from an uncommitted tree carries the SHA of
    the previous commit, because that is what HEAD says. The version string then
    names a commit whose content is not what is running. That happened here --
    a firmware with the nothrow fix in it reported the commit before it -- and
    section 8.8-9 of the spec had already warned about exactly this. A written
    lesson did not prevent it; a suffix in the string nobody has to remember to
    check might.

    Untracked files are deliberately NOT dirt. This tree normally carries
    untracked work (japanese.yaml, build_instr/, lib/Instrument/), so counting
    them would mark every build -dirty and the label would mean nothing -- the
    stale-baseline failure in 8.8-32, in a new costume. What matters here is
    whether the code that went into the binary is the code the SHA names, and
    that is a question about tracked content.

    Returns None rather than guessing when git cannot answer, and the caller
    then omits the suffix: an instrument must not stop the thing it measures.
    """
    try:
        out = subprocess.run(
            ['git', 'status', '--porcelain', '--untracked-files=no'],
            cwd=project_dir, capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError) as e:
        warn(f'cannot determine whether the tree is dirty ({e}); '
             'omitting the -dirty suffix')
        return None
    if out.returncode != 0:
        warn('git status failed (exit '
             f'{out.returncode}): {out.stderr.strip()}; '
             'omitting the -dirty suffix')
        return None
    return bool(out.stdout.strip())


def inject_version(env):
    # Only applies to development environments; release envs set the
    # version via build_flags in platformio.ini and are unaffected.
    if env['PIOENV'] not in ('default', 'sticky'):
        return

    project_dir = env['PROJECT_DIR']
    base_version = get_base_version(project_dir)
    branch = get_git_branch(project_dir)
    short_sha = get_git_short_sha(project_dir)

    # This fork's own version, on top of the upstream one it is built from:
    #
    #   1.5.0-jp-0.2          a tagged release (git tag jp-0.2)
    #   1.5.0-jp-dev-<sha>    everything else
    #
    # The upstream number stays in front on purpose. It is what tells a reader
    # which upstream this is a Japanese build of, and the Escape Hatch note in
    # the release checklist depends on it -- a 1.4 device refuses a 1.5 image,
    # and the person holding the card needs to see which they have.
    #
    # The branch name is gone from the untagged form. It was there to tell two
    # development branches apart on one desk, and it leaks a working detail into
    # a string shown on the device's boot screen.
    jp_tag = get_jp_tag(project_dir)
    if jp_tag:
        version_string = f'{base_version}-{jp_tag}'
    else:
        version_string = f'{base_version}-jp-dev-{short_sha}'

    # A tracked modification means the SHA above does not describe what is in
    # this binary, so say so in the string itself.
    dirty = is_working_tree_dirty(project_dir)
    if dirty:
        version_string += '-dirty'
        if jp_tag:
            # The bad case. A tagged build is the one that gets handed to
            # someone, and this one contains code that is in no commit, so the
            # tag names something unreproducible.
            #
            # It is labelled rather than refused, on purpose. Failing the build
            # would put a version-string script in the way of ordinary work --
            # and the label is the stronger guard anyway, because it travels:
            # it is on the boot screen, in the crash report and in the OTA
            # descriptor, where refusing to build leaves no trace at all. The
            # release checklist's job is to require that -dirty is absent.
            warn(f'TAGGED BUILD FROM A DIRTY TREE: {version_string}. '
                 'The tag names content that is not committed. '
                 'Do not distribute this image; commit, then rebuild.')

    env.Append(CPPDEFINES=[('CROSSPOINT_VERSION', f'\\"{version_string}\\"')])
    print(f'CrossPoint build version: {version_string}')


# PlatformIO/SCons entry point — Import and env are SCons builtins injected at runtime.
# When run directly with Python (e.g. for validation), a lightweight fake env is used
# so the git/version logic can be exercised without a full build.
try:
    Import('env')           # noqa: F821  # type: ignore[name-defined]
    inject_version(env)     # noqa: F821  # type: ignore[name-defined]
except NameError:
    class _Env(dict):
        def Append(self, **_): pass

    _project_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    inject_version(_Env({'PIOENV': 'default', 'PROJECT_DIR': _project_dir}))
