# xsct build_yolo.tcl  (Vitis 2023.1) -- plateforme depuis l'XSA + application yolo_run
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set xsa  $root/vivado/design_1_wrapper.xsa
set src  $root/sw_net
file mkdir $ws
setws $ws
platform create -name plat_tile -hw $xsa -proc ps7_cortexa9_0 -os standalone -out $ws
platform generate
app create -name yolo_run -platform plat_tile -domain standalone_domain -template {Empty Application(C)}
file delete -force $ws/yolo_run/src/helloworld.c
foreach f {main_zybo.c yolo_core.c yolo_core.h net_def.h expected.h gemm_pack.h} {
  file copy -force $src/$f $ws/yolo_run/src/$f
}
file rename -force $ws/yolo_run/src/main_zybo.c $ws/yolo_run/src/main.c
app config -name yolo_run build-config release
app config -name yolo_run -set compiler-optimization {Optimize more (-O2)}
app config -name yolo_run -add compiler-misc {-ffp-contract=off}
app config -name yolo_run -add libraries m
app build -name yolo_run
puts "FIN : ELF = [glob -nocomplain $ws/yolo_run/*/yolo_run.elf]"
