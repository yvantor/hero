THIS_DIR=$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")

if [[ -z "${HERO_INSTALL}" ]]; then
    echo "Error: HERO_INSTALL variable is not set (set it to toolchain installation path)"
    return
fi
export PATH=${HERO_INSTALL}/bin:$PATH

if [[ -z "${HERO_TARGET_HOST}" ]]; then
  export HERO_TARGET_PATH="/mnt/root/"
fi
export HERO_TARGET_PATH_APPS="${HERO_TARGET_PATH}/apps"
export HERO_TARGET_PATH_DRIVER="${HERO_TARGET_PATH}/drivers"
export HERO_TARGET_PATH_LIB="${HERO_TARGET_PATH}/libs"

export PLATFORM=ARIANE
export BOARD=URANIA

export ARCH="riscv"
export HERO_TOOLCHAIN_HOST_TARGET="${ARCH}64-buildroot-linux-gnu"
export CROSS_COMPILE="${HERO_TOOLCHAIN_HOST_TARGET}-"

export HERO_TOOLCHAIN_HOST_LINUX_ARCH="${ARCH}"
export KERNEL_ARCH="${ARCH}"
export KERNEL_CROSS_COMPILE=${CROSS_COMPILE}

export KERNEL_DIR="${THIS_DIR}/../output/br-hrv/build/linux-5.16.9"

export HERCULES_ARCH=ALSAQR

export PULP_RISCV_GCC_TOOLCHAIN=${HERO_INSTALL}

export HERO_PULP_SDK_DIR=$(readlink -f "$THIS_DIR/../pulp/sdk")

source ${HERO_PULP_SDK_DIR}/init.sh > /dev/null
if [ -f ${HERO_PULP_SDK_DIR}/sourceme.sh ]; then
    export HERO_PULP_INC_DIR=${HERO_PULP_SDK_DIR}/pkg/sdk/dev/install/include
    source ${HERO_PULP_SDK_DIR}/sourceme.sh
fi

unset CFLAGS
unset LDFLAGS
