set title "BFS on Twitter Data (16 processes)"
set terminal postscript eps color size 3.3,2
set output "gnuplot_real_16_mirasol_bfs.eps"
set xrange [0.5:4.5]

set datafile missing "-"

set yrange [0.01:16]
set logscale y
set grid ytics mytics lt 1 lc rgb "#EEEEEE"
set xlabel 'Twitter Input Graph'
set ylabel 'Mean BFS Time (seconds, log scale)'
set key right bottom
set xtics ('small' 1, 'medium' 2, 'large' 3, 'huge' 4)
plot\
 "gnuplot_real_16_mirasol_bfs.hand" using 1:2 title 'KDT' lw 7 lc rgb '#FF0000' with lines,\
 "gnuplot_real_16_mirasol_bfs.hand" using 1:3 title 'SEJITS+KDT' lw 7 lc rgb '#0000FF' with lines,\
 "gnuplot_real_16_mirasol_bfs.hand" using 1:4 title 'CombBLAS' lw 7 lc rgb '#DAA520' with lines