#!/usr/bin/env zsh
set -euo pipefail

SCRIPT_DIR="${0:a:h}"
RESOURCE_DIR="${SCRIPT_DIR}/resources"
mkdir -p "$RESOURCE_DIR"
CHARS=("A" "B" "C")

mkdir -p "${RESOURCE_DIR}"

for char in "${CHARS[@]}"; do
  magick -size 64x64 \
         -background white \
         -fill black \
         -font "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" \
         -gravity center \
         label:"${char}" \
         "${RESOURCE_DIR}/${char}.png"
done

# One second per letter: loop each still for 1s, then concatenate.
inputs=()
for char in "${CHARS[@]}"; do
  inputs+=(-loop 1 -t 1 -framerate 30 -i "${RESOURCE_DIR}/${char}.png")
done

filter=""
for (( i = 0; i < ${#CHARS[@]}; i++ )); do
  filter+="[${i}:v]"
done
filter+="concat=n=${#CHARS[@]}:v=1:a=0[v]"

ffmpeg -y "${inputs[@]}" \
       -filter_complex "${filter}" \
       -map "[v]" \
       -c:v libx264 \
       -pix_fmt yuv420p \
       "${RESOURCE_DIR}/ABC.mp4"

# 12.mp4: flashes numbers 0 through 11, one second each.
NUMS=({0..11})

for num in "${NUMS[@]}"; do
  magick -size 64x64 \
         -background white \
         -fill black \
         -font "/System/Library/Fonts/Supplemental/Arial Unicode.ttf" \
         -gravity center \
         label:"${num}" \
         "${RESOURCE_DIR}/${num}.png"
done

inputs=()
for num in "${NUMS[@]}"; do
  inputs+=(-loop 1 -t 1 -framerate 30 -i "${RESOURCE_DIR}/${num}.png")
done

filter=""
for (( i = 0; i < ${#NUMS[@]}; i++ )); do
  filter+="[${i}:v]"
done
filter+="concat=n=${#NUMS[@]}:v=1:a=0[v]"

ffmpeg -y "${inputs[@]}" \
       -filter_complex "${filter}" \
       -map "[v]" \
       -c:v libx264 \
       -pix_fmt yuv420p \
       "${RESOURCE_DIR}/12.mp4"
