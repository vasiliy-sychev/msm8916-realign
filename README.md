# msm8916-realign
Small experimental tool for aligning partitions on eMMC drive

Designed for, and tested on Snapdragon 410-based smartphone (Wingtech wt88047 a.k.a. Xiaomi Redmi 2), but maybe this tool will work with other modern Qualcomm-based smartphones

Important note: this tool uses CRC32 code from "efone - distributed internet phone system", which are GPLv2-licensed

# How to do realignment
Before you begin, requirements:
1. You need to know eMMC size in sectors (can be obtained with mmc_utils / fdisk / dmesg) and erase group size (can be obtained using mmc_utils, or from datasheet). For my 8GB eMMC from Numonyx Micron this values is 15302656 sectors and 8MB erase group size
2. Original FULL firmware package for your smartphone (FULL package, with all system files (gpt_main0.bin, gpt_backup0.bin, sbl, tz, rpm, modem, ...) and firehose bootloader), not a update.zip for recovery mode
3. Backup of important system partitions (modemst1, modemst2, fsc, fsg, misc, config, DDR, ...)
4. QPST and drivers installed

Step 1. Unpack firmware, and do realignment using msm8916-realign, and do not close command line window!

first file:
> msm8916-realign 8M 15302656 main gpt_main0.bin

and second:
> msm8916-realign 8M 15302656 backup gpt_backup0.bin

Step 2. Replace start_byte_hex values inside a rawprogram0.xml file, with values generated by msm8916-realign (see example in target_wt88047_8gb), and do other necessary edits (file names, sparse=true/false)

Step 3. Reboot your phone into a "Qualcomm HS-USB QDLoader (9008) mode"

There are three ways:
1. First (simplest): using hardware keys (doesn't work on my device)
2. Second: "kill" secondary bootloader (SBL) using fastboot (fastboot erase sbl1; fastboot erase sbl1bak)
3. Third: "kill" SBL using dd (dd if=/dev/zero of=/dev/block/mmcblk0pX bs=1024 count=XXX), requires root-access or advanced recovery (like a TWRP)

Step 4. Flash modified firmware package using QFIL

Step 5. Turn on your phone!
