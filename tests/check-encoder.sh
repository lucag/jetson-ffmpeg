#!/bin/bash
# Check the nvmpi encoders at widths that aren't multiples of 64 and at
# end of stream: every encode must exit 0, keep every frame, and match its
# source picture (no shear from mishandled strides).
#
# Usage: tests/check-encoder.sh            (uses ffmpeg/ffprobe on PATH)
#        FFMPEG=../ffmpeg/ffmpeg tests/check-encoder.sh
set -u
FFMPEG=${FFMPEG:-ffmpeg}
FFPROBE=${FFPROBE:-ffprobe}
FRAMES=48
MIN_PSNR=30
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failed=0

# The source goes through hflip, so the encoder gets frames whose rows are
# padded beyond the width, as frames from a decoder or a filter usually are.
check() {
	local encoder=$1 size=$2 out="$work/out.mkv"
	local src="testsrc2=size=$size:rate=24,trim=end_frame=$FRAMES,hflip"
	rm -f "$out"
	timeout -k 5 120 "$FFMPEG" -nostdin -v error -y -f lavfi -i "$src" \
		-c:v "$encoder" -b:v 4M "$out" >/dev/null 2>"$work/err"
	local rc=$?
	if [ $rc -ne 0 ]; then
		echo "FAIL $encoder $size: ffmpeg exited $rc"; failed=1; return
	fi
	local frames
	frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
		-show_entries stream=nb_read_frames -of csv=p=0 "$out")
	if [ "$frames" != "$FRAMES" ]; then
		echo "FAIL $encoder $size: $frames frames, expected $FRAMES"; failed=1; return
	fi
	# Compared frame by frame: MKV rounds timestamps to milliseconds, so
	# pairing frames by time would compare neighbours.
	local psnr
	psnr=$("$FFMPEG" -nostdin -v info -i "$out" -f lavfi -i "$src" \
		-lavfi "[0:v]settb=1/24,setpts=N[a];[1:v]settb=1/24,setpts=N[b];[a][b]psnr" -f null - 2>&1 | sed -n 's/.*PSNR.* average:\([0-9.inf]*\).*/\1/p' | tail -1)
	if [ -z "$psnr" ] || { [ "$psnr" != inf ] && [ "${psnr%.*}" -lt $MIN_PSNR ]; }; then
		echo "FAIL $encoder $size: PSNR ${psnr:-unknown} against the source"; failed=1; return
	fi
	echo "ok   $encoder $size: $frames frames, PSNR $psnr"
}

# A decoded SD source, as from a DVD: this crashed the unpatched encoder.
check_decoded() {
	local encoder=$1 src="$work/sd.mpg" out="$work/out.mkv"
	"$FFMPEG" -nostdin -v error -y -f lavfi -i "testsrc2=size=720x576:rate=25,trim=end_frame=$FRAMES" \
		-c:v mpeg2video -b:v 8M "$src" >/dev/null 2>&1
	rm -f "$out"
	timeout -k 5 120 "$FFMPEG" -nostdin -v error -y -i "$src" -c:v "$encoder" -b:v 4M "$out" \
		>/dev/null 2>"$work/err"
	local rc=$?
	if [ $rc -ne 0 ]; then
		echo "FAIL $encoder decoded MPEG-2 720x576: ffmpeg exited $rc"; failed=1; return
	fi
	local frames
	frames=$("$FFPROBE" -v error -count_frames -select_streams v:0 \
		-show_entries stream=nb_read_frames -of csv=p=0 "$out")
	if [ "$frames" != "$FRAMES" ]; then
		echo "FAIL $encoder decoded MPEG-2 720x576: $frames frames, expected $FRAMES"; failed=1; return
	fi
	echo "ok   $encoder decoded MPEG-2 720x576: $frames frames"
}

for encoder in hevc_nvmpi h264_nvmpi; do
	for size in 720x576 720x480 854x480 1440x1080 1920x1080; do
		check "$encoder" "$size"
	done
	check_decoded "$encoder"
done
exit $failed
