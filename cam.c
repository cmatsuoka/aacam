/* A quick and dirty aalib v4l client
 *
 * changelog (acme: I love changelogs)
 *
 * 2000/07/10 - claudio@conectiva.com - added thread support for improved
 *		asynchronous operation with interactive commands. Oh, and
 *		added interactive commands as well!
 *
 * 2000/07/05 - acme@conectiva.com.br - check if the open on the video device
 * 		failed, moved atexit call to after we successfully initialize
 *		aalib.
 *
 * 2000/07/05 - claudio@conectiva.com - now you can even choose the video
 *		device to use! you're not stuck at /dev/video1 anymore.
 */


#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/videodev.h>
#include <pthread.h>
#include <getopt.h>
#include <aalib.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define XSIZ aa_imgwidth(context)
#define YSIZ (aa_imgheight(context)-2*context->muly)
#define YMAX (aa_imgheight(context)/context->muly-1)

static int fd;				/* video device descriptor */
static unsigned char *fb;		/* mmap'd framebuffer */
static int rgb = 0;			/* use RGB instead of YUV */
static int running;
static struct video_mmap vid_mmap;
static struct video_capability capability;
static aa_context *context;
static aa_renderparams *params;
static pthread_t grab_thread;
static pthread_mutex_t grab_mutex = PTHREAD_MUTEX_INITIALIZER;
static double fps;
static char *videodev = "/dev/video";


static int get_time ()
{
	struct timeval tv;
	struct timezone tz;

	gettimeofday (&tv, &tz);

	return 1000 * tv.tv_sec + tv.tv_usec / 1000;
}


static void uninitialize ()
{
	close (fd);
	pthread_join (grab_thread, NULL);
	aa_uninitkbd (context);
	aa_showcursor(context);
	aa_close (context);
}


static void *grab ()
{
	int i, j;
	int t0, t1;

	while (running) {
		t0 = get_time ();
		ioctl (fd, VIDIOCSYNC, &vid_mmap);
		if (ioctl(fd, VIDIOCMCAPTURE, &vid_mmap) == -1) {
			perror ("Fatal");
			exit (-1);
		}
		for (j = 0; j < YSIZ; j++) {
			for (i = 0; i < XSIZ; i++) {
				int r = j * 240 / YSIZ;
				int c = i * 320 / XSIZ;
				unsigned char p;
				p = rgb ?
					(820 * fb[(r * 320 + c) * 3] +
					6094 * fb[(r * 320 + c) * 3 + 1] +
					3086 * fb[(r * 320 + c) * 3 + 2]) / 10000
					: fb[r * 320 + c];
				context->imagebuffer[j * XSIZ + i] = p;
			}
		}

		pthread_mutex_lock (&grab_mutex);
		aa_render (context, params, 0, 0,
			aa_scrwidth(context), aa_scrheight(context));
		aa_flush(context);
		pthread_mutex_unlock (&grab_mutex);

		t1 = get_time ();

		fps = 1000.0 / (t1 - t0);
	}
}


static int init_video ()
{

	fd = open (videodev, O_RDWR);
	if (ioctl(fd,VIDIOCGCAP,&capability) == -1) {
		perror ("Fatal");
		return -1;
	}
	vid_mmap.format = VIDEO_PALETTE_YUV422P;
	vid_mmap.frame = 0;
	vid_mmap.width = 320;
	vid_mmap.height = 240;

	if (ioctl(fd, VIDIOCMCAPTURE, &vid_mmap) == -1) {
		vid_mmap.format = VIDEO_PALETTE_RGB24;
		if (ioctl(fd, VIDIOCMCAPTURE, &vid_mmap) == -1) {
			fprintf (stderr, "YUV422P/RGB24 modes not available. Giving up.\n");
			return -1;
		}
		rgb = 1;
	}

	fb = (unsigned char*)mmap(0, 320*240*(rgb?3:1), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

	return 0;
}


static int init_aalib ()
{
	if (!(context = aa_autoinit(&aa_defparams)))
		return -1;

	aa_autoinitkbd (context, 0);
	params = aa_getrenderparams();
	params->dither = AA_FLOYD_S;
	aa_hidecursor(context);

	return 0;
}


static void render ()
{
	int i, j;
	int key, mode=0;
	int ctrl[3];
	struct video_picture p;

	running = 1;
	if (ioctl (fd, VIDIOCGPICT, &p) < 0)
		return;
	ctrl[0] = p.brightness >> 8;
	ctrl[1] = p.contrast >> 8;
	ctrl[2] = p.whiteness >> 8;

	aa_printf (context, 0, YMAX-1, AA_BOLD, "v4l: %s (%s)",
		capability.name, rgb ? "RGB24" : "YUV422P");

	pthread_create(&grab_thread, NULL, grab, NULL);

	while (42) {
		key = aa_getkey (context, 0);

		pthread_mutex_lock (&grab_mutex);
		aa_printf (context, 0, YMAX, AA_BOLD, "fps: %5.2f", fps);
		aa_printf (context, 15, YMAX, mode==0 ? AA_REVERSE : AA_BOLD, "Bri:%3d", ctrl[0]);
		aa_printf (context, 24, YMAX, mode==1 ? AA_REVERSE : AA_BOLD, "Con:%3d", ctrl[1]);
		aa_printf (context, 33, YMAX, mode==2 ? AA_REVERSE : AA_BOLD, "Whi:%3d", ctrl[2]);
		aa_flush(context);
		pthread_mutex_unlock (&grab_mutex);

#define INC_RANGE(x,y) { if (x < y) x++; }
#define DEC_RANGE(x,y) { if (x > 0) x--; }

		switch (key) {
		case 'q':
			running = 0;
			exit (0);
		case AA_LEFT:
			DEC_RANGE(mode,2);
			break;
		case AA_RIGHT:
			INC_RANGE(mode,2);
			break;
		case AA_UP:
			INC_RANGE(ctrl[mode],0xff);
			break;
		case AA_DOWN:
			DEC_RANGE(ctrl[mode],0xff);
			break;
		}

		if (ctrl[0] != (p.brightness >> 8) ||
			ctrl[1] != (p.contrast >> 8) ||
			ctrl[2] != (p.whiteness >> 8)) {
			pthread_mutex_lock (&grab_mutex);
			p.brightness = ctrl[0] << 8;
			p.contrast = ctrl[1] << 8;
			p.whiteness = ctrl[2] << 8;
			if (ioctl (fd, VIDIOCSPICT, &p) < 0) {
				printf ("Aiee!\n");
				return;
			}
			pthread_mutex_unlock (&grab_mutex);
		}
	}
}


int main (int argc, char **argv)
{
	int o;

	if (!aa_parseoptions(NULL, NULL, &argc, argv) || argc != 1) {
		fprintf (stderr, "%s", aa_help);
		exit (0);
	}

	while ((o = getopt (argc, argv, "d:")) != -1) {
		switch (o) {
		case 'd':
			videodev = optarg;
			break;
		}
	}

	if (init_video () < 0) {
		fprintf (stderr, "Error initializing video\n");
		exit (-1);
	}

	if (init_aalib () < 0) {
		fprintf (stderr, "Error initializing aalib\n");
		exit (-1);
	}

	atexit (uninitialize);
	render ();

	return 0;
}

