# xsct diag_cpu1.tcl -- APRES un run ou le coeur 1 ne repond pas : ne reinitialise rien, affiche l'etat des deux coeurs et la zone partagee.
connect
foreach t {0 1} {
    targets -set -nocase -filter "name =~ \"ARM*#$t\""
    puts "=== coeur $t ==="
    puts [state]
    catch { stop; after 200; puts "pc = [rrd pc]"; puts "cpsr = [rrd cpsr]" }
}
targets -set -nocase -filter {name =~ "ARM*#0"}
puts "=== zone partagee 0x1F000000 (go done ready M N K ...) ==="
puts [mrd -force 0x1F000000 18]
puts "=== DIAG 0x1F000080 (mot 16 = etat du coeur 1, 17 = travaux) ==="
puts [mrd -force 0x1F000080 20]
exit
