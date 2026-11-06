#!/usr/bin/env bash
# Read-only fact collection on Arch. Output goes to ./phase0-out/
set -u
out=phase0-out; mkdir -p "$out"
{ echo "== audio PCI"; lspci -nn | grep -i -E "audio|multimedia"; } > "$out/pci.txt"
sudo lspci -vvv -nn -s 00:1f.3 > "$out/lspci-1f3.txt" 2>&1
sudo lspci -xxxx -s 00:1f.3 > "$out/lspci-1f3-config.txt" 2>&1
{ echo "== SOF / DMIC / NHLT in dmesg"; sudo dmesg | grep -i -E "sof|dmic|nhlt|snd_hda|dspcfg|firmware"; } > "$out/dmesg-sof.txt"
{ echo "== cards"; cat /proc/asound/cards; echo; echo "== capture devices"; arecord -l; echo; echo "== modules"; lsmod | grep -E "snd_sof|snd_hda|snd_soc"; } > "$out/alsa.txt" 2>&1
{ echo "== firmware files"; ls -lR /usr/lib/firmware/intel/sof* 2>/dev/null | head -80; echo; pacman -Qi sof-firmware 2>/dev/null | head -8; } > "$out/firmware.txt"
sudo cat /sys/firmware/acpi/tables/NHLT > "$out/NHLT.bin" 2>/dev/null
{ echo "== debugfs sof"; sudo ls -R /sys/kernel/debug/sof 2>/dev/null | head -60; } > "$out/sof-debugfs.txt"
{ echo "== kernel"; uname -a; echo; zcat /proc/config.gz 2>/dev/null | grep -E "SND_SOC_SOF|SND_INTEL_DSP_CONFIG|SND_HDA_INTEL" ; } > "$out/kernel.txt"
echo "Done. Send or commit the contents of $out/"
