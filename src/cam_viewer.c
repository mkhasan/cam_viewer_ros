// tutorial04.c
// A pedagogical video player that will stream through every video frame as fast as it can,
// and play audio (out of sync).
//
// Code based on FFplay, Copyright (c) 2003 Fabrice Bellard, 
// and a tutorial by Martin Bohme (boehme@inb.uni-luebeckREMOVETHIS.de)
// Tested on Gentoo, CVS version 5/01/07 compiled with GCC 4.1.1
// With updates from https://github.com/chelyaev/ffmpeg-tutorial
// Updates tested on:
// LAVC 54.59.100, LAVF 54.29.104, LSWS 2.1.101, SDL 1.2.15
// on GCC 4.7.2 in Debian February 2015
// Use
//
// gcc -o tutorial04 tutorial04.c -lavformat -lavcodec -lswscale -lz -lm `sdl-config --cflags --libs`
// to build (assuming libavformat and libavcodec are correctly installed, 
// and assuming you have sdl-config. Please refer to SDL docs for your installation.)
//
// Run using
// tutorial04 myvideofile.mpg
//
// to play the video stream on your screen.

#include "cam_viewer/cam_viewer.h"



#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

//#include <SDL.h>
//#include <SDL_thread.h>

#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>

#include <stdlib.h>


int player_id = -2;
int pause_it = 0;

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

//#define FF_REFRESH_EVENT (SDL_USEREVENT)
//#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1



typedef enum AVPixelFormat out_format_t;


//pthread_mutex_t debug_lock;


typedef struct PacketQueue {
  AVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
} PacketQueue;


typedef struct VideoPicture {
  uint8_t *buffer;
  int width, height; /* source height & width */
  AVFrame *frame;

} VideoPicture;

typedef struct VideoState {

  AVFormatContext *pFormatCtx;
  int             videoStream, audioStream;
  AVStream        *audio_st;
  AVCodecContext  *audio_ctx;
  PacketQueue     audioq;
  uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
  unsigned int    audio_buf_size;
  unsigned int    audio_buf_index;
  AVFrame         audio_frame;
  AVPacket        audio_pkt;
  uint8_t         *audio_pkt_data;
  int             audio_pkt_size;
  AVStream        *video_st;
  AVCodecContext  *video_ctx;
  PacketQueue     videoq;
  struct SwsContext *sws_ctx;

  VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
  int             pictq_size, pictq_rindex, pictq_windex;
  
  pthread_mutex_t pictq_mutex;
  pthread_cond_t pictq_cond;
  pthread_t parse_tid;
  pthread_t video_tid;

  char            filename[1024];



  struct shm_data *pData;

  sem_t *mutex;
  sem_t *debugLock;

  int *quit;

} VideoState;



static out_format_t out_format = AV_PIX_FMT_YUV420P;//AV_PIX_FMT_RGB24;



//SDL_Surface     *screen;
//SDL_mutex       *screen_mutex;
pthread_mutex_t screen_mutex;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;


//void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame);
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const uint8_t * src_y, int src_stride_y,
		const uint8_t * src_u, int src_stride_u, const uint8_t * src_v, int src_stride_v);


void debug_print(const char * str);
void info_print(const char * str);
void warn_print(const char * str);
void error_print(const char * str);


void packet_queue_init(PacketQueue *q) {
  memset(q, 0, sizeof(PacketQueue));
  //q->mutex = SDL_CreateMutex();
  //q->cond = SDL_CreateCond();
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->cond, NULL);

}

void packet_queue_finalize(PacketQueue *q) {

	pthread_mutex_lock(&q->mutex);
	AVPacketList *pkt1 = q->first_pkt;
	while(pkt1 != NULL) {
		q->first_pkt = pkt1->next;
		q->nb_packets --;
		q->size -= pkt1->pkt.size;
		av_packet_unref(&pkt1->pkt);
		av_free(pkt1);
		pkt1 = q->first_pkt;
	}

	q->first_pkt = q->last_pkt = NULL;
	pthread_mutex_unlock(&q->mutex);

	pthread_mutex_destroy(&q->mutex);
	pthread_cond_destroy(&q->cond);

}

int packet_queue_put(PacketQueue *q, AVPacket *p) {

  AVPacketList *pkt1;


  AVPacket pkt;
  av_init_packet(&pkt);
  int ret;
  if(q == NULL || p == NULL)
	  return -1;

  ret = av_packet_ref(&pkt, p);
  if(ret)
	  return ret;

  pkt1 = (AVPacketList*) av_malloc(sizeof(AVPacketList));
  if(!pkt1)
	  return -1;
  pkt1->pkt = pkt;
  pkt1->next = NULL;

  /*
  int ret;

  pkt1 = av_mallocz(sizeof(AVPacketList));
  if (!pkt1)
    return -1;

  if ((ret = av_packet_ref(&pkt1->pkt, pkt)) < 0) {
	  av_free(pkt1);
	  return -1;
  }




  pkt1->pkt = *pkt;
  pkt1->next = NULL;
  */
  
  //SDL_LockMutex(q->mutex);
  pthread_mutex_lock(&q->mutex);

  if (!q->last_pkt)
    q->first_pkt = pkt1;
  else
    q->last_pkt->next = pkt1;
  q->last_pkt = pkt1;
  q->nb_packets++;
  q->size += pkt1->pkt.size;
  //SDL_CondSignal(q->cond);
  pthread_cond_signal(&q->cond);
  
  //SDL_UnlockMutex(q->mutex);
  pthread_mutex_unlock(&q->mutex);
  return 0;
}


static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, sem_t * debugLock)
{
	char msg[256];
	AVPacketList *pkt1;
	int ret;

	//SDL_LockMutex(q->mutex);
	pthread_mutex_lock(&q->mutex);

	for(;;) {


		sprintf(msg, "%d: packet_queue_get: trying to get packet \n", player_id);
		info_print(msg);



		if(*(global_video_state->quit)) {
			ret = -1;
			break;
		}


		sprintf(msg, "%d: packet_queue_get: global_video_state accessed \n", player_id);
		info_print(msg);


		pkt1 = q->first_pkt;


		sprintf(msg, "%d: packet_queue_get: q accessed pkt1 is %x\n", player_id, pkt1);
		info_print(msg);


		if (pkt1) {


			sprintf(msg, "%d: packet_queue_get: pkt1 is non zero \n", player_id);
			info_print(msg);


			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;

			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;

			sprintf(msg, "%d: packet_queue_get: non blocking mode \n", player_id);
			info_print(msg);

			break;
		} else {

			sprintf(msg, "%d: packet_queue_get: condition wait q->cond \n", player_id);
			info_print(msg);

			//SDL_CondWait(q->cond, q->mutex);
			pthread_cond_wait(&q->cond, &q->mutex);

			sprintf(msg, "%d: packet_queue_get: woke up q->cond \n", player_id);
			info_print(msg);

		}
	}

	//SDL_UnlockMutex(q->mutex);
	pthread_mutex_unlock(&q->mutex);
	return ret;
}



//static void schedule_refresh(VideoState *is, int delay);


void video_display(VideoState *is) {


	char msg[256];

	sprintf(msg, "%d: video_display\n", player_id);
	debug_print(msg);


	VideoPicture *vp;
	float aspect_ratio;
	int w, h, x, y;
	int i;
	AVPicture pict;
	vp = &is->pictq[is->pictq_rindex];

	if(vp->buffer) {
		static int count = 0;
		const int gap = 10;


#ifdef _FILE_TEST

	 	if(count++ % gap == 0) {

	 		SaveFrame(NULL, is->video_ctx->width, is->video_ctx->height, count/gap, vp->frame->data[0], vp->frame->linesize[0],
	 				vp->frame->data[1], vp->frame->linesize[1], vp->frame->data[2], vp->frame->linesize[2]);


	 	}

#else
		int terminate = 0;

#ifndef TEST_
		if(sem_wait(is->mutex) < 0) {

			fprintf( stderr, "decode.c: error in acquiring mutex");
			exit(-1);
		}
#endif
		if((is->pData->len < MAX_DATA_SIZE) && (pause_it == 0)) { // don't touch if its ready to terminate

			char *p = (char *)is->pData->data;



			sprintf(p, "P6\n%d %d\n255\n", is->video_ctx->width, is->video_ctx->height);
			int len = strlen(p);

			p += len;
			int width = is->video_ctx->width;
			int height = is->video_ctx->height;
			static uint8_t my_data[MAX_DATA_SIZE];
			int linesize = 3*width ;
			//char debug_info[MAX_LEN];

#ifdef _DEBUG
			assert(linesize*height < MAX_DATA_SIZE-len);
#endif

			int ret = __I420ToRAW(vp->frame->data[0], vp->frame->linesize[0], vp->frame->data[1], vp->frame->linesize[1], vp->frame->data[2], vp->frame->linesize[2], p, linesize, width, height);

			len += linesize*height;
			is->pData->len = len;
			is->pData->width = width;
			is->pData->height = height;

			print_time(is->pData->timestamp);

		}

		if(is->pData->url[0] == NULL) {
			fprintf(stderr, "terminate req received from remote system \n");
			terminate = 1;


		}

		else if(is->pData->url[0] == 0x10) {
			pause_it = 1;
			warn_print("puase it 1");
		}
		else {
			pause_it = 0;
		}


#ifndef TEST_
		if(sem_post(is->mutex) < 0) {
			fprintf( stderr, "decode.c: error in releasing mutex");
			exit(-1);
		}
#endif
		if(terminate == 1) {
			*(is->quit) = TERMINATE;
		}

#endif
	}
	else {

		debug_print("vp buffer is empty");

	}

}

void video_refresh_timer(void *userdata, int64_t *delay) {

	char msg[256];
	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;

	sprintf(msg, "%d: vrt video_st %x pictq_size %d \n", player_id, is->video_st, is->pictq_size);
	//info_print(msg);

	if(is->video_st) {
		if(is->pictq_size == 0) {
			//schedule_refresh(is, 20);
			*delay = 20000;
		} else {
			vp = &is->pictq[is->pictq_rindex];
			/* Now, normally here goes a ton of code
			about timing, etc. we're just going to
			guess at a delay for now. You can
			increase and decrease this value and hard code
			the timing - but I don't suggest that ;)
			We'll learn how to do it for real later.
			*/
			//schedule_refresh(is, 40);
			//warn_print("got it");
			*delay = 40000;

			/* show the picture! */
			video_display(is);

			/* update queue for next picture! */
			if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
			is->pictq_rindex = 0;
			}
			//SDL_LockMutex(is->pictq_mutex);
			pthread_mutex_lock(&is->pictq_mutex);
			is->pictq_size--;
			//SDL_CondSignal(is->pictq_cond);
			pthread_cond_signal(&is->pictq_cond);
			//SDL_UnlockMutex(is->pictq_mutex);
			pthread_mutex_unlock(&is->pictq_mutex);
		}
	} else {
		//schedule_refresh(is, 100);
		*delay = 100000;
	}
}
      
void alloc_picture(void *userdata) {

	char msg[256];

	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;


	sprintf(msg, "%d: alloc_picture\n", player_id);
	debug_print(msg);



	vp = &is->pictq[is->pictq_windex];
	if(vp->buffer) {
		// we already have one make another, bigger/smaller
		assert(0);
		av_free(vp->buffer);
		av_frame_free(&vp->frame);
	}
  // Allocate a place to put our YUV image on that screen
  //SDL_LockMutex(screen_mutex);

	pthread_mutex_lock(&screen_mutex);

  /*
  vp->bmp = SDL_CreateYUVOverlay(is->video_ctx->width,
				 is->video_ctx->height,
				 SDL_YV12_OVERLAY,
				 screen);
				 */

	int numBytes=avpicture_get_size(out_format, is->video_ctx->width,
		is->video_ctx->height);
	vp->buffer = (uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

	vp->frame = av_frame_alloc();

	//SDL_UnlockMutex(screen_mutex);
	pthread_mutex_unlock(&screen_mutex);

	vp->width = is->video_ctx->width;
	vp->height = is->video_ctx->height;
	avpicture_fill((AVPicture *) vp->frame, vp->buffer, out_format,
			is->video_ctx->width, is->video_ctx->height);


}

int queue_picture(VideoState *is, AVFrame *pFrame) {

	char msg[256];


	sprintf(msg, "%d: queue_picture\n", player_id);
	debug_print(msg);


	VideoPicture *vp;
	int dst_pix_fmt;
	AVPicture pict;

	/* wait until we have space for a new pic */
	//SDL_LockMutex(is->pictq_mutex);
	pthread_mutex_lock(&is->pictq_mutex);
	while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
	!(*(is->quit))) {
	  //info_print("waiting");
		//SDL_CondWait(is->pictq_cond, is->pictq_mutex);
		pthread_cond_wait(&is->pictq_cond, &is->pictq_mutex);
	}

	//SDL_UnlockMutex(is->pictq_mutex);
	pthread_mutex_unlock(&is->pictq_mutex);


	sprintf(msg, "%d: queue_picture Unlock Mutex \n", player_id);
	debug_print(msg);

	if(*(is->quit))
		return -1;

	// windex is set to 0 initially
	vp = &is->pictq[is->pictq_windex];

	/* allocate or resize the buffer! */
	if(!vp->buffer ||
	 vp->width != is->video_ctx->width ||
	 vp->height != is->video_ctx->height) {



		sprintf(msg, "%d: alloc_picture \n", player_id);
		debug_print(msg);

		alloc_picture(is);
		if(*(is->quit)) {

			return -1;

		}
	}

	/* We have a place to put our picture on the queue */

	if(vp->buffer) {

	//SDL_LockYUVOverlay(vp->bmp);


		sprintf(msg, "%d: buffer not NULL \n", player_id);
		debug_print(msg);

		dst_pix_fmt = out_format;
		/* point pict at the queue */

		/*
		pict.data[0] = vp->bmp->pixels[0];
		pict.data[1] = vp->bmp->pixels[2];
		pict.data[2] = vp->bmp->pixels[1];

		pict.linesize[0] = vp->bmp->pitches[0];
		pict.linesize[1] = vp->bmp->pitches[2];
		pict.linesize[2] = vp->bmp->pitches[1];

		*/
		// Convert the image into YUV format that SDL uses
		sws_scale(is->sws_ctx, (uint8_t const * const *)pFrame->data,
			  pFrame->linesize, 0, is->video_ctx->height,
			  vp->frame->data, vp->frame->linesize);

		static count = 0;
		const int gap = 10;


		/*

		if(count % gap == 0) {

			SaveFrame(NULL, is->video_ctx->width, is->video_ctx->height, count/gap, vp->frame->data[0], vp->frame->linesize[0],
					vp->frame->data[2], vp->frame->linesize[2], vp->frame->data[1], vp->frame->linesize[1]);

			;//SaveFrame(&pict, is->video_ctx->width, is->video_ctx->height, count/gap);
		}
		count ++;
		*/
		//SDL_UnlockYUVOverlay(vp->bmp);
		/* now we inform our display thread that we have a pic ready */
		if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			is->pictq_windex = 0;
		}
		//SDL_LockMutex(is->pictq_mutex);
		pthread_mutex_lock(&is->pictq_mutex);
		is->pictq_size++;
		//SDL_UnlockMutex(is->pictq_mutex);
		pthread_mutex_unlock(&is->pictq_mutex);


		sprintf(msg, "%d: pictq_size ++ done \n", player_id);
		debug_print(msg);

	}
	return 0;
}

void * video_thread(void *arg) {

	char msg[256];

	VideoState *is = (VideoState *)arg;


	sprintf(msg, "%d: video_thread\n", player_id);
	info_print(msg);



	AVPacket pkt1, *packet = &pkt1;
	int frameFinished;
	AVFrame *pFrame;

	pFrame = av_frame_alloc();

	while(*(is->quit) == 0) {
		if(packet_queue_get(&is->videoq, packet, 1, is->debugLock) < 0) {
			// means we quit getting packets
			fprintf(stderr, "%d: Error in getting packet from the queue \n", player_id);
			break;
		}
		// Decode video frame
		if (avcodec_decode_video2(is->video_ctx, pFrame, &frameFinished, packet) == 0)
			fprintf(stderr, "%d: Error in decoding vidoe \n", player_id);


		sprintf(msg, "%d: video_thread: video decoded frame is finished %d\n", player_id, frameFinished);
		info_print(msg);

		// Did we get a video frame?
		if(frameFinished) {
			if(queue_picture(is, pFrame) < 0) {
				fprintf(stderr, "%d Error in queuing pictures \n", player_id);
				break;
			}
		}


		sprintf(msg, "%d: video_thread: read packet stats %d vid stream %d aud stream %d ref of ref count %x\n", player_id, packet->stream_index, is->videoStream, is->audioStream, packet->buf);
		info_print(msg);


		av_packet_unref(packet);
	}
	av_frame_free(&pFrame);
	fprintf(stderr, "%d: video_thread finishing \n", player_id);
	return NULL;
}

int stream_component_open(VideoState *is, int stream_index) {

  AVFormatContext *pFormatCtx = is->pFormatCtx;
  AVCodecContext *codecCtx = NULL;
  AVCodec *codec = NULL;
  //SDL_AudioSpec wanted_spec, spec;

  if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) {
    return -1;
  }

  codec = avcodec_find_decoder(pFormatCtx->streams[stream_index]->codec->codec_id);
  if(!codec) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  codecCtx = avcodec_alloc_context3(codec);
  if(avcodec_copy_context(codecCtx, pFormatCtx->streams[stream_index]->codec) != 0) {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }


  if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
    // Set audio settings from codec info
	  /*
    wanted_spec.freq = codecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = codecCtx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = is;
    */
    /*
    if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
      fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
      return -1;
    }
    */
  }
  if(avcodec_open2(codecCtx, codec, NULL) < 0) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1;
  }

  int iret;
  switch(codecCtx->codec_type) {
  case AVMEDIA_TYPE_AUDIO:
	  /*
    is->audioStream = stream_index;
    is->audio_st = pFormatCtx->streams[stream_index];
    is->audio_ctx = codecCtx;
    is->audio_buf_size = 0;
    is->audio_buf_index = 0;
    memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
    packet_queue_init(&is->audioq);

    SDL_PauseAudio(0);
    */
    break;
  case AVMEDIA_TYPE_VIDEO:
    is->videoStream = stream_index;
    is->video_st = pFormatCtx->streams[stream_index];
    is->video_ctx = codecCtx;
    packet_queue_init(&is->videoq);
    //is->video_tid = SDL_CreateThread(video_thread, is);

    iret = pthread_create( &is->video_tid, NULL, video_thread, (void *) is);

    if(iret) {
        fprintf(stderr, "Error in creating video thread \n");
        return -1;
    }
    is->sws_ctx = sws_getContext(is->video_ctx->width, is->video_ctx->height,
				 is->video_ctx->pix_fmt, is->video_ctx->width,
				 is->video_ctx->height, out_format,//AV_PIX_FMT_YUV420P,
				 SWS_BILINEAR, NULL, NULL, NULL
				 );

    break;
  default:
    break;
  }

  return 0;
}


int decode_thread(void *arg) {

	char msg[256];


	VideoState *is = (VideoState *)arg;

	sprintf(msg, "%d: decode_thread\n", player_id);
	debug_print(msg);



	AVFormatContext *pFormatCtx = NULL;
	AVPacket pkt1, *packet = &pkt1;
	av_init_packet(packet);

	int video_index = -1;
	int audio_index = -1;
	int i;

	is->videoStream=-1;
	is->audioStream=-1;

	global_video_state = is;

	// Open video file

	int terminate = 0;
	while(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0) {
		if (*(is->quit))
			return -1;

#ifndef TEST_
		if(sem_wait(is->mutex) < 0) {

			fprintf( stderr, "decode.c: error in acquiring mutex");
			exit(-1);
		}

#endif
		terminate = (is->pData->url[0] == 0);		// gets terminating signal from shm_manager controlled by client_interface

#ifndef TEST_
		if(sem_post(is->mutex) < 0) {

			fprintf( stderr, "decode.c: error in acquiring mutex");
			exit(-1);
		}
#endif

		if(terminate) {
			fprintf(stderr, "terminate req received from remote system \n");
			*(is->quit) = TERMINATE;	// quit = 2 measn client handler requests to quit
			return -1;

		}
		fprintf(stderr, "%d: decode_thread: Could not open %s \n", player_id, is->filename);
		sleep(1);

	}



	is->pFormatCtx = pFormatCtx;

	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL)<0) {
		fprintf(stderr, "%d: decode_thread: Could find stream info %s \n", player_id, is->filename);
		return -1; // Couldn't find stream information
	}


	// Dump information about file onto standard error
	//av_dump_format(pFormatCtx, 0, is->filename, 0);

	// Find the first video stream

	for(i=0; i<pFormatCtx->nb_streams; i++) {
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO && video_index < 0) {
			video_index=i;
		}

		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO &&
			audio_index < 0) {
			audio_index=i;
		}
	}
	if(audio_index >= 0) {
		;//stream_component_open(is, audio_index);
	}
	if(video_index >= 0) {
		if (stream_component_open(is, video_index) != 0) {
			fprintf(stderr, "%s: could not open stream component\n", is->filename);
			goto fail;
		}
	}

	if(is->videoStream < 0 ) {
		fprintf(stderr, "%s: could not open codecs\n", is->filename);
		goto fail;
	}

	// main decode loop

	for(;;) {
		if(*(is->quit)) {
			break;
		}
		// seek stuff goes here

		sprintf(msg, "%d: decode_thread: videoq size %d \n", player_id, is->videoq.size);
		debug_print(msg);


		if(is->audioq.size > MAX_AUDIOQ_SIZE ||
			is->videoq.size > MAX_VIDEOQ_SIZE) {
			//SDL_Delay(10);
			usleep(10000);
			continue;
		}


		sprintf(msg, "%d: decode_thread: trying to read frame \n", player_id);
		debug_print(msg);



		if(av_read_frame(is->pFormatCtx, packet) < 0) {
			if(is->pFormatCtx->pb == NULL) {
				sprintf(msg, "%d: decode_thread: pb is NULL \n", player_id);
				error_print(msg);
				*(is->quit) = FRAME_READ_ERROR;
				break;
			}
			if(is->pFormatCtx->pb->error == 0) {
				//SDL_Delay(100); /* no error; wait for user input */
				sprintf(msg, "%d: decode_thread: pb->error is zero so let's continue \n", player_id);
				usleep(100000);
				continue;
			} else {
				sprintf(msg, "%d: decode_thread: Error in reading frame \n", player_id);
				error_print(msg);
				break;
			}
		}


		sprintf(msg, "%d: decode_thread: reading frame done stream is %d vid stream %d aud stream %d ref of ref count %x\n", player_id, packet->stream_index, is->videoStream, is->audioStream, packet->buf);
		debug_print(msg);


		// Is this a packet from the video stream?
		if(packet->stream_index == is->videoStream) {
			if (packet_queue_put(&is->videoq, packet) != 0)
			fprintf(stderr, "%d: Error in putting packet in the Q", player_id);
		}
		/*
		else if(packet->stream_index == is->audioStream) { // not interested to take audio packet
			av_packet_unref(packet);// packet_queue_put(&is->audioq, packet);
		} else {
			av_packet_unref(packet);
		}
		*/

		//sprintf(msg, "%d: decode_thread: read packet stats %d vid stream %d aud stream %d ref of ref count %x\n", player_id, packet->stream_index, is->videoStream, is->audioStream, packet->buf);
		//debug_print(msg);


		//debug_print("before unref");
		av_packet_unref(packet);
		//debug_print("after unref");



	}
	/* all done - wait for it */
	while(*(is->quit) == 0) {
		//SDL_Delay(100);
		usleep(100000);
	}


	sprintf(msg, "%d: decode_thread: going to quit with quit = %d", player_id, (*(is->quit)));
	error_print(msg);


	pthread_cond_signal(&is->videoq.cond);

	debug_print("waiting for video thread");
	pthread_join( is->video_tid, NULL);
	debug_print("done");
	packet_queue_finalize(&is->videoq);
	fprintf(stderr, "%d: decode thread: finishing \n", player_id);

	fail:
	if(1){
	  //assert(0);
		if(*(is->quit) == 0)		// local node did not initiated
			*(is->quit) = DEVICE_ERROR;
	}

	return 0;
}

int play(cam_info_t info) {			// main entry point of this lib



	int iret;

	VideoState      *is;

	player_id = info.id;

	if(info.p == NULL) {
		fprintf(stderr, "Could not get shared mem address");
		return -1;
	}
	is = av_mallocz(sizeof(VideoState));

	is->pData = info.p;
	is->mutex = info.mutex;
	is->quit = info.quit;
	is->debugLock = info.debugLock;


	/*
	if(pthread_mutex_init(&debug_lock, NULL) != 0) {
		printf("Error in initializing debug_lock \n");
		return -1;
	}
	*/

	av_register_all();
	avcodec_register_all();
	avdevice_register_all();
	avformat_network_init();

	//if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
	/*
	if(SDL_Init(SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}
	*/

	//screen_mutex = SDL_CreateMutex();

#ifndef TEST_
	if(sem_wait(is->mutex) < 0) {

		fprintf( stderr, "decode.c: error in acquiring mutex");
		exit(-1);
	}
#endif

	av_strlcpy(is->filename, info.p->url, sizeof(is->filename));

#ifndef TEST_
	if(sem_post(is->mutex) < 0) {

		fprintf( stderr, "decode.c: error in acquiring mutex");
		exit(-1);
	}

#endif

	if(is->filename[0] == NULL) {
		fprintf(stderr, "Invalid filename \n");
		return -1;
	}


	printf("file name is %s \n", is->filename);


	//is->pictq_mutex = SDL_CreateMutex();
	//is->pictq_cond = SDL_CreateCond();

	pthread_mutex_init(&is->pictq_mutex, NULL);
	pthread_cond_init(&is->pictq_cond, NULL);
	pthread_mutex_init(&screen_mutex, NULL);

	//schedule_refresh(is, 40);
	usleep(40000);

	//is->parse_tid = SDL_CreateThread(decode_thread, is);


    iret = pthread_create( &is->parse_tid, NULL, decode_thread, (void *) is);

    if(iret) {
        fprintf(stderr, "Error in creating decode thread \n");
		av_free(is);
		return -1;
    }



	int64_t twakeup, delay, now, tsleep;
	while(*(is->quit) == NO_QUIT) {
		twakeup = av_gettime();
		delay = -1;
		video_refresh_timer(is, &delay);
		assert(delay > 0);
		twakeup += delay;
		now = av_gettime();
		tsleep = twakeup-now;
		if(tsleep > 0) {
			if(tsleep > 1000000) {
				tsleep = 1000000;

				warn_print("too long seep required\n");

			}
			usleep((unsigned int)tsleep);
		}
		else {

			warn_print("no time to sleep\n");

		}

	}

	pthread_cond_signal(&is->pictq_cond);

	pthread_join( is->parse_tid, NULL);

	int i;
	VideoPicture *vp;
	for (i=0; i<VIDEO_PICTURE_QUEUE_SIZE; i++) {
		vp = &is->pictq[is->pictq_windex];
		av_frame_free(&vp->frame);
		if (vp->buffer != NULL) {
			av_free(vp->buffer);
		}
	}


	if(is->video_ctx) {
		avcodec_free_context(&is->video_ctx);
	}

	if(is->sws_ctx) {
		sws_freeContext(is->sws_ctx);
	}

	if(is->pFormatCtx) {
		avformat_close_input(&is->pFormatCtx);
	}



	pthread_mutex_destroy(&is->pictq_mutex);
	pthread_cond_destroy(&is->pictq_cond);
	pthread_mutex_destroy(&screen_mutex);

	av_free(is);
	avformat_network_deinit();
	fprintf(stderr, "%d: freed all", player_id);


	return 0;

}


#include <client_interface/shm_data.h>
void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame, const uint8_t * src_y, int src_stride_y,
		const uint8_t * src_u, int src_stride_u, const uint8_t * src_v, int src_stride_v) {
  FILE *pFile;
  char szFilename[32];
  static uint8_t * data[MAX_DATA_SIZE];
  //static uint8_t * data1[MAX_DATA_SIZE];
  int linesize = 3*width;
  int linesize1;
  int  y;

  //char * buffer = (char *) malloc (width*30);

  //return;
  // Open file
  sprintf(szFilename, "Frame%d.ppm", iFrame);
  pFile=fopen(szFilename, "wb");
  if(pFile==NULL)
    return;

  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);

  // Write pixel data
  int ret;
  //ret = __I420ToRGB24(src_y+y*src_stride_y, src_stride_y, src_u+y*src_stride_u, src_stride_u, src_v+y*src_stride_v, src_stride_v, data, linesize, width, height);

  //ret = __I420ToRGBA(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v, data, linesize, width, height);

  /*
   *   for(y=0; y<height; y++)
    fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, width*3, pFile);
   *
   */

  __I420ToRAW(src_y, src_stride_y, src_u, src_stride_u, src_v, src_stride_v, data, linesize, width, height);
  //for(y=0; y<height; y++) {



	//__ARGBToRGBA(data, linesize, data1, linesize1, width, height);
    fwrite(data, 1, linesize*height, pFile);
	//fwrite(data, 1, linesize, pFile);



  //}

  fprintf(stderr, "linesize is %d widht is %d height is %d y_stride is %d u_stride is %d v stride is %d\n", linesize, width, height, src_stride_y, src_stride_u, src_stride_v);

  //
  // Close file
  fclose(pFile);

  //free (buffer);
}

