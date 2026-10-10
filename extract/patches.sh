# Copyright (C) 2022 The LineageOS Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The new qspi settings fail to initialize in cboot, revert to what was used in l4t r32
function patch_rey_qspi_bct() {
  echo -n "Patching rey qspi bct...";

  sed -i 's/interface-frequency = 50/interface-frequency = 133/' ${LINEAGE_ROOT}/${OUTDIR}/galen/r35/BCT/tegra19x-mb1-bct-device-qspi-p3668.cfg
  sed -i 's/maximum-bus-width = 0/maximum-bus-width = 2/' ${LINEAGE_ROOT}/${OUTDIR}/galen/r35/BCT/tegra19x-mb1-bct-device-qspi-p3668.cfg
  sed -i 's/trimmer2-val = 0/trimmer2-val = 0x10/' ${LINEAGE_ROOT}/${OUTDIR}/galen/r35/BCT/tegra19x-mb1-bct-device-qspi-p3668.cfg

  echo "";
}

function patch_rey_bpmp_dtb() {
  echo -n "Patching rey bpmp dtb...";

  git -C ${LINEAGE_ROOT}/${OUTDIR} apply -q ${LINEAGE_ROOT}/device/nvidia/galen/extract/rey_bpmp.patch

  echo "";
}

function patch_t194_chip_caps() {
  echo -n "Patching T194 chip capabilities...";

  # Shield-TV libnvhwcomposer rejects > 7 display windows; the chip library reports 8 for T194.
  # See patch_chip_caps.py for the measurement and the exact bytes.
  local NVGPU=${LINEAGE_ROOT}/${OUTDIR}/common/rel-shield-r/nvgpu
  python3 ${LINEAGE_ROOT}/device/nvidia/galen/extract/patch_chip_caps.py \
    ${NVGPU}/lib64/libnvrm_chip.so ${NVGPU}/lib/libnvrm_chip.so

  echo "";
}

patch_rey_qspi_bct;
patch_rey_bpmp_dtb;
patch_t194_chip_caps;
