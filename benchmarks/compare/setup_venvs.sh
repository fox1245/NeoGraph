#!/bin/bash
# Create the virtual environments of the cross-framework cohort.
#
#   setup_venvs.sh WORKDIR [venv-name ...]
#
# WORKDIR/venvs/<name> is created for every `# venv: <name>` section of benchmarks/requirements-compare.txt
# (the latest releases) and benchmarks/requirements-compare-head.txt (the tip of each default branch, from git).
# WORKDIR/venvs/_empty is a venv with nothing installed, the baseline of the installed-footprint figures.
# With names, only those environments are created. An existing environment is not touched.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$1
shift
wanted=("$@")
mkdir -p "$work/venvs"

make_venv() {
    local name=$1
    local venv=$work/venvs/$name
    if [ -d "$venv" ]; then
        echo "keep $name"
        return 0
    fi
    python3 -m venv "$venv" || return 1
    "$venv/bin/python" -m pip install --quiet --disable-pip-version-check --upgrade pip || return 1
}

want() {
    [ ${#wanted[@]} -eq 0 ] && return 0
    local n
    for n in "${wanted[@]}"; do [ "$n" = "$1" ] && return 0; done
    return 1
}

if want _empty; then make_venv _empty; fi

for file in "$here/../requirements-compare.txt" "$here/../requirements-compare-head.txt"; do
    name=""
    section=""
    flush() {
        if [ -n "$name" ] && want "$name"; then
            if [ -d "$work/venvs/$name" ]; then
                echo "keep $name"
            else
                make_venv "$name" || { echo "FAILED to create $name"; return 1; }
                printf '%s' "$section" > "$work/venvs/$name.requirements.txt"
                if "$work/venvs/$name/bin/python" -m pip install --quiet --disable-pip-version-check \
                        -r "$work/venvs/$name.requirements.txt"; then
                    echo "installed $name"
                else
                    echo "FAILED to install $name"
                fi
            fi
        fi
    }
    while IFS= read -r line; do
        case "$line" in
            "# venv: "*) flush; name=${line#"# venv: "}; section="" ;;
            "#"*|"") ;;
            *) section+="$line"$'\n' ;;
        esac
    done < "$file"
    flush
done
