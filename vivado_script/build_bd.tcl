# Vivado 2023.1 : vivado -mode batch -source build_bd.tcl
# Cree le projet Zybo Z7-20 : PS + IP gemm_tile a 100 MHz, genere bitstream + XSA
set root     C:/Users/USER/Desktop/yolo_hw
set proj_dir $root/vivado
set ip_repo  $root/ip/gemm_tile_prj/sol_100mhz/impl/ip

create_project yolo_hw $proj_dir -part xc7z020clg400-1 -force
set_property ip_repo_paths $ip_repo [current_project]
update_ip_catalog -rebuild

create_bd_design design_1

# --- Processing System : configuration reprise de l'XSA d'Armin (carte Zybo Z7-20 qui marche) ---
create_bd_cell -type ip -vlnv xilinx.com:ip:processing_system7:5.5 processing_system7_0
set ps [get_bd_cells processing_system7_0]
source $root/vivado_script/ps7_config.tcl      ;# genere par extract_ps7.ps1
# nos besoins a nous : horloge 100 MHz, GP0 (controle), HP0 (acces memoire de l'IP)
set_property -dict [list \
  CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ {100} \
  CONFIG.PCW_USE_M_AXI_GP0 {1} \
  CONFIG.PCW_USE_S_AXI_HP0 {1}] $ps
puts "VERIF PS7 : UART1=[get_property CONFIG.PCW_UART1_PERIPHERAL_ENABLE $ps]  DDR=[get_property CONFIG.PCW_UIPARAM_DDR_PARTNO $ps]  FCLK0=[get_property CONFIG.PCW_FPGA0_PERIPHERAL_FREQMHZ $ps]"
make_bd_intf_pins_external [get_bd_intf_pins processing_system7_0/DDR]
make_bd_intf_pins_external [get_bd_intf_pins processing_system7_0/FIXED_IO]

# --- IP HLS ---
create_bd_cell -type ip -vlnv xilinx.com:hls:gemm_tile:1.0 gemm_tile_0

# --- Interconnexions ---
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_ctl
set_property -dict [list CONFIG.NUM_SI {1} CONFIG.NUM_MI {1}] [get_bd_cells sc_ctl]
create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect:1.0 sc_hp
set_property -dict [list CONFIG.NUM_SI {3} CONFIG.NUM_MI {1}] [get_bd_cells sc_hp]
create_bd_cell -type ip -vlnv xilinx.com:ip:proc_sys_reset:5.0 rst_ps

connect_bd_intf_net [get_bd_intf_pins processing_system7_0/M_AXI_GP0] [get_bd_intf_pins sc_ctl/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_ctl/M00_AXI]                  [get_bd_intf_pins gemm_tile_0/s_axi_control]
connect_bd_intf_net [get_bd_intf_pins gemm_tile_0/m_axi_gmem0] [get_bd_intf_pins sc_hp/S00_AXI]
connect_bd_intf_net [get_bd_intf_pins gemm_tile_0/m_axi_gmem1] [get_bd_intf_pins sc_hp/S01_AXI]
connect_bd_intf_net [get_bd_intf_pins gemm_tile_0/m_axi_gmem2] [get_bd_intf_pins sc_hp/S02_AXI]
connect_bd_intf_net [get_bd_intf_pins sc_hp/M00_AXI]           [get_bd_intf_pins processing_system7_0/S_AXI_HP0]

# --- Horloge 100 MHz et resets ---
set clk [get_bd_pins processing_system7_0/FCLK_CLK0]
connect_bd_net $clk [get_bd_pins processing_system7_0/M_AXI_GP0_ACLK]
connect_bd_net $clk [get_bd_pins processing_system7_0/S_AXI_HP0_ACLK]
connect_bd_net $clk [get_bd_pins sc_ctl/aclk]
connect_bd_net $clk [get_bd_pins sc_hp/aclk]
connect_bd_net $clk [get_bd_pins gemm_tile_0/ap_clk]
connect_bd_net $clk [get_bd_pins rst_ps/slowest_sync_clk]
connect_bd_net [get_bd_pins processing_system7_0/FCLK_RESET0_N] [get_bd_pins rst_ps/ext_reset_in]
connect_bd_net [get_bd_pins rst_ps/peripheral_aresetn] [get_bd_pins sc_ctl/aresetn]
connect_bd_net [get_bd_pins rst_ps/peripheral_aresetn] [get_bd_pins sc_hp/aresetn]
connect_bd_net [get_bd_pins rst_ps/peripheral_aresetn] [get_bd_pins gemm_tile_0/ap_rst_n]

assign_bd_address
validate_bd_design
save_bd_design

# --- Wrapper, synthese, implementation, bitstream ---
make_wrapper -files [get_files $proj_dir/yolo_hw.srcs/sources_1/bd/design_1/design_1.bd] -top
add_files -norecurse $proj_dir/yolo_hw.gen/sources_1/bd/design_1/hdl/design_1_wrapper.v
set_property top design_1_wrapper [current_fileset]
launch_runs impl_1 -to_step write_bitstream -jobs 4
wait_on_run impl_1

open_run impl_1
report_timing_summary -file $root/logs/vivado_timing.rpt
report_utilization    -file $root/logs/vivado_util.rpt
write_hw_platform -fixed -include_bit -force $proj_dir/design_1_wrapper.xsa
puts "FIN : XSA = $proj_dir/design_1_wrapper.xsa"
