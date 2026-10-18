# xsct build_app.tcl   (Vitis 2023.1) -- cree la plateforme depuis l'XSA, compile main.c
set root C:/Users/USER/Desktop/yolo_hw
set ws   $root/vitis_ws
set xsa  $root/vivado/design_1_wrapper.xsa
file mkdir $ws
setws $ws

platform create -name plat_tile -hw $xsa -proc ps7_cortexa9_0 -os standalone -out $ws
platform generate

app create -name test_tile -platform plat_tile -domain standalone_domain -template {Empty Application(C)}
# on remplace les sources du modele par les notres
file delete -force $ws/test_tile/src/helloworld.c
file copy -force $root/sw/main.c       $ws/test_tile/src/main.c
file copy -force $root/sw/gemm_pack.h  $ws/test_tile/src/gemm_pack.h
app config -name test_tile build-config release
app config -name test_tile -set compiler-optimization {Optimize more (-O2)}
app build -name test_tile
puts "FIN : ELF = [glob -nocomplain $ws/test_tile/*/test_tile.elf]"
