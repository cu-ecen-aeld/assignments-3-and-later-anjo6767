#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"

if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi

if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    echo "Cleaning command"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper

    echo "Configuring default kernel"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig

    echo "Building kernel Image"
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

    echo "Building kernel modules"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules

    echo "Building device tree"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs

    cd "${OUTDIR}"
fi

echo "Adding the Image in outdir"
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"   
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p "${OUTDIR}/rootfs"  
cd "${OUTDIR}/rootfs"		

mkdir -p bin dev etc home lib lib64 proc sbin sys tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
#rm -rf busybox   
if [ ! -d "${OUTDIR}/busybox" ]
then
    echo "going in if"
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean 
    make defconfig  
  
else
    echo "going in else cd busybox "
    cd busybox
fi

# TODO: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install


#anuja check below before submission

# TODO: Add library dependencies to rootfs
echo "Library dependencies"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a ${OUTDIR}/rootfs/bin/busybox | grep "Shared library"



SYSROOT="$(${CROSS_COMPILE}gcc -print-sysroot)"
#LIBGCC_DIR="$(dirname "$(${CROSS_COMPILE}gcc -print-libgcc-file-name)")"

cp ${SYSROOT}/lib/ld-linux-aarch64.so.* ${OUTDIR}/rootfs/lib/

for f in \
  ${SYSROOT}/lib64/libc.so.6 \
  ${SYSROOT}/lib64/libm.so.6 \
  ${SYSROOT}/lib64/libresolv.so.2

  
  #"${LIBGCC_DIR}/libgcc_s.so.1"
do
  if [ -e "$f" ]; then
    echo "Copying $(basename "$f")"
    cp "$f" "${OUTDIR}/rootfs/lib64/" 
  else
    echo "NOTE: Missing $f (may not be required on your setup)"
  fi
done


# TODO: Make device nodes
# === Minimal device nodes ===
cd "${OUTDIR}/rootfs"
sudo mknod -m 666 ${OUTDIR}/rootfs/dev/null c 1 3  
sudo mknod -m 600 ${OUTDIR}/rootfs/dev/console c 5 1  


# TODO: Clean and build the writer utility
echo "Building writer with CROSS_COMPILE=${CROSS_COMPILE}"
make -C "${FINDER_APP_DIR}" clean
#make -C "${FINDER_APP_DIR}" CROSS_COMPILE="${CROSS_COMPILE}gcc"
make -C "${FINDER_APP_DIR}" CROSS_COMPILE=${CROSS_COMPILE} writer


# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cp "${FINDER_APP_DIR}/writer"  "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/" 
mkdir -p "${OUTDIR}/rootfs/home/conf"
cp "${FINDER_APP_DIR}/conf/username.txt" "${OUTDIR}/rootfs/home/conf/"
cp "${FINDER_APP_DIR}/conf/assignment.txt" "${OUTDIR}/rootfs/home/conf/" 
cp "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/" 

# TODO: Chown the root directory
sudo chown -R root:root "${OUTDIR}/rootfs"

# TODO: Create initramfs.cpio.gz
echo "Creating initramfs.cpio.gz"
cd "${OUTDIR}/rootfs"
# -H newc : format understood by the kernel; --owner sets uid/gid in archive
find . | cpio -H newc -ov --owner root:root > "${OUTDIR}/initramfs.cpio"
gzip -f "${OUTDIR}/initramfs.cpio"

echo "Done. Kernel Image: ${OUTDIR}/Image"
