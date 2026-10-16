#include "nvmpi.h"
#include "NvVideoEncoder.h"
#include "nvbuf_utils.h"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <memory>

#define CHUNK_SIZE 2*1024*1024
// How long get_packet waits for each packet while draining after end of stream.
#define DRAIN_TIMEOUT_MS 10000
#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cout<< message;                         \
}


using namespace std;

struct nvEncodedPacket{
	std::vector<unsigned char> data;
	uint64_t pts;
	bool key;
};

struct nvmpictx{
	NvVideoEncoder *enc;
	int index;
	uint32_t width;
	uint32_t height;
	uint32_t profile;
	bool enableLossless;
	uint32_t bitrate;
	uint32_t peak_bitrate;
	uint32_t raw_pixfmt;
	uint32_t encoder_pixfmt;
	enum v4l2_mpeg_video_bitrate_mode ratecontrol;
	enum v4l2_mpeg_video_h264_level level;
	enum v4l2_enc_hw_preset_type hw_preset_type;
	uint32_t iframe_interval;
	uint32_t idr_interval;
	uint32_t fps_n;
	uint32_t fps_d;
	bool enable_extended_colorformat;
	uint32_t qmax;
	uint32_t qmin;
	uint32_t num_b_frames;
	uint32_t num_reference_frames;
	bool insert_sps_pps_at_idr;

	uint32_t packets_num;  // buffers in each plane

	// The capture thread queues a copy of each encoded packet; get_packet
	// takes them in order. The queue has no bound: ffmpeg takes at most one
	// packet per frame it puts, so packets pile up whenever the encoder's
	// latency grows, and none may be dropped.
	std::queue<std::unique_ptr<nvEncodedPacket>> * packets;
	std::unique_ptr<nvEncodedPacket> * taken;  // the last packet get_packet returned
	std::mutex * lock;
	std::condition_variable * packet_ready;
	bool eos_sent;      // put_frame(NULL) queued the end-of-stream buffer
	bool eos_received;  // the encoder's end of stream came out, or the capture thread stopped
};

static void mark_drained(nvmpictx *ctx){
	std::lock_guard<std::mutex> guard(*ctx->lock);
	ctx->eos_received=true;
	ctx->packet_ready->notify_all();
}


static bool encoder_capture_plane_dq_callback(struct v4l2_buffer *v4l2_buf, NvBuffer * buffer, NvBuffer * shared_buffer, void *arg){

	nvmpictx *ctx = (nvmpictx *) arg;
	NvVideoEncoder *enc = ctx->enc;
	//uint32_t frame_num = ctx->enc->capture_plane.getTotalDequeuedBuffers() - 1;

	if (v4l2_buf == NULL)
	{
		cout << "Error while dequeing buffer from output plane" << endl;
		mark_drained(ctx);
		return false;
	}

	// An empty buffer is the encoder's end of stream: every frame is out.
	if (buffer->planes[0].bytesused == 0)
	{
		mark_drained(ctx);
		return false;
	}

	std::unique_ptr<nvEncodedPacket> packet(new nvEncodedPacket);
	packet->data.assign(buffer->planes[0].data,buffer->planes[0].data+buffer->planes[0].bytesused);
	packet->pts=(v4l2_buf->timestamp.tv_usec % 1000000) + (v4l2_buf->timestamp.tv_sec * 1000000UL);

	v4l2_ctrl_videoenc_outputbuf_metadata enc_metadata;
	ctx->enc->getMetadata(v4l2_buf->index, enc_metadata);
	packet->key=enc_metadata.KeyFrame;

	{
		std::lock_guard<std::mutex> guard(*ctx->lock);
		ctx->packets->push(std::move(packet));
		ctx->packet_ready->notify_all();
	}

	if (ctx->enc->capture_plane.qBuffer(*v4l2_buf, NULL) < 0)
	{

		ERROR_MSG("Error while Qing buffer at capture plane");
		return false;
	}

	return true;
}


nvmpictx* nvmpi_create_encoder(nvCodingType codingType,nvEncParam * param){

	int ret;
	log_level = LOG_LEVEL_INFO;
	nvmpictx *ctx=new nvmpictx;
	ctx->index=0;
	ctx->width=param->width;
	ctx->height=param->height;
	ctx->enableLossless=false;
	ctx->bitrate=param->bitrate;
	ctx->peak_bitrate=param->peak_bitrate;
	ctx->ratecontrol = V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;	
	ctx->idr_interval = param->idr_interval;
	ctx->fps_n = param->fps_n;
	ctx->fps_d = param->fps_d;
	ctx->iframe_interval = param->iframe_interval;
	ctx->packets=new std::queue<std::unique_ptr<nvEncodedPacket>>;
	ctx->taken=new std::unique_ptr<nvEncodedPacket>;
	ctx->lock=new std::mutex;
	ctx->packet_ready=new std::condition_variable;
	ctx->eos_sent=false;
	ctx->eos_received=false;
	ctx->enable_extended_colorformat=false;
	ctx->packets_num=param->capture_num;
	ctx->qmax=param->qmax;
	ctx->qmin=param->qmin;
	ctx->num_b_frames=param->max_b_frames;
	ctx->num_reference_frames=param->refs;
	ctx->insert_sps_pps_at_idr=(param->insert_spspps_idr==1)?true:false;

	switch(param->profile){
		case 77://FF_PROFILE_H264_MAIN
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;
		case 66://FF_PROFILE_H264_BASELINE
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
			break;
		case 100://FF_PROFILE_H264_HIGH
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
			break;

		default:
			ctx->profile=V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
			break;

	}

	switch(param->level){
		case 10:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
			break;
		case 11:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
			break;
		case 12:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
			break;
		case 13:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
			break;
		case 20:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
			break;
		case 21:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
			break;
		case 22:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
			break;
		case 30:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
			break;
		case 31:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
			break;
		case 32:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
			break;
		case 40:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
			break;
		case 41:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
			break;
		case 42:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
			break;
		case 50:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_0;
			break;
		case 51:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;
		default:
			ctx->level=V4L2_MPEG_VIDEO_H264_LEVEL_5_1;
			break;	
	}

	switch(param->hw_preset_type){
		case 1:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_ULTRAFAST;
			break;
		case 2:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_FAST;
			break;
		case 3:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;
		case 4:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_SLOW;
			break;
		default:
			ctx->hw_preset_type = V4L2_ENC_HW_PRESET_MEDIUM;
			break;

	}



	if(param->enableLossless)
		ctx->enableLossless=true;

	if(param->mode_vbr)
		ctx->ratecontrol=V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;

	if(codingType==NV_VIDEO_CodingH264){
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H264;
	}else if(codingType==NV_VIDEO_CodingHEVC){
		ctx->encoder_pixfmt=V4L2_PIX_FMT_H265;
	}
	ctx->enc=NvVideoEncoder::createVideoEncoder("enc0");
	TEST_ERROR(!ctx->enc, "Could not create encoder",ret);

	ret = ctx->enc->setCapturePlaneFormat(ctx->encoder_pixfmt, ctx->width,ctx->height, CHUNK_SIZE);

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	switch (ctx->profile)
	{
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN10:
			ctx->raw_pixfmt = V4L2_PIX_FMT_P010M;
			break;
		case V4L2_MPEG_VIDEO_H265_PROFILE_MAIN:
		default:
			ctx->raw_pixfmt = V4L2_PIX_FMT_YUV420M;
	}

	if (ctx->enableLossless && codingType == NV_VIDEO_CodingH264)
	{
		ctx->profile = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE;
		ret = ctx->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV444M, ctx->width,ctx->height);
	}
	else{
		ret = ctx->enc->setOutputPlaneFormat(ctx->raw_pixfmt, ctx->width,ctx->height);
	}

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	ret = ctx->enc->setBitrate(ctx->bitrate);
	TEST_ERROR(ret < 0, "Could not set encoder bitrate", ret);

	ret=ctx->enc->setHWPresetType(ctx->hw_preset_type);
	TEST_ERROR(ret < 0, "Could not set encoder HW Preset Type", ret);

	if(ctx->num_reference_frames){
		ret = ctx->enc->setNumReferenceFrames(ctx->num_reference_frames);
		TEST_ERROR(ret < 0, "Could not set num reference frames", ret);
	}

	if(ctx->num_b_frames != (uint32_t) -1 && codingType == NV_VIDEO_CodingH264 ){
		ret = ctx->enc->setNumBFrames(ctx->num_b_frames);
		TEST_ERROR(ret < 0, "Could not set number of B Frames", ret);
	}


	if (codingType == NV_VIDEO_CodingH264 || codingType == NV_VIDEO_CodingHEVC)
	{
		ret = ctx->enc->setProfile(ctx->profile);
		TEST_ERROR(ret < 0, "Could not set encoder profile", ret);
	}

	if( codingType== NV_VIDEO_CodingH264){
		ret = ctx->enc->setLevel(ctx->level);
		TEST_ERROR(ret < 0, "Could not set encoder level", ret);
	}


	if (ctx->enableLossless){
		ret = ctx->enc->setConstantQp(0);
		TEST_ERROR(ret < 0, "Could not set encoder constant qp=0", ret);

	}else{

		ret = ctx->enc->setRateControlMode(ctx->ratecontrol);
		TEST_ERROR(ret < 0, "Could not set encoder rate control mode", ret);

		if (ctx->ratecontrol == V4L2_MPEG_VIDEO_BITRATE_MODE_VBR){

			uint32_t peak_bitrate;
			if (ctx->peak_bitrate < ctx->bitrate)
				peak_bitrate = 1.2f * ctx->bitrate;
			else
				peak_bitrate = ctx->peak_bitrate;
			ret = ctx->enc->setPeakBitrate(peak_bitrate);
			TEST_ERROR(ret < 0, "Could not set encoder peak bitrate", ret);
		}
	}

	ret = ctx->enc->setIDRInterval(ctx->idr_interval);
	TEST_ERROR(ret < 0, "Could not set encoder IDR interval", ret);

	if(ctx->qmax>0 ||ctx->qmin >0){
		ctx->enc->setQpRange(ctx->qmin, ctx->qmax, ctx->qmin,ctx->qmax, ctx->qmin, ctx->qmax);	
	}
	ret = ctx->enc->setIFrameInterval(ctx->iframe_interval);
	TEST_ERROR(ret < 0, "Could not set encoder I-Frame interval", ret);
	
	if(ctx->insert_sps_pps_at_idr){
		ret = ctx->enc->setInsertSpsPpsAtIdrEnabled(true);
		TEST_ERROR(ret < 0, "Could not set insertSPSPPSAtIDR", ret);
	}

	ret = ctx->enc->setFrameRate(ctx->fps_n, ctx->fps_d);
	TEST_ERROR(ret < 0, "Could not set framerate", ret);

	ret = ctx->enc->output_plane.setupPlane(V4L2_MEMORY_USERPTR, ctx->packets_num, false, true);
	TEST_ERROR(ret < 0, "Could not setup output plane", ret);

	ret = ctx->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, ctx->packets_num, true, false);
	TEST_ERROR(ret < 0, "Could not setup capture plane", ret);

	ret = ctx->enc->subscribeEvent(V4L2_EVENT_EOS,0,0);
	TEST_ERROR(ret < 0, "Could not subscribe EOS event", ret);

	ret = ctx->enc->output_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in output plane streamon", ret);

	ret = ctx->enc->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in capture plane streamon", ret);


	ctx->enc->capture_plane.setDQThreadCallback(encoder_capture_plane_dq_callback);

	ctx->enc->capture_plane.startDQThread(ctx);

	// Enqueue all the empty capture plane buffers
	for (uint32_t i = 0; i < ctx->enc->capture_plane.getNumBuffers(); i++){
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];
		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, MAX_PLANES * sizeof(struct v4l2_plane));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;

		ret = ctx->enc->capture_plane.qBuffer(v4l2_buf, NULL);
		TEST_ERROR(ret < 0, "Error while queueing buffer at capture plane", ret);

	}

	return ctx;
}


// Copy a plane row by row: the frame's rows may be padded (its linesize),
// while the encoder's buffer has its own stride.
static void copy_plane(NvBuffer::NvBufferPlane *plane, const unsigned char *src,
		unsigned int src_stride, unsigned long src_size){

	uint32_t row_bytes=plane->fmt.width*plane->fmt.bytesperpixel;
	uint32_t rows=plane->fmt.height;

	if(src_stride==0) // no linesize given: rows are as long as the plane is wide
		src_stride=row_bytes;
	if(src_size && src_size/src_stride < rows) // never read past the frame
		rows=src_size/src_stride;

	for(uint32_t y=0;y<rows;y++)
		memcpy(plane->data+y*plane->fmt.stride,src+y*src_stride,std::min(row_bytes,src_stride));

	plane->bytesused=plane->fmt.stride*plane->fmt.height;
}

int nvmpi_encoder_put_frame(nvmpictx* ctx,nvFrame* frame){
	int ret;

	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if(ctx->enc->isInError())
		return -1;

	if(frame==NULL && ctx->eos_sent)
		return 0;

	if(ctx->index < ctx->enc->output_plane.getNumBuffers()){

		nvBuffer=ctx->enc->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index ;
		ctx->index++;

	}else{
		ret = ctx->enc->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0) {
			cout << "Error DQing buffer at output plane" << std::endl;
			return -1;
		}

	}

	if(frame==NULL){
		// End of stream: an empty buffer. The encoder answers, once every
		// frame queued before it is encoded, with an empty capture buffer.
		for(uint32_t i=0;i<nvBuffer->n_planes;i++)
			nvBuffer->planes[i].bytesused=0;

		{
			std::lock_guard<std::mutex> guard(*ctx->lock);
			ctx->eos_sent=true;
		}
		ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
		if(ret < 0){
			cout << "Error while queueing end of stream at output plane" << std::endl;
			mark_drained(ctx);
			return -1;
		}
		return 0;
	}

	for(uint32_t i=0;i<3 && i<nvBuffer->n_planes;i++)
		copy_plane(&nvBuffer->planes[i],frame->payload[i],frame->linesize[i],frame->payload_size[i]);

	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_usec = frame->timestamp % 1000000;
	v4l2_buf.timestamp.tv_sec = frame->timestamp / 1000000;

	ret = ctx->enc->output_plane.qBuffer(v4l2_buf, NULL);
	TEST_ERROR(ret < 0, "Error while queueing buffer at output plane", ret);

	return 0;
}

// Take the next encoded packet. Before end of stream it doesn't wait: -1 means
// none is ready yet. After put_frame(NULL) it waits for each packet, and -1
// means the encoder is drained. The payload stays valid until the next call.
int nvmpi_encoder_get_packet(nvmpictx* ctx,nvPacket* packet){

	std::unique_lock<std::mutex> guard(*ctx->lock);

	if(ctx->eos_sent){
		bool ready=ctx->packet_ready->wait_for(guard,std::chrono::milliseconds(DRAIN_TIMEOUT_MS),
				[ctx]{return !ctx->packets->empty() || ctx->eos_received;});
		if(!ready)
			cout << "Timed out draining the encoder" << std::endl;
	}

	if(ctx->packets->empty())
		return -1;

	*ctx->taken=std::move(ctx->packets->front());
	ctx->packets->pop();
	guard.unlock();

	nvEncodedPacket *taken=ctx->taken->get();
	packet->payload=taken->data.data();
	packet->payload_size=taken->data.size();
	packet->pts=taken->pts;
	if(taken->key)
		packet->flags|= 0x0001;//AV_PKT_FLAG_KEY 0x0001
	return 0;
}

int nvmpi_encoder_close(nvmpictx* ctx){

	ctx->enc->capture_plane.stopDQThread();
	ctx->enc->capture_plane.waitForDQThread(1000);
	delete ctx->enc;
	delete ctx->packets;
	delete ctx->taken;
	delete ctx->packet_ready;
	delete ctx->lock;
	delete ctx;
	return 0;
}

