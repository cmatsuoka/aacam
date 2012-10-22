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
#include <linux/videodev2.h>
#include <pthread.h>
#include <getopt.h>
#include <aalib.h>
#include <errno.h>

#define XSIZ aa_imgwidth(context)
#define YSIZ (aa_imgheight(context)-2*context->muly)
#define YMAX (aa_imgheight(context)/context->muly-1)

struct my_buffer {
	void *start;
	size_t length;
};

static int fd;				/* video device descriptor */
static int rgb = 0;			/* use RGB instead of YUV */
static int running;
static struct video_mmap vid_mmap;
static struct v4l2_capability capability;
static struct v4l2_cropcap cropcap;
static struct v4l2_crop crop;
static struct v4l2_format format;
static struct v4l2_requestbuffers requestbuffers;
static struct my_buffer *my_buffers;
static aa_context *context;
static aa_renderparams *params;
static pthread_t grab_thread;
static pthread_mutex_t grab_mutex = PTHREAD_MUTEX_INITIALIZER;
static double fps;
static char *videodev = "/dev/video";


static int get_time()
{
	struct timeval tv;
	struct timezone tz;

	gettimeofday (&tv, &tz);

	return 1000 * tv.tv_sec + tv.tv_usec / 1000;
}


static void start_capture()
{
	int i;
	enum v4l2_buf_type buf_type;
	struct v4l2_buffer buffer;

	for (i = 0; i < requestbuffers.count; i++) {
		memset(&buffer, 0, sizeof(struct v4l2_buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("VIDIOC_QBUF");
			return;
		}
	}

	buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMON, &buf_type) < 0) {
		perror("VIDIOC_STREAMON");
		return;
	}
}


static void stop_capture()
{
	enum v4l2_buf_type buf_type;

	buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMOFF, &buf_type) < 0) {
		perror("VIDIOC_STREAMOFF");
	}
}


static void uninitialize()
{
	stop_capture();
	close (fd);
	pthread_join (grab_thread, NULL);
	aa_uninitkbd (context);
	aa_showcursor(context);
	aa_close (context);
}


static void *grab()
{
	int i, j;
	int t0, t1;
	struct v4l2_buffer buffer;
	unsigned char *fb;

	while (running) {
		t0 = get_time ();

		memset(&buffer, 0, sizeof(struct v4l2_buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;

		if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
			if (errno != EAGAIN) {
				perror("VIDIOC_DQBUF");
				return;
			}
		}

		fb = my_buffers[0].start;

		for (j = 0; j < YSIZ; j++) {
			for (i = 0; i < XSIZ; i++) {
				int r = j * 240 / YSIZ;
				int c = i * 320 / XSIZ;
				int val = r * 320 + c;
				unsigned char p;
				p = rgb ? (820 * fb[val * 3] +
					6094 * fb[val * 3 + 1] +
					3086 * fb[val * 3 + 2]) / 10000
					: fb[val];
				context->imagebuffer[j * XSIZ + i] = p;
			}
		}

		pthread_mutex_lock (&grab_mutex);
		aa_render (context, params, 0, 0,
			aa_scrwidth(context), aa_scrheight(context));
		aa_flush(context);
		pthread_mutex_unlock (&grab_mutex);

		if (ioctl(fd, VIDIOC_QBUF, &buffer) < 0) {
			perror("VIDIOC_QBUF");
			return;
		}

		t1 = get_time ();

		fps = 1000.0 / (t1 - t0);
	}
}


static int init_video()
{
	int i;

	if ((fd = open(videodev, O_RDWR)) < 0) {
		perror(videodev);
		goto err;
	}
	printf("using %s\n", videodev);

	if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
		perror("VIDIOC_QUERYCAP");
		goto err1;
	}
	printf("device is %s (%s)\n", capability.card, capability.driver);

	if (capability.capabilities & V4L2_CAP_VIDEO_CAPTURE == 0) {
		fprintf(stderr, "can't capture from device\n");
		goto err1;
	}

	if (capability.capabilities & V4L2_CAP_STREAMING == 0) {
		fprintf(stderr, "can't stream from device\n");
		goto err1;
	}
	printf("can stream from device\n");

	memset(&cropcap, 0, sizeof(struct v4l2_cropcap));
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (ioctl(fd, VIDIOC_CROPCAP, &cropcap) == 0) {
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect;

		ioctl(fd, VIDIOC_S_CROP, &crop);
	}

	memset(&format, 0, sizeof(struct v4l2_format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.field = V4L2_FIELD_ANY;
	format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV422P;
	format.fmt.pix.width = 320;
	format.fmt.pix.height = 240;

	if (ioctl(fd, VIDIOC_G_FMT, &format) < 0) {
		format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
		if (ioctl(fd, VIDIOC_G_FMT, &format) < 0) {
			perror("VIDIOC_G_FMT");
			goto err1;
		}
		rgb = 1;
	}
	printf("format is %s\n", rgb ? "RGB24" : "YUV422");
	
	memset(&requestbuffers, 0, sizeof(struct v4l2_requestbuffers));
	requestbuffers.count = 1;
	requestbuffers.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	requestbuffers.memory = V4L2_MEMORY_MMAP;

	if (ioctl(fd, VIDIOC_REQBUFS, &requestbuffers) < 0) {
		perror("VIDIOC_REQBUFS");
		goto err1;
	}
	printf("%d buffers\n", requestbuffers.count);

	my_buffers = calloc(requestbuffers.count, sizeof(struct my_buffer));
	if (my_buffers == NULL) {
		fprintf(stderr, "can't alloc buffers\n");
		goto err1;
	}

	for (i = 0; i < requestbuffers.count; i++) {
		struct v4l2_buffer buffer;

		memset(&buffer, 0, sizeof(struct v4l2_buffer));
		buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buffer.memory = V4L2_MEMORY_MMAP;
		buffer.index = i;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buffer) < 0) {
			perror("VIDIOC_QUERYBUF");
			goto err2;
		}

		my_buffers[i].length = buffer.length;
		my_buffers[i].start = mmap(NULL, buffer.length, PROT_READ |
				PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
		if (my_buffers[i].start == MAP_FAILED) {
			perror("mmap");
			goto err2;
		}

		printf("mmap buffer %d\n", i);
	}

	return 0;

err2:
	free(my_buffers);
err1:
	close(fd);
err:
	return -1;
}


static int init_aalib()
{
	if (!(context = aa_autoinit(&aa_defparams)))
		return -1;

	aa_autoinitkbd (context, 0);
	params = aa_getrenderparams();
	params->dither = AA_FLOYD_S;
	aa_hidecursor(context);

	return 0;
}


static void render()
{
	int i, j;
	int key, mode=0;
	int ctrl[3];
	struct v4l2_queryctrl queryctrl;
	struct v4l2_control control;
	int brightness, contrast, whiteness;

	start_capture();

	running = 1;

	memset(&queryctrl, 0, sizeof (queryctrl));

	queryctrl.id = V4L2_CID_BRIGHTNESS;
	ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl);
	brightness = ctrl[0] = queryctrl.default_value >> 8;

	queryctrl.id = V4L2_CID_CONTRAST;
	ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl);
	contrast = ctrl[1] = queryctrl.default_value >> 8;

	queryctrl.id = V4L2_CID_WHITENESS;
	ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl);
	whiteness = ctrl[2] = queryctrl.default_value >> 8;

	aa_printf(context, 0, YMAX-1, AA_BOLD, "v4l: %s (%s)",
		capability.card, rgb ? "RGB24" : "YUV422P");

	pthread_create(&grab_thread, NULL, grab, NULL);

#define ATTR(x,y) ((x)==(y) ? AA_REVERSE : AA_BOLD)

	while (42) {
		key = aa_getkey (context, 0);

		pthread_mutex_lock(&grab_mutex);
		aa_printf(context, 0, YMAX, AA_BOLD, "fps: %5.2f", fps);
		aa_printf(context, 15, YMAX, ATTR(mode, 0), "Bri:%3d", ctrl[0]);
		aa_printf(context, 24, YMAX, ATTR(mode, 1), "Con:%3d", ctrl[1]);
		aa_printf(context, 33, YMAX, ATTR(mode, 2), "Whi:%3d", ctrl[2]);
		aa_flush(context);
		pthread_mutex_unlock(&grab_mutex);

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

		if (ctrl[0] != brightness || ctrl[1] != contrast ||
						ctrl[2] != whiteness) {
			pthread_mutex_lock (&grab_mutex);

			memset (&control, 0, sizeof (control));

			if (ctrl[0] != brightness) {
				control.id = V4L2_CID_BRIGHTNESS;	
				control.value = ctrl[0] << 8;
				ioctl(fd, VIDIOC_S_CTRL, &control);
			}

			if (ctrl[1] != contrast) {
				control.id = V4L2_CID_CONTRAST;	
				control.value = ctrl[1] << 8;
				ioctl(fd, VIDIOC_S_CTRL, &control);
			}

			if (ctrl[2] != whiteness) {
				control.id = V4L2_CID_WHITENESS;	
				control.value = ctrl[2] << 8;
				ioctl(fd, VIDIOC_S_CTRL, &control);
			}

			pthread_mutex_unlock (&grab_mutex);
		}
	}
}


int main(int argc, char **argv)
{
	int o;

	if (!aa_parseoptions(NULL, NULL, &argc, argv) || argc != 1) {
		while ((o = getopt (argc, argv, "d:h")) != -1) {
			switch (o) {
			case 'd':
				videodev = optarg;
				break;
			case 'h':
			default:
				fprintf(stderr, "  -d <videodev>  select video device "
				"(default is %s)\n\n%s", videodev, aa_help);
			exit(0);
			}
		}
	}

	if (init_video() < 0) {
		fprintf (stderr, "Error initializing video\n");
		exit(-1);
	}

	if (init_aalib() < 0) {
		fprintf (stderr, "Error initializing aalib\n");
		exit(-1);
	}

	atexit(uninitialize);
	render();

	return 0;
}


