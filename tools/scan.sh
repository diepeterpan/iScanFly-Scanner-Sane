#!/bin/bash
set -e

#read -n 1 -s -r -p "Press any key to continue when the scanner is ready to calibrate"
#./s400w 192.168.18.33 23 calibrate
read -n 1 -s -r -p "Press any key to continue when the scanner is ready"
while true
do
  now=$(date +"%s")
  ./s400w 192.168.18.33 23 scan 600 test.jpg
  magick test.jpg -crop +2120 -fuzz 33% -trim -gravity North +repage "$now".jpg 
  read -n 1 -s -r -p "Press any key to continue when the next photo is loaded"
done
