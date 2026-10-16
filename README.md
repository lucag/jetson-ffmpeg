# jetson-ffmpeg
L4T Multimedia API for ffmpeg

This fork of [jocover/jetson-ffmpeg](https://github.com/jocover/jetson-ffmpeg) fixes three encoder bugs:

- **Widths that aren't a multiple of 64** (720×576, 720×480, 854×480, 1440×1080, …) crashed ffmpeg or corrupted the picture. Frames were copied into the encoder ignoring its buffers' stride. They are now copied row by row.
- **The last frames of every encode were lost.** The encoder was never told the stream had ended. On flush, it now gets an end-of-stream buffer and is drained until its own end of stream comes out.

- **Frames were lost from the middle of about one encode in twenty**, with ffmpeg still exiting 0. Encoded packets went into a fixed pool of 10, and ffmpeg takes at most one per frame it puts in; when the encoder's latency grew, packets were overwritten before ffmpeg took them. They now go into a queue with no bound, each a copy of its own, so a packet is also never overwritten while ffmpeg copies it out.

`tests/check-encoder.sh` checks all three, for `hevc_nvmpi` and `h264_nvmpi`; the third with repeated encodes (`REPEAT`, 60 by default).

**1.build and install library**

    git clone -b fix-stride-and-flush https://github.com/lucag/jetson-ffmpeg.git
    cd jetson-ffmpeg
    mkdir build
    cd build
    cmake ..
    make
    sudo make install
    sudo ldconfig
	
**2.patch ffmpeg and build**

    git clone git://source.ffmpeg.org/ffmpeg.git -b release/4.2 --depth=1
    cd ffmpeg
    git apply ../jetson-ffmpeg/ffmpeg_nvmpi.patch
    ./configure --enable-nvmpi
    make

**3.check the encoders**

    FFMPEG=../ffmpeg/ffmpeg FFPROBE=../ffmpeg/ffprobe tests/check-encoder.sh

Every line should read `ok`, and the script exits 0.

**4.using**

### Supports Decoding
  - MPEG2
  - H.264/AVC
  - HEVC
  - VP8
  - VP9
  
**example**

    ffmpeg -c:v h264_nvmpi -i input_file -f null -
	
### Supports Encoding
  - H.264/AVC
  - HEVC
  
**example**

    ffmpeg -i input_file -c:v h264_nvmpi <output.mp4>
