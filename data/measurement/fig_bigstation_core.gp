set terminal pdfcairo
set output 'fig_bigstation_core.pdf'
set style line 1 \
	linecolor rgb '#a00000' \
	linetype 1 linewidth 2
set style line 2 \
	linecolor rgb '#00a000' \
	linetype 2 linewidth 2
set style line 3 \
	linecolor rgb '#0000a0' \
	linetype 3 linewidth 2
set style line 4 \
	linecolor rgb '#a0a000' \
	linetype 4 linewidth 2
set style line 5 \
	linecolor rgb '#a000a0' \
	linetype 5 linewidth 2
set style line 6 \
	linecolor rgb '#00a0a0' \
	linetype 6 linewidth 2

set style line 501 lt 1 lc rgb "#000000" lw 2
set border 3 ls 501
set size 1, 0.5
# set xr [-0.2:3]
# set yr [-4:110]

red = "#FF0000"; green = "#00FF00"; blue = "#0000FF"; skyblue = "#87CEEB";
c1 = "#eff3ff"; c2 = "#bdd7e7"; c3 = "#6baed6"; c4 = "#2171b5"; 
set yrange [0:160]
set style data histogram
set style histogram cluster gap 1
set style fill solid
set boxwidth 0.9
set xtics format ""
set grid ytics
set offset -1,0,0,0

set xtics font ', 10'
set ytics font ', 10'
# set xlabel 'Settings' font 'Times-Roman,20'
set ylabel 'Minimum core usage' font 'Times-Roman,10'
set key outside above font 'Times-Roman,10' maxrows 1
# set key off
# set grid xtics ytics mxtics mytics
# set mxtics 1
# set mytics 1
set style fill solid border -1
set arrow from graph 0,first 29 to graph 1,first 29 nohead dashtype 17 lc "red" front
set arrow from graph 0,first 58 to graph 1,first 58 nohead dashtype 17 lc "red" front
set arrow from graph 0,first 87 to graph 1,first 87 nohead dashtype 17 lc "red" front
set arrow from graph 0,first 116 to graph 1,first 116 nohead dashtype 17 lc "red" front
set arrow from graph 0,first 145 to graph 1,first 145 nohead dashtype 17 lc "red" front
set label "1 server" at 6.3, 22 font 'Times-Roman,8'
set label "2 servers" at 6.3, 51 font 'Times-Roman,8'
set label "3 servers" at 6.3, 80 font 'Times-Roman,8'
set label "4 servers" at 6.3, 109 font 'Times-Roman,8'
set label "5 servers" at 6.3, 138 font 'Times-Roman,8'
plot 'fig_bigstation_core.txt' using 2:xtic(1) title "Hydra-UL" linecolor rgb c1 lw 1.5, '' using 0:2:2 with labels notitle offset -2.6,0.5 font 'Times-Roman,10', \
	'fig_bigstation_core.txt' using ($3>=8?$3/1.:1/0) title "BigStation-UL" linecolor rgb c2 lw 1.5, '' using 0:($3>=8?$3/1.:1/0):($3>=8?$3/1.:1/0) with labels notitle offset -0.8,0.5 font 'Times-Roman,10', \
	'fig_bigstation_core.txt' using ($4>=8?$4/1.:1/0) title "Hydra-DL" linecolor rgb c3 lw 1.5, '' using 0:($4>=8?$4/1.:1/0):($4>=8?$4/1.:1/0) with labels notitle offset 0.8,0.5 font 'Times-Roman,10', \
	'fig_bigstation_core.txt' using ($5>=8?$5/1.:1/0) title "BigStation-DL" linecolor rgb c4 lw 1.5, '' using 0:($5>=8?$5/1.:1/0):($5>=8?$5/1.:1/0) with labels notitle offset 2.6,0.5 font 'Times-Roman,10'