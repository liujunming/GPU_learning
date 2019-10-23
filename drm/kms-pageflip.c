/**
FLAGS=`pkg-config --cflags --libs libdrm libkms`
FLAGS+=-Wall -O0 -g
FLAGS+=-D_FILE_OFFSET_BITS=64
FLAGS+=-lcairo

all:
        gcc -o kms-pageflip kms-pageflip.c $(FLAGS)

**/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libkms/libkms.h>
#include <cairo/cairo.h>

struct flip_context{
	int fb_id[2];
	int current_fb_id;
	int crtc_id;
	struct timeval start;
	int swap_count;
};

#define max(a, b) ((a) > (b) ? (a) : (b))

typedef void (*draw_func_t)(char *addr, int w, int h, int pitch);

void draw_buffer(char *addr, int w, int h, int pitch)
{
	int ret, i, j;

	/* paint the buffer with colored tiles */
	for (j = 0; j < h; j++) {
		uint32_t *fb_ptr = (uint32_t*)((char*)addr + j * pitch);
		for (i = 0; i < w; i++) {
			div_t d = div(i, w);
			fb_ptr[i] =
				0x00130502 * (d.quot >> 6) +
				0x000a1120 * (d.rem >> 6);
		}
	}
}

void draw_buffer_with_cairo(char *addr, int w, int h, int pitch)
{
	cairo_t *cr;
	cairo_surface_t *surface;

	surface = cairo_image_surface_create_for_data(
		addr,
		CAIRO_FORMAT_ARGB32,
        w, h, pitch);
    cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	/* Use normalized coordinates hereinafter */
	cairo_scale (cr, w, h);

	/* rectangle stroke */
	cairo_set_source_rgb (cr, 1, 1, 1);
	cairo_set_line_width (cr, 0.05);
	cairo_rectangle (cr, 0.1, 0.1, 0.3, 0.4);
	cairo_stroke (cr);

	/* circle fill */
	cairo_set_source_rgba(cr, 1, 0, 0, 0.5);
	cairo_arc(cr, 0.7, 0.3, 0.2, 0, 2 * M_PI);
	cairo_fill(cr);

	/* text */
	cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
	cairo_select_font_face (cr, "Georgia",
    	CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size (cr, 0.1);
	cairo_move_to (cr, 0.1, 0.8);
	cairo_show_text (cr, "drawn with cairo");
	
	cairo_destroy(cr);
}

void create_bo(struct kms_driver *kms_driver, 
	int w, int h, int *out_pitch, struct kms_bo **out_kms_bo, 
	int *out_handle, draw_func_t draw)
{
	void *map_buf;
	struct kms_bo *bo;
	int pitch, handle;
	unsigned bo_attribs[] = {
		KMS_WIDTH,   w,
		KMS_HEIGHT,  h,
		KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
		KMS_TERMINATE_PROP_LIST
	};
	int ret;

	/* ceate kms buffer object, opaque struct identied by struct kms_bo pointer */
	ret = kms_bo_create(kms_driver, bo_attribs, &bo);
	if(ret){
		fprintf(stderr, "kms_bo_create failed: %s\n", strerror(errno));
		goto exit;
	}

	/* get the "pitch" or "stride" of the bo */
	ret = kms_bo_get_prop(bo, KMS_PITCH, &pitch);
	if(ret){
		fprintf(stderr, "kms_bo_get_prop KMS_PITCH failed: %s\n", strerror(errno));
		goto free_bo;
	}

	/* get the handle of the bo */
	ret = kms_bo_get_prop(bo, KMS_HANDLE, &handle);
	if(ret){
		fprintf(stderr, "kms_bo_get_prop KMS_HANDL failed: %s\n", strerror(errno));
		goto free_bo;
	}

	/* map the bo to user space buffer */
	ret = kms_bo_map(bo, &map_buf);
	if(ret){
		fprintf(stderr, "kms_bo_map failed: %s\n", strerror(errno));
		goto free_bo;
	}

	draw(map_buf, w, h, pitch);

	kms_bo_unmap(bo);

	ret = 0;
	*out_kms_bo = bo;
	*out_pitch = pitch;
	*out_handle = handle;
	goto exit;

free_bo:
	kms_bo_destroy(&bo);
	
exit:
	return;

}

void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	struct flip_context *context;
	unsigned int new_fb_id;
	struct timeval end;
	double t;

	context = data;
	if (context->current_fb_id == context->fb_id[0])
		new_fb_id = context->fb_id[1];
	else
		new_fb_id = context->fb_id[0];
			
	drmModePageFlip(fd, context->crtc_id, new_fb_id,
			DRM_MODE_PAGE_FLIP_EVENT, context);
	context->current_fb_id = new_fb_id;
	context->swap_count++;
	if (context->swap_count == 60) {
		gettimeofday(&end, NULL);
		t = end.tv_sec + end.tv_usec * 1e-6 -
			(context->start.tv_sec + context->start.tv_usec * 1e-6);
		fprintf(stderr, "freq: %.02fHz\n", context->swap_count / t);
		context->swap_count = 0;
		context->start = end;
	}
} 

int main(int argc, char *argv[])
{
	int fd, pitch, bo_handle, fb_id, second_fb_id;
	drmModeRes *resources;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeModeInfo mode;
	drmModeCrtcPtr orig_crtc;
	struct kms_driver *kms_driver;
	struct kms_bo *kms_bo, *second_kms_bo;
	void *map_buf;
	int ret, i;
	
#if 0
	fd = drmOpen("i915", NULL);
#else
	fd = open("/dev/dri/card0", O_RDWR);
#endif
	if(fd < 0){
		fprintf(stderr, "drmOpen failed: %s\n", strerror(errno));
		goto out;
	}

	resources = drmModeGetResources(fd);
	if(resources == NULL){
		fprintf(stderr, "drmModeGetResources failed: %s\n", strerror(errno));
		goto close_fd;
	}

	/* find the first available connector with modes */
	for(i=0; i < resources->count_connectors; ++i){
		connector = drmModeGetConnector(fd, resources->connectors[i]);
		if(connector != NULL){
			fprintf(stderr, "connector %d found\n", connector->connector_id);
			if(connector->connection == DRM_MODE_CONNECTED
				&& connector->count_modes > 0)
				break;
			drmModeFreeConnector(connector);
		}
		else
			fprintf(stderr, "get a null connector pointer\n");
	}
	if(i == resources->count_connectors){
		fprintf(stderr, "No active connector found.\n");
		goto free_drm_res;
	}

	mode = connector->modes[0];
	fprintf(stderr, "(%dx%d)\n", mode.hdisplay, mode.vdisplay);

	/* find the encoder matching the first available connector */
	for(i=0; i < resources->count_encoders; ++i){
		encoder = drmModeGetEncoder(fd, resources->encoders[i]);
		if(encoder != NULL){
			fprintf(stderr, "encoder %d found\n", encoder->encoder_id);
			if(encoder->encoder_id == connector->encoder_id);
				break;
			drmModeFreeEncoder(encoder);
		} else
			fprintf(stderr, "get a null encoder pointer\n");
	}
	if(i == resources->count_encoders){
		fprintf(stderr, "No matching encoder with connector, shouldn't happen\n");
		goto free_drm_res;
	}

	/* init kms bo stuff */	
	ret = kms_create(fd, &kms_driver);
	if(ret){
		fprintf(stderr, "kms_create failed: %s\n", strerror(errno));
		goto free_drm_res;
	}

	create_bo(kms_driver, mode.hdisplay, mode.vdisplay, 
		&pitch, &kms_bo, &bo_handle, draw_buffer);

	/* add FB which is associated with bo */
	ret = drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32, pitch, bo_handle, &fb_id);
	if(ret){
		fprintf(stderr, "drmModeAddFB failed (%ux%u): %s\n",
			mode.hdisplay, mode.vdisplay, strerror(errno));
		goto free_first_bo;
	}

	orig_crtc = drmModeGetCrtc(fd, encoder->crtc_id);
	if (orig_crtc == NULL)
	  goto free_first_bo;

	/* kernel mode setting, wow! */
	ret = drmModeSetCrtc(
				fd, encoder->crtc_id, fb_id, 
				0, 0, 	/* x, y */ 
				&connector->connector_id, 
				1, 		/* element count of the connectors array above*/
				&mode);
	if(ret){
		fprintf(stderr, "drmModeSetCrtc failed: %s\n", strerror(errno));
		goto free_first_fb;
	}

	create_bo(kms_driver, mode.hdisplay, mode.vdisplay, 
		&pitch, &second_kms_bo, &bo_handle, draw_buffer_with_cairo);

	/* add another FB which is associated with bo */
	ret = drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32, pitch, bo_handle, &second_fb_id);
	if(ret){
		fprintf(stderr, "drmModeAddFB failed (%ux%u): %s\n",
			mode.hdisplay, mode.vdisplay, strerror(errno));
		goto free_second_bo;
	}
	
	struct flip_context flip_context;
	memset(&flip_context, 0, sizeof flip_context);

	ret = drmModePageFlip(
		fd, encoder->crtc_id, second_fb_id,
		DRM_MODE_PAGE_FLIP_EVENT, &flip_context);
	if (ret) {
		fprintf(stderr, "failed to page flip: %s\n", strerror(errno));
		goto free_second_fb;
	}

	flip_context.fb_id[0] = fb_id;
	flip_context.fb_id[1] = second_fb_id;
	flip_context.current_fb_id = second_fb_id;
	flip_context.crtc_id = encoder->crtc_id;
	flip_context.swap_count = 0;
	gettimeofday(&flip_context.start, NULL);

	/* disable stdin buffered i/o and local echo */
	struct termios old_tio, new_tio;
	tcgetattr(STDIN_FILENO,&old_tio);
	new_tio = old_tio;
	new_tio.c_lflag &= (~ICANON & ~ECHO);
	tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);

	drmEventContext evctx;
	memset(&evctx, 0, sizeof evctx);
	evctx.version = DRM_EVENT_CONTEXT_VERSION;
	evctx.vblank_handler = NULL;
	evctx.page_flip_handler = page_flip_handler;

	while(1){
		struct timeval timeout = { 
			.tv_sec = 3, 
			.tv_usec = 0 
		};
		fd_set fds;

		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		FD_SET(fd, &fds);
		ret = select(max(STDIN_FILENO, fd) + 1, &fds, NULL, NULL, &timeout);

		if (ret <= 0) {
			continue;
		} else if (FD_ISSET(STDIN_FILENO, &fds)) {
			char c = getchar();
			if(c == 'q' || c == 27)
				break;
		} else {
			/* drm device fd data ready */
			ret = drmHandleEvent(fd, &evctx);
			if (ret != 0) {
				fprintf(stderr, "drmHandleEvent failed: %s\n", strerror(errno));
				break;
			}
		}
	}

	ret = drmModeSetCrtc(fd, orig_crtc->crtc_id, orig_crtc->buffer_id,
					orig_crtc->x, orig_crtc->y,
					&connector->connector_id, 1, &orig_crtc->mode);
	if (ret) {
		fprintf(stderr, "drmModeSetCrtc() restore original crtc failed: %m\n");
	}

	/* restore the old terminal settings */
	tcsetattr(STDIN_FILENO,TCSANOW,&old_tio);


free_second_fb:
	drmModeRmFB(fd, second_fb_id);
	
free_second_bo:
	kms_bo_destroy(&second_kms_bo);
	
free_first_fb:
	drmModeRmFB(fd, fb_id);
	
free_first_bo:
	kms_bo_destroy(&kms_bo);

free_kms_driver:
	kms_destroy(&kms_driver);
	
free_drm_res:
	drmModeFreeResources(resources);

close_fd:
	drmClose(fd);
	
out:
	return EXIT_SUCCESS;
}
