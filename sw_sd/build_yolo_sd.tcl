# xsct build_yolo_sd.tcl  (Vitis 2023.1) -- plateforme + bibliotheque FatFs (xilffs) + application yolo_sd
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set xsa  $root/vivado/design_1_wrapper.xsa
set src  $root/sw_sd
file mkdir $ws
setws $ws
platform create -name plat_tile -hw $xsa -proc ps7_cortexa9_0 -os standalone -out $ws
domain active standalone_domain
bsp setlib -name xilffs
bsp config use_lfn 1
platform generate
app create -name yolo_sd -platform plat_tile -domain standalone_domain -template {Empty Application(C)}
file delete -force $ws/yolo_sd/src/helloworld.c
foreach f {main_zybo_sd.c app_sd.c app_sd.h gemm_arm.h dualcore.h fsio.h fsio_fatfs.c yolo_core.c yolo_core.h gemm_pack.h font8x12.h stb_image.h} { file copy -force $src/$f $ws/yolo_sd/src/$f }
file copy -force $root/sw_net/net_def.h $ws/yolo_sd/src/net_def.h
file rename -force $ws/yolo_sd/src/main_zybo_sd.c $ws/yolo_sd/src/main.c
# pile et tas agrandis (decodage JPEG/PNG, FatFs, printf)
set ld $ws/yolo_sd/src/lscript.ld
set fp [open $ld r]; set txt [read $fp]; close $fp
set n1 [regsub {(_STACK_SIZE = DEFINED\(_STACK_SIZE\) \? _STACK_SIZE : )0x[0-9a-fA-F]+} $txt {\10x100000} txt]
set n2 [regsub {(_HEAP_SIZE = DEFINED\(_HEAP_SIZE\) \? _HEAP_SIZE : )0x[0-9a-fA-F]+} $txt {\10x100000} txt]
puts "lscript : pile modifiee=$n1 tas modifie=$n2"
set fp [open $ld w]; puts -nonewline $fp $txt; close $fp
app config -name yolo_sd build-config release
app config -name yolo_sd -set compiler-optimization {Optimize more (-O2)}
app config -name yolo_sd -add compiler-misc {-ffp-contract=off}
app config -name yolo_sd -add libraries m
app build -name yolo_sd
puts "FIN : ELF = [glob -nocomplain $ws/yolo_sd/*/yolo_sd.elf]"
