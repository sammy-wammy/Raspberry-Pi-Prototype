/* 
 * File:   opencv_demo.c
 * Author: Hassan : author Tasanakorn
 *
 * Created on Oct 1, 2013, 00:00 AM
 */

#include <stdio.h>
#include <stdlib.h>

#include <opencv2/core/core_c.h>
#include <opencv2/objdetect/objdetect.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"

#include "vgfont.h"
#include "wiringPi.h"

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

/* GPIO pin assignment */
#define BUZZ 0
#define BUTTON 2
#define SLC_BUTTON 3
#define FACE 7
#define L_TURN 4
#define R_TURN 5
/* ******************* */

typedef struct {
    int video_width;
    int video_height;
    int preview_width;
    int preview_height;
    int opencv_width;
    int opencv_height;
    float video_fps;
    MMAL_POOL_T *camera_video_port_pool;
    CvHaarClassifierCascade *cascade;
    CvMemStorage* storage;
    IplImage* image;
    IplImage* image2;
    VCOS_SEMAPHORE_T complete_semaphore;
} PORT_USERDATA;

static void video_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    static int frame_count = 0;
    static int frame_post_count = 0;
    static struct timespec t1;
    struct timespec t2;
    MMAL_BUFFER_HEADER_T *new_buffer;
    PORT_USERDATA * userdata = (PORT_USERDATA *) port->userdata;
    MMAL_POOL_T *pool = userdata->camera_video_port_pool;

    if (frame_count == 0) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
    }
    frame_count++;

    //img = cvLoadImage("test.jpg",CV_LOAD_IMAGE_COLOR);
    mmal_buffer_header_mem_lock(buffer);
    memcpy(userdata->image->imageData, buffer->data, userdata->video_width * userdata->video_height);
    mmal_buffer_header_mem_unlock(buffer);
    //printf("img = %d w=%d, h=%d\n", img, img->width, img->height);

    if (vcos_semaphore_trywait(&(userdata->complete_semaphore)) != VCOS_SUCCESS) {
        vcos_semaphore_post(&(userdata->complete_semaphore));
        frame_post_count++;
    }

    if (frame_count % 10 == 0) {
        // print framerate every n frame
        clock_gettime(CLOCK_MONOTONIC, &t2);
        float d = (t2.tv_sec + t2.tv_nsec / 1000000000.0) - (t1.tv_sec + t1.tv_nsec / 1000000000.0);
        float fps = 0.0;

        if (d > 0) {
            fps = frame_count / d;
        } else {
            fps = frame_count;
        }
        userdata->video_fps = fps;
       // printf("  Frame = %d, Frame Post %d, Framerate = %.0f fps \n", frame_count, frame_post_count, fps);
    }

    mmal_buffer_header_release(buffer);
    // and send one back to the port (if still open)
    if (port->is_enabled) {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pool->queue);

        if (new_buffer)
            status = mmal_port_send_buffer(port, new_buffer);

        if (!new_buffer || status != MMAL_SUCCESS)
            printf("Unable to return a buffer to the video port\n");
    }
}

int main(int argc, char** argv) {
    /* GPIO pins setup */
    wiringPiSetup();
    pinMode(BUZZ, OUTPUT);
    pinMode(BUTTON, INPUT);
    pinMode(SLC_BUTTON, INPUT);
    pinMode(FACE, OUTPUT);
    pinMode(L_TURN, INPUT);
    pinMode(R_TURN, INPUT);
    /* *************** */

    MMAL_COMPONENT_T *camera = 0;
    MMAL_COMPONENT_T *preview = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;
    MMAL_PORT_T *camera_preview_port = NULL, *camera_video_port = NULL, *camera_still_port = NULL;
    MMAL_PORT_T *preview_input_port = NULL;
    MMAL_POOL_T *camera_video_port_pool;
    MMAL_CONNECTION_T *camera_preview_connection = 0;
    PORT_USERDATA userdata;
    int display_width, display_height;

    printf("Running...\n");

    bcm_host_init();

    userdata.preview_width = 1280 / 1;
    userdata.preview_height = 720 / 1;
    userdata.video_width = 1280 / 1;
    userdata.video_height = 720 / 1;
    userdata.opencv_width = 1280 / 4;
    userdata.opencv_height = 720 / 4;


    graphics_get_display_size(0, &display_width, &display_height);

    float r_w, r_h;
    r_w = (float) display_width / (float) userdata.opencv_width;
    r_h = (float) display_height / (float) userdata.opencv_height;

    printf("Display resolution = (%d, %d)\n", display_width, display_height);

    /* setup opencv */
    userdata.cascade = (CvHaarClassifierCascade*) cvLoad("/usr/share/opencv/haarcascades/haarcascade_frontalface_alt.xml", NULL, NULL, NULL);
    CvHaarClassifierCascade* eyes_cascade = (CvHaarClassifierCascade*) cvLoad("/usr/share/opencv/haarcascades/haarcascade_eye.xml", NULL, NULL, NULL); //<--
    userdata.storage = cvCreateMemStorage(0);
    userdata.image = cvCreateImage(cvSize(userdata.video_width, userdata.video_height), IPL_DEPTH_8U, 1);
    userdata.image2 = cvCreateImage(cvSize(userdata.opencv_width, userdata.opencv_height), IPL_DEPTH_8U, 1);
    if (!userdata.cascade) {
        printf("Error: unable to load harrcascade\n");
    }
    //printf("Load cascade at %d\n", userdata.cascade);

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
    if (status != MMAL_SUCCESS) {
        printf("Error: create camera %x\n", status);
        return -1;
    }

    camera_preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
            { MMAL_PARAMETER_CAMERA_CONFIG, sizeof (cam_config)},
            .max_stills_w = 1280,
            .max_stills_h = 720,
            .stills_yuv422 = 0,
            .one_shot_stills = 0,
            .max_preview_video_w = 1280,
            .max_preview_video_h = 720,
            .num_preview_video_frames = 2,
            .stills_capture_circular_buffer_height = 0,
            .fast_preview_resume = 1,
            .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
        };

        mmal_port_parameter_set(camera->control, &cam_config.hdr);
    }

    format = camera_video_port->format;

    format->encoding = MMAL_ENCODING_I420;
    format->encoding_variant = MMAL_ENCODING_I420;

    format->es->video.width = userdata.video_width;
    format->es->video.height = userdata.video_width;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = userdata.video_width;
    format->es->video.crop.height = userdata.video_height;
    format->es->video.frame_rate.num = 30;
    format->es->video.frame_rate.den = 1;

    camera_video_port->buffer_size = userdata.preview_width * userdata.preview_height * 12 / 8;
    camera_video_port->buffer_num = 1;
    printf("  Camera video buffer_size = %d\n", camera_video_port->buffer_size);

    status = mmal_port_format_commit(camera_video_port);

    if (status != MMAL_SUCCESS) {
        printf("Error: unable to commit camera video port format (%u)\n", status);
        return -1;
    }

    format = camera_preview_port->format;

    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;

    format->es->video.width = userdata.preview_width;
    format->es->video.height = userdata.preview_height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = userdata.preview_width;
    format->es->video.crop.height = userdata.preview_height;

    status = mmal_port_format_commit(camera_preview_port);

    if (status != MMAL_SUCCESS) {
        printf("Error: camera viewfinder format couldn't be set\n");
        return -1;
    }

    // crate pool form camera video port
    camera_video_port_pool = (MMAL_POOL_T *) mmal_port_pool_create(camera_video_port, camera_video_port->buffer_num, camera_video_port->buffer_size);
    userdata.camera_video_port_pool = camera_video_port_pool;
    camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *) &userdata;

    status = mmal_port_enable(camera_video_port, video_buffer_callback);
    if (status != MMAL_SUCCESS) {
        printf("Error: unable to enable camera video port (%u)\n", status);
        return -1;
    }



    status = mmal_component_enable(camera);

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &preview);
    if (status != MMAL_SUCCESS) {
        printf("Error: unable to create preview (%u)\n", status);
        return -1;
    }
    preview_input_port = preview->input[0];

    {
        MMAL_DISPLAYREGION_T param;
        param.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
        param.hdr.size = sizeof (MMAL_DISPLAYREGION_T);
        param.set = MMAL_DISPLAY_SET_LAYER;
        param.layer = 0;
        param.set |= MMAL_DISPLAY_SET_FULLSCREEN;
        param.fullscreen = 1;
        status = mmal_port_parameter_set(preview_input_port, &param.hdr);
        if (status != MMAL_SUCCESS && status != MMAL_ENOSYS) {
            printf("Error: unable to set preview port parameters (%u)\n", status);
            return -1;
        }
    }

    status = mmal_connection_create(&camera_preview_connection, camera_preview_port, preview_input_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);
    if (status != MMAL_SUCCESS) {
        printf("Error: unable to create connection (%u)\n", status);
        return -1;
    }

    status = mmal_connection_enable(camera_preview_connection);
    if (status != MMAL_SUCCESS) {
        printf("Error: unable to enable connection (%u)\n", status);
        return -1;
    }

    if (1) {
        // Send all the buffers to the camera video port
        int num = mmal_queue_length(camera_video_port_pool->queue);
        int q;

        for (q = 0; q < num; q++) {
            MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(camera_video_port_pool->queue);

            if (!buffer) {
                printf("Unable to get a required buffer %d from pool queue\n", q);
            }

            if (mmal_port_send_buffer(camera_video_port, buffer) != MMAL_SUCCESS) {
                printf("Unable to send a buffer to encoder output port (%d)\n", q);
            }
        }


    }

    if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
        printf("%s: Failed to start capture\n", __func__);
    }

    vcos_semaphore_create(&userdata.complete_semaphore, "mmal_opencv_demo-sem", 0);
    int opencv_frames = 0;
    struct timespec t1;
    struct timespec t2;
    clock_gettime(CLOCK_MONOTONIC, &t1);

    GRAPHICS_RESOURCE_HANDLE img_overlay;
    GRAPHICS_RESOURCE_HANDLE img_overlay2;

    gx_graphics_init("/opt/vc/src/hello_pi/hello_font");

    gx_create_window(0, userdata.opencv_width, userdata.opencv_height, GRAPHICS_RESOURCE_RGBA32, &img_overlay);
    gx_create_window(0, 500, 200, GRAPHICS_RESOURCE_RGBA32, &img_overlay2);
    graphics_resource_fill(img_overlay, 0, 0, GRAPHICS_RESOURCE_WIDTH, GRAPHICS_RESOURCE_HEIGHT, GRAPHICS_RGBA32(0xff, 0, 0, 0x55));
    graphics_resource_fill(img_overlay2, 0, 0, GRAPHICS_RESOURCE_WIDTH, GRAPHICS_RESOURCE_HEIGHT, GRAPHICS_RGBA32(0xff, 0, 0, 0x55));

    graphics_display_resource(img_overlay, 0, 1, 0, 0, display_width, display_height, VC_DISPMAN_ROT0, 1);
    char text[256];
    /* *****SAM***** */
    /* system flags and control variables */
    int slc_flag = 0; // 1 == True, 0 == false
    int l_turn, r_turn; // used
    int padding_flag = 0;// 1 == True, 0 == flase
    int padding_x = 0; // used
    int padding_y = 0; // used
    int padding_w = 0; // used
    int padding_h = 0; // used
    int avg_x = 0;
    int avg_y = 0;
    int avg_w = 0;
    int avg_h = 0;
    int avg_iteration = 0;
    int avg_max = 20;
    int draw_flag = 0;
    int out_of_bound = 0; // used
    int face_flag = 0; // used
    int recalibrate = 0 ; // not used
    int reset_timer = 0; // used
    int eyes_detected = 0;
    clock_t alarm_begin = 0 , alarm_end = 0;
    clock_t cal_begin = 0, cal_end = 0;
    clock_t eye_begin = 0, eye_end = 0;
    CvMemStorage* eyes_storage = cvCreateMemStorage(0);
    CvSeq* eyes_objects;
    //CvRect* eyes_box;
    CvRect* r_eye;
    IplImage* face_img;
    IplImage* eye_img;
    IplImage* eye_img_resized;
    /* ********************************* */
    while(1)
    {
    	if(vcos_semaphore_wait(&(userdata.complete_semaphore)) == VCOS_SUCCESS)
	{
        	opencv_frames++;
            	float fps = 0.0;
                clock_gettime(CLOCK_MONOTONIC, &t2);
                float d = (t2.tv_sec + t2.tv_nsec / 1000000000.0) - (t1.tv_sec + t1.tv_nsec / 1000000000.0);
                if (d > 0)
		{
                	fps = opencv_frames / d;
               	}
		else
		{
                	fps = opencv_frames;
               	}
            	graphics_resource_fill(img_overlay, 0, 0, GRAPHICS_RESOURCE_WIDTH, GRAPHICS_RESOURCE_HEIGHT, GRAPHICS_RGBA32(0, 0, 0, 0x00));
            	graphics_resource_fill(img_overlay2, 0, 0, GRAPHICS_RESOURCE_WIDTH, GRAPHICS_RESOURCE_HEIGHT, GRAPHICS_RGBA32(0, 0, 0, 0x00));
               	cvResize(userdata.image, userdata.image2, CV_INTER_LINEAR);
		cvEqualizeHist(userdata.image2, userdata.image2);
                CvSeq* objects = cvHaarDetectObjects(userdata.image2, userdata.cascade, userdata.storage, 1.4, 3, 0, cvSize(100, 100), cvSize(150, 150));
                CvRect* r;
	       	/* input checkpoint (silance and turn signal) */
		int last_slc = LOW;
	       	int current_slc = digitalRead(SLC_BUTTON);
	       	if(current_slc == HIGH && last_slc == LOW)
	       	{
			slc_flag = !slc_flag;
			padding_flag = !padding_flag;
			last_slc = HIGH;
		}
		else
		{
			last_slc = digitalRead(SLC_BUTTON);
		}
		l_turn = digitalRead(R_TURN);
		r_turn = digitalRead(L_TURN);
		printf("R:%d L:%d\n", r_turn, l_turn);
		/* **** */
		face_flag = (objects->total > 0);
              	if(face_flag)
               	{
			r = (CvRect*) cvGetSeqElem(objects, 0);
			/* Calibration check *//*
         		int last_pad = LOW;
			int current_pad = digitalRead(BUTTON);
			if(current_pad == HIGH && last_pad == LOW)
			{
				padding_flag = !padding_flag;
				last_pad = HIGH;
			}
			else
			{
				last_pad = digitalRead(INPUT);
			}
			/* to be removed */
			/* recalibration stage */
			/* avg */
			if(!draw_flag)
			{
				if(avg_iteration < avg_max)
				{
					avg_x += r->x;
					avg_y += r->y;
					avg_w += r->width;
					avg_h += r->height;
					avg_iteration++;
				}
				else
				{
					padding_x = (int)(((avg_x) - (0.075*avg_w))/avg_max);
					padding_y = (int)(((avg_y) - (0.015*avg_h))/avg_max);
					padding_w = (int)(1.3*avg_w/avg_max);
					padding_h = (int)(1.25*avg_h/avg_max);
					draw_flag = 1;
					cal_begin = clock();
					eye_begin = clock();
				}
			}
			/* *** */
			if(draw_flag)
			{
                		if(padding_flag)
                  		{
                			printf("five seconds\n");
					padding_w = (int)((r->width)*1.30);
                   			padding_h = (int)((r->height)*1.25);
                   			padding_y = (int)((r->y) - (padding_h)*0.05);
                   			padding_x = (int)((r->x) - (padding_w)*0.075);
                   			padding_flag = 0;
					cal_begin = clock();
                  		}
				/* ******************* */
                		graphics_resource_fill(img_overlay, padding_x, padding_y, padding_w, padding_h, GRAPHICS_RGBA32(0xff, 0, 0, 0x88));
                		graphics_resource_fill(img_overlay, padding_x+1, padding_y+1, padding_w-2 ,padding_h-2 , GRAPHICS_RGBA32(0, 0, 0, 0x00));
               			/* Collision Detection Stage*/
				out_of_bound = (r->x < padding_x)
					    || ((r->x+r->width) > (padding_x + padding_w))
					    || (r->y < padding_y)
				   	    || ((r->y + r->height) > (padding_y + padding_h));
				//if(l_turn || r_turn)
					//out_of_bound = out_of_bound && !(r->x < padding_x);
				//if(r_turn)
					//out_of_bound = 0;//out_of_bound && !((r->x + r->width) > (padding_x + padding_w));
				/* *********************** */
			}
			graphics_resource_fill(img_overlay, r->x, r->y, r->width, r->height, GRAPHICS_RGBA32(0xff, 0, 0, 0x88));
	               	graphics_resource_fill(img_overlay, r->x + 1, r->y + 1, r->width - 2, r->height - 2, GRAPHICS_RGBA32(0, 0, 0, 0x00));
               	}
		/* eye detection stage */
		eye_end = ((clock() - eye_begin)/CLOCKS_PER_SEC);
		printf("eyes timer: %d\n", eye_end);
		if(face_flag || out_of_bound)
		{
			eye_begin = clock();
			face_img = cvCreateImage(cvSize(r->width, r->height), userdata.image2->depth, userdata.image2->nChannels);
			cvSetImageROI(userdata.image2, cvRect(r->x, r->y,r->width, r->height));
			cvSetImageCOI(userdata.image2, 0);
			cvCopy(userdata.image2, face_img, NULL);
			cvResetImageROI(userdata.image2);
			cvEqualizeHist(face_img, face_img);
			eyes_objects = cvHaarDetectObjects(face_img, eyes_cascade, eyes_storage, 1.1, 2, CV_HAAR_FIND_BIGGEST_OBJECT|CV_HAAR_SCALE_IMAGE, cvSize(20,20), cvSize(50, 50));
			eyes_detected = (eyes_objects->total > 0);
			printf("eyes:%d\n ", eyes_objects->total);
			//if(eye_detected)
			//{
				//r_eye = (CvRect*)cvGetSeqElem(eyes_objects, 1);
				//int i;
				//for(i=0; i<= eyes_objects->total; i++)
				//{
					//CvRect* r_eye = (CvRect*)cvGetSeqElem(eyes_objects, 0);
					//CvPoint p1;
					//p1.x = (r->x + r_eye->x + r_eye->width * 0.5);
					//p1.y = (r->y + r_eye->y + r_eye->height * 0.5);

					//CvRect* l_eye = (CvRect*)cvGetSeqElem(eyes_objects, 1);
					//CvPoint p2;
					//p2.x = (r->x + l_eye->x + l_eye->width * 0.5);
					//p2.y = (r->y + l_eye->y + l_eye->height * 0.5);

					//int radius_1 = cvRound((r_eye->width + r_eye->height) * 0.25);
					//int radius_2 = cvRound((l_eye->width + l_eye->height) * 0.25);

					//cvCircle(userdata.image2, p1, radius_1, cvScalar(255,0,0,0), 1, 8, 0);
					//cvCircle(userdata.image2, p2, radius_2, cvScalar(255,0,0,0), 1, 8, 0);
					//cvSaveImage("eyecap.jpg" , userdata.image2, 0);
					//graphics_resource_fill(img_overlay,(r->x+ r_eye->x),(r->y+ r_eye->y), r_eye->width, r_eye->height,GRAPHICS_RGBA32(0xff, 0, 0, 0x88));
					//CvRect* l_eye = (CvRect*)cvGetSeqElem(eyes_objects, 1);
					//graphics_resource_fill(img_overlay,(r->x+ l_eye->x),(r->y+ l_eye->y), l_eye->width, l_eye->height,GRAPHICS_RGBA32(0xff, 0, 0, 0x88));
					//graphics_resource_fill(img_overlay, r->x + r_eye->x , r->y + r_eye->y , r_eye->width, r_eye->height,GRAPHICS_RGBA32(0, 0, 0, 0x88));
					//graphics_resource_fill(img_overlay, r->x + r_eye->x + 1, r->y + r_eye->y + 1, r_eye->width - 2, r_eye->height - 2,GRAPHICS_RGBA32(0, 0, 0, 0x00));
					//graphics_resource_fill(img_overlay, r->x + l_eye->x , r->y + l_eye->y , l_eye->width, l_eye->height,GRAPHICS_RGBA32(0, 0, 0, 0x88));
					//graphics_resource_fill(img_overlay, r->x + l_eye->x + 1, r->y + l_eye->y + 1, l_eye->width - 2, l_eye->height - 2,GRAPHICS_RGBA32(0, 0, 0, 0x00));
					//graphics_resource_fill(img_overlay,(r->x+ r_eye->x) + (0.5*r->width),(r->y+ r_eye->y), r_eye->width, r_eye->height,GRAPHICS_RGBA32(0xff, 0, 0, 0x88));
					//graphics_resource_fill(img_overlay, r->x + r_eye->x + 1 + (0.5*r->width), r->x + r_eye->y + 1, r_eye->width - 2, r_eye->height - 2,GRAPHICS_RGBA32(0, 0, 0, 0x00));
				//}
			//}
		}
		/* ******** */
  		/* face LED status */
		digitalWrite(FACE, face_flag);
		/* ***** */
		/* auto recalibrate */
		cal_end = ((clock() - cal_begin)/CLOCKS_PER_SEC);
		if(cal_end > 5)//) && !out_of_bound)
			padding_flag = 1; // recal time to be decided
	    	/* Alert stage */
		if(!(l_turn || r_turn)&&(!face_flag || (out_of_bound&&!eyes_detected)))
		{
		        //cvReleaseVideoWriter(&record);
			//return 1;
 			if(!reset_timer)
			{
				alarm_begin = clock();
				reset_timer = 1;
			}
			alarm_end = ((clock()- alarm_begin)/CLOCKS_PER_SEC);
			printf("time lapsed: %d\n", alarm_end);
			if(alarm_end > 0)
				digitalWrite(BUZZ, !slc_flag);//(!slc_flag && out_of_bound));
		}
		else
		{
			reset_timer = 0;
			digitalWrite(BUZZ, LOW);
		}
		/***************/
            	sprintf(text, "Video = %.2f FPS, OpenCV = %.2f FPS", userdata.video_fps, fps);
            	graphics_resource_render_text_ext(img_overlay2, 0, 0,
                	GRAPHICS_RESOURCE_WIDTH,
                    	GRAPHICS_RESOURCE_HEIGHT,
               		GRAPHICS_RGBA32(0x00, 0xff, 0x00, 0xff), /* fg */
               		GRAPHICS_RGBA32(0, 0, 0, 0x00), /* bg */
               		text, strlen(text), 25);
            	graphics_display_resource(img_overlay, 0, 1, 0, 0, display_width, display_height, VC_DISPMAN_ROT0, 1);
            	graphics_display_resource(img_overlay2, 0, 2, 0, display_width / 16, GRAPHICS_RESOURCE_WIDTH, GRAPHICS_RESOURCE_HEIGHT, VC_DISPMAN_ROT0, 1);

        } // if
    } // while
  // cvReleaseVideoWriter(&record);
    return 0;
}

