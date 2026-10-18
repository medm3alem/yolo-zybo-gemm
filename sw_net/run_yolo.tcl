# xsct run_yolo.tcl -- bitstream + ELF + model.bin/img.bin en DDR, puis lancement
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set bit  [lindex [glob $ws/plat_tile/hw/*.bit] 0]
set ps7  $ws/plat_tile/hw/ps7_init.tcl
set elf  [lindex [glob $ws/yolo_run/*/yolo_run.elf] 0]
set mdl  $root/sw_net/model.bin
set img  $root/sw_net/img.bin
foreach f [list $bit $ps7 $elf $mdl $img] { if {![file exists $f]} { puts "ERREUR : introuvable : $f"; exit 1 } }
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
dow -data $mdl 0x20000000
dow -data $img 0x21000000
dow $elf
con
puts "Lance : [file tail $elf]"
exit
