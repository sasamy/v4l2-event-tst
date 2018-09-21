/*
 * Copyright 2018 www.starterkit.ru <info@starterkit.ru>
 *
 * Based on:
 * Dynamic pipelines example, uridecodebin with sinks added and removed
 * Copyright (c) 2014 Sebastian Dröge <sebastian@centricular.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/poll.h>

#include <gst/gst.h>

static char *videodev;
static int ww, wh, wx, wy;
static int videofd = -1;
static int pipefd[2] = {-1, -1};
static pthread_t threadid;
static unsigned int fidx = 0;

static GMainLoop *loop;
static GstElement *pipeline;
static GstElement *src, *tee, *queue, *sink;
static gboolean motion = FALSE;
static gboolean record = FALSE;
static GList *sinks;

typedef struct {
	GstPad *teepad;
	gboolean removing;
	gchar recordfile[200];

	GstElement *queue;
	GstElement *transform;
	GstElement *enc;
	GstElement *parse;
	GstElement *mux;
	GstElement *sink;
} record_sink;

static GstPadProbeReturn unlink_cb(GstPad *pad,
				GstPadProbeInfo *info,
				gpointer user_data)
{
	record_sink *sink = user_data;
	GstPad *queuepad;

	if (!g_atomic_int_compare_and_exchange(&sink->removing, FALSE, TRUE))
		return GST_PAD_PROBE_OK;

	queuepad = gst_element_get_static_pad(sink->queue, "sink");
	gst_pad_unlink(sink->teepad, queuepad);
	gst_object_unref(queuepad);

	gst_bin_remove(GST_BIN(pipeline), sink->queue);
	gst_bin_remove(GST_BIN(pipeline), sink->transform);
	gst_bin_remove(GST_BIN(pipeline), sink->enc);
	gst_bin_remove(GST_BIN(pipeline), sink->parse);
	gst_bin_remove(GST_BIN(pipeline), sink->mux);
	gst_bin_remove(GST_BIN(pipeline), sink->sink);

	gst_element_set_state(sink->queue, GST_STATE_NULL);
	gst_element_set_state(sink->transform, GST_STATE_NULL);
	gst_element_set_state(sink->enc, GST_STATE_NULL);
	gst_element_set_state(sink->parse, GST_STATE_NULL);
	gst_element_set_state(sink->mux, GST_STATE_NULL);
	gst_element_set_state(sink->sink, GST_STATE_NULL);

	gst_object_unref(sink->queue);
	gst_object_unref(sink->transform);
	gst_object_unref(sink->enc);
	gst_object_unref(sink->parse);
	gst_object_unref(sink->mux);
	gst_object_unref(sink->sink);

	gst_element_release_request_pad(tee, sink->teepad);
	gst_object_unref(sink->teepad);

	g_atomic_int_set(&record, FALSE);
	g_print("Stop recording %s\n", sink->recordfile);

	return GST_PAD_PROBE_REMOVE;
}

static gboolean tick_cb(gpointer data)
{
	if (g_atomic_int_compare_and_exchange(&motion, TRUE, FALSE)) {
		return TRUE;
	} else {
		record_sink *sink = sinks->data;
		sinks = g_list_delete_link(sinks, sinks);
		gst_pad_add_probe(sink->teepad, GST_PAD_PROBE_TYPE_IDLE,
			unlink_cb, sink, (GDestroyNotify)g_free);
		return FALSE;
	}
}

static void start_record(void)
{
	record_sink *sink = g_new0(record_sink, 1);
	GstPad *queuepad;

	sinks = g_list_append(sinks, sink);

	sink->teepad = gst_element_get_request_pad(tee, "src_%u");
	sink->removing = FALSE;

	sink->queue = gst_element_factory_make("queue", NULL);
	sink->transform = gst_element_factory_make("imxipuvideotransform", NULL);
	sink->enc = gst_element_factory_make("imxvpuenc_h264", NULL);
	sink->parse = gst_element_factory_make("h264parse", NULL);
	sink->mux = gst_element_factory_make("matroskamux", NULL);
	sink->sink = gst_element_factory_make("filesink", NULL);

	{
		time_t now = time(NULL);
		struct tm *t = localtime(&now);
		char *path = strdup(videodev);
		char buf[100];

		if (t == NULL || strftime(buf, sizeof(buf), "%d-%m-%y_%H:%M:%S", t) == 0)
			snprintf(buf, sizeof(buf), "record_%d", fidx++);

		snprintf(sink->recordfile, sizeof(sink->recordfile), "%s_%s.mkv",
			basename(path), buf);
		free(path);
	}
	g_object_set(sink->sink, "location", sink->recordfile, NULL);
	g_object_set(sink->enc, "bitrate", 2000, NULL);

	gst_bin_add_many(GST_BIN(pipeline),
		gst_object_ref(sink->queue),
		gst_object_ref(sink->transform),
		gst_object_ref(sink->enc),
		gst_object_ref(sink->parse),
		gst_object_ref(sink->mux),
		gst_object_ref(sink->sink), NULL);

	gst_element_link_many(
		sink->queue,
		sink->transform,
		sink->enc,
		sink->parse,
		sink->mux,
		sink->sink, NULL);

	gst_element_sync_state_with_parent(sink->queue);
	gst_element_sync_state_with_parent(sink->transform);
	gst_element_sync_state_with_parent(sink->enc);
	gst_element_sync_state_with_parent(sink->parse);
	gst_element_sync_state_with_parent(sink->mux);
	gst_element_sync_state_with_parent(sink->sink);

	queuepad = gst_element_get_static_pad(sink->queue, "sink");
	gst_pad_link(sink->teepad, queuepad);
	gst_object_unref(queuepad);

	g_atomic_int_set(&motion, FALSE);
	g_timeout_add_seconds(3, tick_cb, NULL);
	g_print("Start recording %s\n", sink->recordfile);
}

static void restart_pipeline(void)
{
	g_print("%s: pipeline stoppped\n", videodev);
	gst_element_set_state(pipeline, GST_STATE_NULL);
	if (gst_element_get_state(pipeline, NULL, NULL, -1) == GST_STATE_CHANGE_FAILURE)
		g_error("Failed to go into NULL state");

	g_print("%s: pipeline running\n", videodev);
	gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (gst_element_get_state(pipeline, NULL, NULL, -1) == GST_STATE_CHANGE_FAILURE)
		g_error("Failed to go into PLAYING state");
}

static void * event_thread_func(void *arg)
{
	struct pollfd fds[2];

	fds[0].fd = pipefd[0];
	fds[0].events = POLLIN;

	fds[1].fd = videofd;
	fds[1].events = POLLPRI;

	while (1) {
		if (poll(fds, 2, -1) < 0) {
			g_print("%s: poll failed: %s\n", videodev, g_strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN) {
			g_print("%s: quit message received\n", videodev);
			break;
		}

		if (fds[1].revents & POLLPRI) {
			struct v4l2_event ev = {0};

			if (ioctl(fds[1].fd, VIDIOC_DQEVENT, &ev) == 0) {
				switch (ev.type) {
				case V4L2_EVENT_SOURCE_CHANGE:
					g_print("%s: source change event\n", videodev);
					restart_pipeline();
					break;
				case V4L2_EVENT_MOTION_DET:
					g_print("%s: motion detection event seq=%u\n", videodev,
						ev.u.motion_det.flags == V4L2_EVENT_MD_FL_HAVE_FRAME_SEQ ?
							ev.u.motion_det.frame_sequence : 0);
					if (g_atomic_int_compare_and_exchange(&record, FALSE, TRUE))
						start_record();
					else
						g_atomic_int_set(&motion, TRUE);
					break;
				default:
					g_print("%s: unknown event\n", videodev);
					break;
				}
			} else {
				g_print("%s: VIDIOC_DQEVENT failed: %s\n",
					videodev, g_strerror(errno));
			}
		}
	}
	return arg;
}

static int event_thread_create(void)
{
	struct v4l2_event_subscription sub;
	struct v4l2_control control;

	videofd = open(videodev, O_RDONLY | O_NONBLOCK);
	if (videofd < 0) {
		g_print("%s: open failed: %s\n", videodev, g_strerror(errno));
		return (-1);
	}

	memset(&sub, 0, sizeof(sub));
	sub.type = V4L2_EVENT_SOURCE_CHANGE;
	if (ioctl(videofd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		g_print("%s: subscribe V4L2_EVENT_SOURCE_CHANGE failed: %s\n",
			videodev, g_strerror(errno));
		close(videofd);
		return (-1);
	}

	memset(&sub, 0, sizeof(sub));
	sub.type = V4L2_EVENT_MOTION_DET;
	if (ioctl(videofd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		g_print("%s: subscribe V4L2_EVENT_MOTION_DET failed: %s\n",
			videodev, g_strerror(errno));
		close(videofd);
		return (-1);
	}

	memset(&control, 0, sizeof(control));
	control.id = V4L2_CID_DETECT_MD_GLOBAL_THRESHOLD;
	control.value = 0x3FFFF;
	if (ioctl(videofd, VIDIOC_S_CTRL, &control) < 0) {
		g_print("%s: set V4L2_CID_DETECT_MD_GLOBAL_THRESHOLD failed: %s\n",
			videodev, g_strerror(errno));
		close(videofd);
		return (-1);
	}

	memset(&control, 0, sizeof(control));
	control.id = V4L2_CID_DETECT_MD_MODE;
	control.value = V4L2_DETECT_MD_MODE_GLOBAL;
	if (ioctl(videofd, VIDIOC_S_CTRL, &control) < 0) {
		g_print("%s: set V4L2_DETECT_MD_MODE_GLOBAL failed: %s\n",
			videodev, g_strerror(errno));
		close(videofd);
		return (-1);
	}

	if (pipe(pipefd) < 0) {
		g_print("pipe failed: %s\n", g_strerror(errno));
		close(videofd);
		return (-1);
	}

	return pthread_create(&threadid, NULL, event_thread_func, NULL);
}

static int event_thread_cancel(void)
{
	if (write(pipefd[1], "q", 1) != 1) {
		g_print("Could not send message to event thread\n");
		return (-1);
	}

	if (pthread_join(threadid, NULL) != 0) {
		g_print("Failed to join event thread\n");
		return (-1);
	}

	close(pipefd[0]);
	close(pipefd[1]);
	close(videofd);
	return 0;
}

static gboolean message_cb(GstBus *bus,
				GstMessage *message,
				gpointer user_data)
{
	switch (GST_MESSAGE_TYPE(message)) {
	case GST_MESSAGE_ERROR:
		g_print("ERROR received\n");
		g_main_loop_quit(loop);
		break;
	case GST_MESSAGE_EOS:
		g_print("EOS reached\n");
		g_main_loop_quit(loop);
		break;
	default:
		break;
	}
	return TRUE;
}

static void sigint_handler(int signum, siginfo_t *info, void *ptr)
{
	g_print("\nCtrl-C pressed\n");
	g_main_loop_quit(loop);
}

static int get_options(int argc, char *argv[])
{
	int opt;
	videodev = "/dev/video0";
	ww = wh = wx = wy = 0;

	while ((opt = getopt(argc, argv, "v:w:h:l:t:")) != -1) {
		switch (opt) {
		case 'v':
			videodev = strdup(optarg);
			break;
		case 'w':
			ww = atoi(optarg);
			break;
		case 'h':
			wh = atoi(optarg);
			break;
		case 'l':
			wx = atoi(optarg);
			break;
		case 't':
			wy = atoi(optarg);
			break;
		default:
			g_print("Usage: %s [-v vdev] [-w width] [-h height] [-l left] [-t top]\n", argv[0]);
			g_print("default: /dev/video0, full screen output window.\n");
			return (-1);
		}
	}
	return 0;
}


int main(int argc, char *argv[])
{
	GstPad *teepad, *queuepad;
	GstBus *bus;

	gst_init(&argc, &argv);

	if (get_options(argc, argv) != 0)
		return (-1);

	pipeline = gst_pipeline_new(NULL);
	src = gst_element_factory_make("imxv4l2videosrc", NULL);
	tee = gst_element_factory_make("tee", NULL);
	queue = gst_element_factory_make("queue", NULL);
	sink = gst_element_factory_make("imxg2dvideosink", NULL);

	if (!pipeline || !src || !tee || !queue || !sink) {
		g_error("Failed to create elements");
		return (-1);
	}

	g_object_set(src, "device", videodev, NULL);
	g_object_set(src, "queue-size", 16, NULL);
	g_object_set(sink, "window-width", ww, NULL);
	g_object_set(sink, "window-height", wh, NULL);
	g_object_set(sink, "window-x-coord", wx, NULL);
	g_object_set(sink, "window-y-coord", wy, NULL);

	gst_bin_add_many(GST_BIN(pipeline), src, tee, queue, sink, NULL);
	if (!gst_element_link_many(src, tee, NULL) ||
			!gst_element_link_many(queue, sink, NULL)) {
		g_error("Failed to link elements");
		gst_object_unref(pipeline);
		return (-1);
	}

	teepad = gst_element_get_request_pad(tee, "src_%u");
	queuepad = gst_element_get_static_pad(queue, "sink");

	gst_pad_link(teepad, queuepad);
	gst_object_unref(queuepad);

	loop = g_main_loop_new(NULL, FALSE);

	bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
	gst_bus_add_signal_watch(bus);
	g_signal_connect(G_OBJECT(bus), "message", G_CALLBACK(message_cb), NULL);
	gst_object_unref(GST_OBJECT(bus));

	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	if (event_thread_create() == 0) {
		struct sigaction act = {0};
		act.sa_sigaction = sigint_handler;
		act.sa_flags = SA_SIGINFO;
		if (sigaction(SIGINT, &act, NULL) < 0)
			g_print("sigaction failed: %s\n", g_strerror(errno));

		g_main_loop_run(loop);

		event_thread_cancel();
	}

	gst_element_set_state(pipeline, GST_STATE_NULL);

	g_main_loop_unref(loop);

	gst_element_release_request_pad(tee, teepad);
	gst_object_unref(teepad);

	gst_object_unref(pipeline);

	return 0;
}
