# xsct run_yolo_sd_2c.tcl [cpu1.elf] -- comme run_yolo_sd.tcl, mais charge AUSSI le programme du coeur 1 (gemm_cpu1) avant le coeur 0.
# La carte SD doit etre inseree (model.bin, images\, config.txt avec "neon2" dans modes=).
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set bit  [lindex [glob $ws/plat_tile/hw/*.bit] 0]
set ps7  $ws/plat_tile/hw/ps7_init.tcl
set elf  [lindex [glob $ws/yolo_sd/*/yolo_sd.elf] 0]
set cpu1 C:/Users/USER/gemm_dualcore/gemm_cpu1/Debug/gemm_cpu1.elf
if {$argc >= 1} { set cpu1 [lindex $argv 0] }
foreach f [list $bit $ps7 $elf $cpu1] { if {![file exists $f]} { puts "ERREUR : introuvable : $f"; exit 1 } }
connect
targets -set -nocase -filter {name =~ "APU*"}
rst -system
after 3000
targets -set -nocase -filter {name =~ "xc7z*"}
fpga -file $bit
targets -set -nocase -filter {name =~ "ARM*#0"}
source $ps7
ps7_init
ps7_post_config
# zone partagee des deux coeurs remise a zero (sinon un vieux "go=1" d'un run interrompu serait execute)
mwr -force 0x1F000000 0x00000000 32
# coeur 1 d'abord : il attend un travail
targets -set -nocase -filter {name =~ "ARM*#1"}
dow $cpu1
con
after 500
# puis le coeur 0
targets -set -nocase -filter {name =~ "ARM*#0"}
dow $elf
con
puts "Lance : [file tail $elf] (coeur 0) + [file tail $cpu1] (coeur 1)"
exit
