# xsct run_yolo_sd.tcl -- bitstream + ELF (la carte SD doit etre inseree avec model.bin et images/)
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set bit  [lindex [glob $ws/plat_tile/hw/*.bit] 0]
set ps7  $ws/plat_tile/hw/ps7_init.tcl
set elf  [lindex [glob $ws/yolo_sd/*/yolo_sd.elf] 0]
foreach f [list $bit $ps7 $elf] { if {![file exists $f]} { puts "ERREUR : introuvable : $f"; exit 1 } }
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
dow $elf
con
puts "Lance : [file tail $elf]"
exit
