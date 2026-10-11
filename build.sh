#! /usr/bin/env bash

set -Eeo pipefail

# ensure an iso was passed
if [ -z "${1}" ]; then
    echo "Usage:"
    echo "    $0 <path/to/vanilla/melee.iso> [release]"
    exit 1
fi
iso="${1}"

if [ "${2}" = "release" ]; then
    release=true
    # a release ships the reference script, built fresh from the vanilla ISO
    if [ -n "${LLSCRIPT}" ]; then
        echo "Error: unset LLSCRIPT for a release (it would ship a test script)"
        exit 1
    fi
    if [ -f TM-CE.iso ]; then
        echo "Error: move TM-CE.iso out of the way first, so the release is built from a fresh copy"
        exit 1
    fi
elif [ -n "${2}" ]; then
    mode="${2}"
fi

if [ ! -f "${iso}" ]; then
    echo "Error: path '${iso}' does not exist!"
    exit 1
fi

if [[ "$(uname)" =~ "MSYS" ]]; then
    gc_fst="bin/gc_fst.exe"
    hgecko="bin/hgecko.exe"
    hmex="bin/hmex.exe"
    xdelta="bin/xdelta.exe"
elif [[ "$(uname)" == "Darwin" && "$(uname -m)" == "arm64" ]]; then
    gc_fst="bin/gc_fst_macos_arm64"
    hgecko="bin/hgecko_macos_arm64"
    hmex="bin/hmex_macos_arm64"
    xdelta="xdelta3"
else
    gc_fst="bin/gc_fst"
    hgecko="bin/hgecko"
    hmex="bin/hmex"
    xdelta="xdelta3"
fi

# check if en or jp and use appropriate patch
header=$(${gc_fst} get-header "${iso}")
if [ "${header}" = "GALE01" ]; then
    patch="patch.xdelta"
elif [ "${header}" = "GALJ01" ]; then
    patch="patch_jp.xdelta"
else
    echo "Error: Invalid ISO '${iso}'"
    exit 1
fi

# fn to kill all processes everywhere
kill_all() {
    trap "kill -- -$$" EXIT
}

# fn to build mex executable
mex_build() {
    local sym="${1}"
    local out="${2}"
    local src="${3}"

    if [[ -n "${mode}" && "${mode}" != "${out}" ]]; then
        return
    fi

    if [ -n "${4}" ]; then
        local dat="-dat ${4}"
    else
        local dat=""
    fi

    if [ "${release}" = true ]; then
        local opt="-O2"
    else
        local opt="-DTM_DEBUG"
    fi

    # a Landing Lab test build (LLSCRIPT set) skips the stage select:
    # Battlefield, as the test scripts are written for it
    if [ -n "${LLSCRIPT}" ]; then
        opt="${opt} -DLL_TEST_STAGE"
        # LLSTAGE picks another stage (a GRKINDEXT_ name), LLCHAR another
        # fighter (a CKIND_ name), for the data dumps (tools/llsim)
        if [ -n "${LLSTAGE}" ]; then opt="${opt} -DLL_TEST_STAGE_ID=${LLSTAGE}"; fi
        if [ -n "${LLCHAR}" ]; then opt="${opt} -DLL_TEST_CHAR=${LLCHAR}"; fi
    fi

    warn="-Wall -Wextra -Wno-char-subscripts -Wno-builtin-declaration-mismatch -Wno-unused-parameter"
    ${hmex} -q -l "MexTK/melee.link" -f "${warn} ${opt}" -s "${sym}" -t "MexTK/${sym}.txt" -o "${out}" -i ${src} ${dat} || kill_all
    echo built ${out}
}

# make build directory if necessary
mkdir -p build

# compile code in parallel
mex_build "tmFunction" "build/eventMenu.dat" "src/events.c src/menu.c src/osds.c src/savestate_v1.c" "dats/eventMenu.dat" &
mex_build "cssFunction" "build/labCSS.dat" "src/lab_css.c" "dats/labCSS.dat" &
mex_build "evFunction" "build/lab.dat" "src/lab.c" "dats/lab.dat" &
mex_build "evFunction" "build/lcancel.dat" "src/lcancel.c" &
mex_build "evFunction" "build/ledgedash.dat" "src/ledgedash.c" &
mex_build "evFunction" "build/wavedash.dat" "src/wavedash.c" "dats/wavedash.dat" &
mex_build "evFunction" "build/powershield.dat" "src/powershield.c" &
mex_build "evFunction" "build/dthrowknee.dat" "src/dthrowknee.c" &
mex_build "evFunction" "build/edgeguard.dat" "src/edgeguard.c" &
mex_build "evFunction" "build/fc.dat" "src/fc.c" &
mex_build "evFunction" "build/sweetspot.dat" "src/sweetspot.c" &
mex_build "evFunction" "build/laserland.dat" "src/laserland.c" &
mex_build "evFunction" "build/landinglab.dat" "src/landinglab.c" &
mex_build "evFunction" "build/eggs.dat" "src/eggs.c" &
mex_build "evFunction" "build/techchase.dat" "src/techchase.c" &
mex_build "evFunction" "build/slalom.dat" "src/slalom.c" "dats/wavedash.dat" &

# wait for compilation to finish
wait

# compile asm
if [[ -z "${mode}" || "${mode}" = "build/codes.gct" ]]; then
    ${hgecko} -q ASM build/codes.gct
    echo built build/codes.gct
fi

# build mex Start.dol
${gc_fst} read "${iso}" Start.dol build/ISOStart.dol
${xdelta} -dfs build/ISOStart.dol "Build TM Start.dol/${patch}" build/Start.dol

# copy iso over
if [ ! -f TM-CE.iso ]; then cp "${iso}" TM-CE.iso; fi

# add TM files to iso
${gc_fst} fs TM-CE.iso \
    delete MvHowto.mth \
    delete MvOmake15.mth \
    delete MvOpen.mth \
    insert TM/eventMenu.dat build/eventMenu.dat \
    insert TM/lab.dat build/lab.dat \
    insert TM/labCSS.dat build/labCSS.dat \
    insert TM/lcancel.dat build/lcancel.dat \
    insert TM/ledgedash.dat build/ledgedash.dat \
    insert TM/wavedash.dat build/wavedash.dat \
    insert TM/powershield.dat build/powershield.dat \
    insert TM/dthrowknee.dat build/dthrowknee.dat \
    insert TM/edgeguard.dat build/edgeguard.dat \
    insert TM/fc.dat build/fc.dat \
    insert TM/sweetspot.dat build/sweetspot.dat \
    insert TM/laserland.dat build/laserland.dat \
    insert TM/landinglab.dat build/landinglab.dat \
    insert TM/llscript.txt "${LLSCRIPT:-src/landinglab_reference.txt}" \
    insert TM/eggs.dat build/eggs.dat \
    insert TM/techchase.dat build/techchase.dat \
    insert TM/slalom.dat build/slalom.dat \
    insert codes.gct build/codes.gct \
    insert Start.dol build/Start.dol \
    insert opening.bnr opening.bnr
${gc_fst} set-header TM-CE.iso "GTME01" "TM-CE + Landing Lab"

echo "built TM-CE.iso"

# build release: the patch, checked by applying it, in
# TM-CE-LandingLab-<version>.zip with one folder of the same name
if [ "${2}" = "release" ]; then
    # no two files on the disc may overlap
    if ! ${gc_fst} tree TM-CE.iso -s -o -x | python3 -c '
import sys
rows = []
for l in sys.stdin:
    p = l.rstrip("\n").split("\t")
    if len(p) == 3 and p[1].startswith("0x") and int(p[2], 16) > 0:
        rows.append((int(p[1], 16), int(p[2], 16), p[0]))
rows.sort()
bad = [(a[2], b[2]) for a, b in zip(rows, rows[1:]) if a[0] + a[1] > b[0]]
print("overlaps:", bad if bad else "none")
sys.exit(1 if bad else 0)'; then
        echo "Error: files overlap on TM-CE.iso"
        exit 1
    fi
    ${xdelta} -fs "${iso}" -e "TM-CE.iso" "TM-CE/patch.xdelta"
    if [ "$(${xdelta} -dc -s "${iso}" TM-CE/patch.xdelta | sha256sum | cut -d' ' -f1)" != \
         "$(sha256sum TM-CE.iso | cut -d' ' -f1)" ]; then
        echo "Error: the patch doesn't rebuild TM-CE.iso"
        exit 1
    fi
    version=$(sed -n 's/.*TM_VERSSHORT "TM-CE LL \([0-9.]*\)".*/\1/p' src/events.h)
    name="TM-CE-LandingLab-${version}"
    pkg=$(mktemp -d)
    mkdir "${pkg}/${name}"
    cp TM-CE/README.txt TM-CE/patch.xdelta TM-CE/xdelta3.exe TM-CE/build_linux_and_mac.sh \
        "TM-CE/DRAG VANILLA MELEE HERE.bat" "${pkg}/${name}/"
    rm -f "${name}.zip"
    (cd "${pkg}" && zip -qr - "${name}") > "${name}.zip"
    rm -rf "${pkg}"
    echo "built ${name}.zip"
fi
