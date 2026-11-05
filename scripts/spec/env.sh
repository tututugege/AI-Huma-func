# Shared settings of the SPECint2006 SimPoint pipeline. Data lives OUTSIDE /tmp (a reboot clears /tmp) and outside the repo.
#   SPECDATA   root of the generated data: bin/ (executables), img/ (Linux images), bbv/, sp/ (SimPoint), ckpt/, status/
#   LLM4TRACE  Linux kernel/OpenSBI/rootfs (linux_rv32/) and the SPEC2006 tree (spec2006/)
export SPECDATA=${SPECDATA:-$HOME/personal/AI-Huma-data/spec-rv32imab}
export LLM4TRACE=${LLM4TRACE:-$HOME/personal/llm4trace}
FM_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export FM=${FM:-$FM_ROOT/a.out}
export SIMPOINT=${SIMPOINT:-$HOME/personal/AI-Huma-data/tools/SimPoint.3.2/bin/simpoint}   # native build, see README
export MKIMAGE=$FM_ROOT/linux/mkimage.sh
export SPECBIN=$SPECDATA/bin
SPEC_ALL="400.perlbench 401.bzip2 403.gcc 429.mcf 445.gobmk 456.hmmer 458.sjeng 462.libquantum 464.h264ref 471.omnetpp 473.astar 483.xalancbmk"
