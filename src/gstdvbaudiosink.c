/*
 * GStreamer DVB Media Sink
 * Copyright 2006 Felix Domke <tmbinc@elitedvb.net>
 * based on code by:
 * Copyright 2005 Thomas Vander Stichele <thomas@apestaart.org>
 * Copyright 2005 Ronald S. Bultje <rbultje@ronald.bitfreak.net>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Alternatively, the contents of this file may be used under the
 * GNU Lesser General Public License Version 2.1 (the "LGPL"), in
 * which case the following provisions apply instead of the ones
 * mentioned above:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:element-plugin
 *
 * <refsect2>
 * <title>Example launch line</title>
 * <para>
 * <programlisting>
 * gst-launch -v -m audiotestsrc ! plugin ! fakesink silent=TRUE
 * </programlisting>
 * </para>
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/video.h>
#include <fcntl.h>
#include <poll.h>

#include <gst/gst.h>

#include "gstdvbaudiosink.h"
#include "gstdvbsink-marshal.h"

/* We add a control socket as in fdsrc to make it shutdown quickly when it's blocking on the fd.
 * Poll is used to determine when the fd is ready for use. When the element state is changed,
 * it happens from another thread while fdsink is poll'ing on the fd. The state-change thread 
 * sends a control message, so fdsink wakes up and changes state immediately otherwise
 * it would stay blocked until it receives some data. */

/* the poll call is also performed on the control sockets, that way
 * we can send special commands to unblock the poll call */
#define CONTROL_STOP		'S'			/* stop the poll call */
#define CONTROL_SOCKETS(sink)	sink->control_sock
#define WRITE_SOCKET(sink)	sink->control_sock[1]
#define READ_SOCKET(sink)	sink->control_sock[0]

#define SEND_COMMAND(sink, command)			\
G_STMT_START {						\
	unsigned char c; c = command;			\
	write (WRITE_SOCKET(sink), &c, 1);		\
} G_STMT_END

#define READ_COMMAND(sink, command, res)		\
G_STMT_START {						\
	res = read(READ_SOCKET(sink), &command, 1);	\
} G_STMT_END

#ifndef AUDIO_GET_PTS
#define AUDIO_GET_PTS		_IOR('o', 19, gint64)
#endif

GST_DEBUG_CATEGORY_STATIC (dvbaudiosink_debug);
#define GST_CAT_DEFAULT dvbaudiosink_debug

enum
{
	SIGNAL_GET_DECODER_TIME,
	LAST_SIGNAL
};

static guint gst_dvbaudiosink_signals[LAST_SIGNAL] = { 0 };

guint AdtsSamplingRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0 };

static GstStaticPadTemplate sink_factory_ati_xilleon =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/mpeg, "
		"mpegversion = (int) [ 1, 2 ], "
		"layer = (int) [ 1, 2 ]; "
		"audio/x-ac3; "
		"audio/x-private1-ac3")
);

static GstStaticPadTemplate sink_factory_broadcom_dts =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/mpeg; "
		"audio/x-ac3; "
		"audio/x-private1-ac3; "
		"audio/x-dts; "
		"audio/x-private1-dts")
);

static GstStaticPadTemplate sink_factory_broadcom =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/mpeg; "
		"audio/x-ac3; "
		"audio/x-private1-ac3")
);

#define DEBUG_INIT(bla) \
	GST_DEBUG_CATEGORY_INIT (dvbaudiosink_debug, "dvbaudiosink", 0, "dvbaudiosink element");

GST_BOILERPLATE_FULL (GstDVBAudioSink, gst_dvbaudiosink, GstBaseSink, GST_TYPE_BASE_SINK, DEBUG_INIT);

static gboolean gst_dvbaudiosink_start (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_stop (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_dvbaudiosink_render (GstBaseSink * sink, GstBuffer * buffer);
static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink);
static gboolean gst_dvbaudiosink_unlock_stop (GstBaseSink * basesink);
static gboolean gst_dvbaudiosink_set_caps (GstBaseSink * sink, GstCaps * caps);
static void gst_dvbaudiosink_dispose (GObject * object);
static GstStateChangeReturn gst_dvbaudiosink_change_state (GstElement * element, GstStateChange transition);
static gint64 gst_dvbaudiosink_get_decoder_time (GstDVBAudioSink *self);

typedef enum { DM7025, DM800, DM8000, DM500HD } hardware_type_t;

static hardware_type_t hwtype;

static void
gst_dvbaudiosink_base_init (gpointer klass)
{
	static GstElementDetails element_details = {
		"A DVB audio sink",
		"Generic/DVBAudioSink",
		"Outputs a MPEG2 PES / ES into a DVB audio device for hardware playback",
		"Felix Domke <tmbinc@elitedvb.net>"
	};
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
	
	int fd = open("/proc/stb/info/model", O_RDONLY);
	if ( fd > 0 )
	{
		gchar string[8] = { 0, };
		ssize_t rd = read(fd, string, 7);
		if ( rd >= 5 )
		{
			string[rd] = 0;
			if ( !strncasecmp(string, "DM7025", 6) ) {
				hwtype = DM7025;
				GST_INFO ("model is DM7025 set ati xilleon caps");
			}
			else if ( !strncasecmp(string, "DM8000", 6) ) {
				hwtype = DM8000;
				GST_INFO ("model is DM8000 set broadcom dts caps");
				gst_element_class_add_pad_template (element_class,
					gst_static_pad_template_get (&sink_factory_broadcom_dts));
			}
			else if ( !strncasecmp(string, "DM800", 5) ) {
				hwtype = DM800;
				GST_INFO ("model is DM800 set broadcom caps", string);
				gst_element_class_add_pad_template (element_class,
					gst_static_pad_template_get (&sink_factory_broadcom));
			}
			else if ( !strncasecmp(string, "DM500HD", 7) ) {
				hwtype = DM500HD;
				GST_INFO ("model is DM500HD set broadcom caps", string);
				gst_element_class_add_pad_template (element_class,
					gst_static_pad_template_get (&sink_factory_broadcom));
			}
		}
		close(fd);
	}

	if (hwtype == DM7025) {
		gst_element_class_add_pad_template (element_class,
			gst_static_pad_template_get (&sink_factory_ati_xilleon));
	}

	gst_element_class_set_details (element_class, &element_details);
}

/* initialize the plugin's class */
static void
gst_dvbaudiosink_class_init (GstDVBAudioSinkClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
	GstElementClass *gelement_class = GST_ELEMENT_CLASS (klass);

	gobject_class->dispose = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_dispose);

	gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_stop);
	gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_render);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_event);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_unlock);
	gstbasesink_class->unlock_stop = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_unlock_stop);
	gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_set_caps);

	gelement_class->change_state = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_change_state);

	gst_dvbaudiosink_signals[SIGNAL_GET_DECODER_TIME] =
		g_signal_new ("get-decoder-time",
		G_TYPE_FROM_CLASS (klass),
		G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
		G_STRUCT_OFFSET (GstDVBAudioSinkClass, get_decoder_time),
		NULL, NULL, gst_dvbsink_marshal_INT64__VOID, G_TYPE_INT64, 0);

	klass->get_decoder_time = gst_dvbaudiosink_get_decoder_time;
}

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void
gst_dvbaudiosink_init (GstDVBAudioSink *klass, GstDVBAudioSinkClass * gclass)
{
	klass->bypass_set = FALSE;

	klass->aac_adts_header_valid = FALSE;

	klass->no_write = 0;

	gst_base_sink_set_sync (GST_BASE_SINK(klass), FALSE);
	gst_base_sink_set_async_enabled (GST_BASE_SINK(klass), TRUE);
}

static void gst_dvbaudiosink_dispose (GObject * object)
{
	GstDVBAudioSink *self;
	
	self = GST_DVBAUDIOSINK (object);
	
	GST_DEBUG_OBJECT (self, "dispose");

	close (READ_SOCKET (self));
	close (WRITE_SOCKET (self));
	READ_SOCKET (self) = -1;
	WRITE_SOCKET (self) = -1;

	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static gint64 gst_dvbaudiosink_get_decoder_time (GstDVBAudioSink *self)
{
	gint64 cur = 0;
	static gint64 last_pos = 0;

	ioctl(self->fd, AUDIO_GET_PTS, &cur);

	/* workaround until driver fixed */
	if (cur)
		last_pos = cur;
	else
		cur = last_pos;

	cur *= 11111;

	return cur;
}

static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	GST_OBJECT_LOCK(self);
	self->no_write |= 1;
	GST_OBJECT_UNLOCK(self);
	SEND_COMMAND (self, CONTROL_STOP);
	GST_DEBUG_OBJECT (basesink, "unlock");
	return TRUE;
}

static gboolean gst_dvbaudiosink_unlock_stop (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	GST_OBJECT_LOCK(self);
	self->no_write &= ~1;
	GST_OBJECT_UNLOCK(self);
	SEND_COMMAND (self, CONTROL_STOP);
	GST_DEBUG_OBJECT (basesink, "unlock_stop");
	return TRUE;
}

static gboolean 
gst_dvbaudiosink_set_caps (GstBaseSink * basesink, GstCaps * caps)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	GstStructure *structure = gst_caps_get_structure (caps, 0);
	const char *type = gst_structure_get_name (structure);
	int bypass = -1;

	self->skip = 0;
	self->is_dts = 0;
	if (!strcmp(type, "audio/mpeg")) {
		gint mpegversion;
		gst_structure_get_int (structure, "mpegversion", &mpegversion);
		switch (mpegversion) {
			case 1:
			{
				gint layer;
				gst_structure_get_int (structure, "layer", &layer);
				if ( layer == 3 )
					bypass = 0xA;
				else
					bypass = 1;
				GST_INFO_OBJECT (self, "MIMETYPE %s version %d layer %d",type,mpegversion,layer);
				break;
			}
			case 2:
			case 4:
			{
				const GValue *codec_data = gst_structure_get_value (structure, "codec_data");
				GST_INFO_OBJECT (self, "MIMETYPE %s version %d (AAC)", type, mpegversion);
				if (codec_data) {
					guint8 *h = GST_BUFFER_DATA(gst_value_get_buffer (codec_data));
					guint8 obj_type = ((h[0] & 0xC) >> 2) + 1;
					guint8 rate_idx = ((h[0] & 0x3) << 1) | ((h[1] & 0x80) >> 7);
					guint8 channels = (h[1] & 0x78) >> 3;
					GST_INFO_OBJECT (self, "have codec data -> obj_type = %d, rate_idx = %d, channels = %d\n",
						obj_type, rate_idx, channels);
					/* Sync point over a full byte */
					self->aac_adts_header[0] = 0xFF;
					/* Sync point continued over first 4 bits + static 4 bits
					 * (ID, layer, protection)*/
					self->aac_adts_header[1] = 0xF1;
					if (mpegversion == 2)
						self->aac_adts_header[1] |= 8;
					/* Object type over first 2 bits */
					self->aac_adts_header[2] = obj_type << 6;
					/* rate index over next 4 bits */
					self->aac_adts_header[2] |= rate_idx << 2;
					/* channels over last 2 bits */
					self->aac_adts_header[2] |= (channels & 0x4) >> 2;
					/* channels continued over next 2 bits + 4 bits at zero */
					self->aac_adts_header[3] = (channels & 0x3) << 6;
					self->aac_adts_header_valid = TRUE;
				}
				else {
					gint rate, channels, rate_idx=0, obj_type=1; // hardcoded yet.. hopefully this works every time ;)
					GST_INFO_OBJECT (self, "no codec data");
					if (gst_structure_get_int (structure, "rate", &rate) && gst_structure_get_int (structure, "channels", &channels)) {
						do {
							if (AdtsSamplingRates[rate_idx] == rate)
								break;
							++rate_idx;
						} while (AdtsSamplingRates[rate_idx]);
						if (AdtsSamplingRates[rate_idx]) {
							GST_INFO_OBJECT (self, "mpegversion %d, channels %d, rate %d, rate_idx %d\n", mpegversion, channels, rate, rate_idx);
							/* Sync point over a full byte */
							self->aac_adts_header[0] = 0xFF;
							/* Sync point continued over first 4 bits + static 4 bits
							 * (ID, layer, protection)*/
							self->aac_adts_header[1] = 0xF1;
							if (mpegversion == 2)
								self->aac_adts_header[1] |= 8;
							/* Object type over first 2 bits */
							self->aac_adts_header[2] = obj_type << 6;
							/* rate index over next 4 bits */
							self->aac_adts_header[2] |= rate_idx << 2;
							/* channels over last 2 bits */
							self->aac_adts_header[2] |= (channels & 0x4) >> 2;
							/* channels continued over next 2 bits + 4 bits at zero */
							self->aac_adts_header[3] = (channels & 0x3) << 6;
							self->aac_adts_header_valid = TRUE;
						}
					}
				}
				if (bypass == -1)
					bypass = 0x0b;
				break;
			}
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled mpeg version %i", mpegversion));
				break;
		}
	}
	else if (!strcmp(type, "audio/x-ac3") || !strcmp(type, "audio/ac3"))
	{
		GST_INFO_OBJECT (self, "MIMETYPE %s",type);
		bypass = 0;
	}
	else if (!strcmp(type, "audio/x-private1-dts"))
	{
		GST_INFO_OBJECT (self, "MIMETYPE %s (DVD Audio - 2 byte skipping)",type);
		bypass = 2;
		self->skip = 2;
		self->is_dts = 1;
	}
	else if (!strcmp(type, "audio/x-private1-ac3"))
	{
		GST_INFO_OBJECT (self, "MIMETYPE %s (DVD Audio - 2 byte skipping)",type);
		bypass = 0;
		self->skip = 2;
	} 
	else if (!strcmp(type, "audio/x-dts") || !strcmp(type, "audio/dts"))
	{
		GST_INFO_OBJECT (self, "MIMETYPE %s",type);
		bypass = 2;
		self->is_dts = 1;
	} else
	{
		GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("unimplemented stream type %s", type));
		return FALSE;
	}

	GST_INFO_OBJECT(self, "setting dvb mode 0x%02x\n", bypass);

	if (ioctl(self->fd, AUDIO_SET_BYPASS_MODE, bypass) < 0)
	{
		if (self->is_dts) {
			GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("hardware decoder can't be set to bypass mode type %s", type));
			return FALSE;
		}
		GST_ELEMENT_WARNING (self, STREAM, DECODE, (NULL), ("hardware decoder can't be set to bypass mode %i.", bypass));
// 		return FALSE;
	}
	self->bypass_set = TRUE;
	return TRUE;
}

static gboolean
gst_dvbaudiosink_event (GstBaseSink * sink, GstEvent * event)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	GST_DEBUG_OBJECT (self, "EVENT %s", gst_event_type_get_name(GST_EVENT_TYPE (event)));

	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_FLUSH_START:
		GST_OBJECT_LOCK(self);
		self->no_write |= 2;
		GST_OBJECT_UNLOCK(self);
		SEND_COMMAND (self, CONTROL_STOP);
	case GST_EVENT_FLUSH_STOP:
		ioctl(self->fd, AUDIO_CLEAR_BUFFER);
		GST_OBJECT_LOCK(self);
		self->no_write &= ~2;
		GST_OBJECT_UNLOCK(self);
		SEND_COMMAND (self, CONTROL_STOP);
		break;
	case GST_EVENT_EOS:
	{
		struct pollfd pfd[2];
		int retval;
		pfd[0].fd = READ_SOCKET(self);
		pfd[0].events = POLLIN;

		if (self->prev_data) {
			int i=0;
			for (; i < 3; ++i)  // write the last frame three times more to fill the buffer...
				gst_dvbaudiosink_render (sink, self->prev_data);
		}

		do {
			unsigned long long cur;

			ioctl(self->fd, AUDIO_GET_PTS, &cur);

			long long diff = self->pts_eos - cur;

			GST_DEBUG_OBJECT (self, "at %llx last %llx (diff %lld)\n", cur, self->pts_eos, diff);

			if ( diff <= 100 )
				break;

			retval = poll(pfd, 1, 500);

				/* check for flush */
			if (pfd[0].revents & POLLIN) {
				GST_DEBUG_OBJECT (self, "wait EOS aborted!!\n");
				return FALSE;
			}
		} while (1);

		break;
	}
	case GST_EVENT_NEWSEGMENT:{
		GstFormat fmt;
		gboolean update;
		gdouble rate, applied_rate;
		gint64 cur, stop, time;
		int skip = 0, repeat = 0, ret;
		gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,	&fmt, &cur, &stop, &time);
		GST_DEBUG_OBJECT (self, "GST_EVENT_NEWSEGMENT rate=%f applied_rate=%f\n", rate, applied_rate);

		int video_fd = open("/dev/dvb/adapter0/video0", O_RDWR);
		if (fmt == GST_FORMAT_TIME)
		{
			if ( rate > 1 )
				skip = (int) rate;
			else if ( rate < 1 )
				repeat = 1.0/rate;
			ret = ioctl(video_fd, VIDEO_SLOWMOTION, repeat);
			ret = ioctl(video_fd, VIDEO_FAST_FORWARD, skip);
//			gst_segment_set_newsegment_full (&dec->segment, update, rate, applied_rate, dformat, cur, stop, time);
		}
		close(video_fd);
		break;
	}

	default:
		break;
	}
	return TRUE;
}

#define ASYNC_WRITE(data, len) do { \
		switch(AsyncWrite(sink, self, data, len)) { \
		case -1: goto poll_error; \
		case -2: goto stopped; \
		case -3: goto write_error; \
		default: break; \
		} \
	} while(0)

static int AsyncWrite(GstBaseSink * sink, GstDVBAudioSink *self, unsigned char *data, unsigned int len)
{
	unsigned int written=0;
	struct pollfd pfd[2];

	pfd[0].fd = READ_SOCKET(self);
	pfd[0].events = POLLIN;
	pfd[1].fd = self->fd;
	pfd[1].events = POLLOUT;

	do {
		GST_LOG_OBJECT (self, "going into poll, have %d bytes to write", len);
		if (poll(pfd, 2, -1) == -1) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (pfd[0].revents & POLLIN) {
			/* read all stop commands */
			while (TRUE) {
				gchar command;
				int res;
				READ_COMMAND (self, command, res);
				if (res < 0) {
					GST_DEBUG_OBJECT (self, "no more commands");
					/* no more commands */
					break;
				}
			}
			return -2;
		}
		if (self->no_write)
			break;
		else if (pfd[1].revents & POLLOUT) {
			int wr = write(self->fd, data+written, len - written);
			if (wr < 0) {
				switch (errno) {
					case EINTR:
					case EAGAIN:
						continue;
					default:
						return -3;
				}
			}
			written += wr;
		}
	} while (written != len);

	return 0;
}


static GstFlowReturn
gst_dvbaudiosink_render (GstBaseSink * sink, GstBuffer * buffer)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	unsigned char pes_header[64];
	int skip = self->skip;
	unsigned int size = GST_BUFFER_SIZE (buffer) - skip;
	unsigned char *data = GST_BUFFER_DATA (buffer) + skip;
	size_t pes_header_size;

//	int i=0;
//	for (;i < (size > 0x1F ? 0x1F : size); ++i)
//		printf("%02x ", data[i]);
//	printf("%d bytes\n", size);
//	printf("timestamp: %08llx\n", (long long)GST_BUFFER_TIMESTAMP(buffer));

	if ( !self->bypass_set )
	{
		GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("hardware decoder not setup (no caps in pipeline?)"));
		return GST_FLOW_ERROR;
	}

	if (self->fd < 0)
		return GST_FLOW_OK;

	pes_header[0] = 0;
	pes_header[1] = 0;
	pes_header[2] = 1;
	pes_header[3] = 0xC0;

	if (self->is_dts) {
		int pos=0;
		while((pos+3) < size) {
			if (!strcmp((char*)(data+pos), "\x64\x58\x20\x25")) {  // is DTS-HD ?
				skip += (size - pos);
				size = pos;
				break;
			}
			++pos;
		}
	}

	if (self->aac_adts_header_valid)
		size += 7;

		/* do we have a timestamp? */
	if (GST_BUFFER_TIMESTAMP(buffer) != GST_CLOCK_TIME_NONE)
	{
		unsigned long long pts = GST_BUFFER_TIMESTAMP(buffer) * 9LL / 100000 /* convert ns to 90kHz */;

		self->pts_eos = pts;

		pes_header[6] = 0x80;

		pes_header[9]  = 0x21 | ((pts >> 29) & 0xE);
		pes_header[10] = pts >> 22;
		pes_header[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes_header[12] = pts >> 7;
		pes_header[13] = 0x01 | ((pts << 1) & 0xFE);

		if (hwtype == DM7025) {  // DM7025 needs DTS in PES header
			int64_t dts = pts; // what to use as DTS-PTS offset?
			pes_header[4] = (size + 13) >> 8;
			pes_header[5] = (size + 13) & 0xFF;
			pes_header[7] = 0xC0;
			pes_header[8] = 10;
			pes_header[9] |= 0x10;

			pes_header[14] = 0x11 | ((dts >> 29) & 0xE);
			pes_header[15] = dts >> 22;
			pes_header[16] = 0x01 | ((dts >> 14) & 0xFE);
			pes_header[17] = dts >> 7;
			pes_header[18] = 0x01 | ((dts << 1) & 0xFE);
			pes_header_size = 19;
		}
		else {
			pes_header[4] = (size + 8) >> 8;
			pes_header[5] = (size + 8) & 0xFF;
			pes_header[7] = 0x80;
			pes_header[8] = 5;
			pes_header_size = 14;
		}
	}
	else {
		pes_header[4] = (size + 3) >> 8;
		pes_header[5] = (size + 3) & 0xFF;
		pes_header[6] = 0x80;
		pes_header[7] = 0x00;
		pes_header[8] = 0;
		pes_header_size = 9;
	}

	if (self->aac_adts_header_valid) {
		self->aac_adts_header[3] &= 0xC0;
		/* frame size over last 2 bits */
		self->aac_adts_header[3] |= (size & 0x1800) >> 11;
		/* frame size continued over full byte */
		self->aac_adts_header[4] = (size & 0x1FF8) >> 3;
		/* frame size continued first 3 bits */
		self->aac_adts_header[5] = (size & 7) << 5;
		/* buffer fullness (0x7FF for VBR) over 5 last bits */
		self->aac_adts_header[5] |= 0x1F;
		/* buffer fullness (0x7FF for VBR) continued over 6 first bits + 2 zeros for
		 * number of raw data blocks */
		self->aac_adts_header[6] = 0xFC;
		memcpy(pes_header + pes_header_size, self->aac_adts_header, 7);
		pes_header_size += 7;
	}

	ASYNC_WRITE(pes_header, pes_header_size);
	ASYNC_WRITE(data, GST_BUFFER_SIZE (buffer) - skip);

	gst_buffer_ref(buffer);

	if (self->prev_data)
		gst_buffer_unref(self->prev_data);

	self->prev_data = buffer;

	return GST_FLOW_OK;
poll_error:
	{
		GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
				("poll on file descriptor: %s.", g_strerror (errno)));
		GST_WARNING_OBJECT (self, "Error during poll");
		return GST_FLOW_ERROR;
	}
write_error:
	{
		GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
				("write on file descriptor: %s.", g_strerror (errno)));
		GST_WARNING_OBJECT (self, "Error during write");
		return GST_FLOW_ERROR;
	}
stopped:
	{

		GST_DEBUG_OBJECT (self, "poll stopped");
		return GST_FLOW_OK;
	}
}

static gboolean
gst_dvbaudiosink_start (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	gint control_sock[2];

	GST_DEBUG_OBJECT (self, "start");

	self->fd = open("/dev/dvb/adapter0/audio0", O_RDWR|O_NONBLOCK);
//	self->fd = open("/dump.pes", O_RDWR|O_CREAT, 0555);

	if (socketpair(PF_UNIX, SOCK_STREAM, 0, control_sock) < 0) {
		perror("socketpair");
		goto socket_pair;
	}

	READ_SOCKET (self) = control_sock[0];
	WRITE_SOCKET (self) = control_sock[1];

	fcntl (READ_SOCKET (self), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (self), F_SETFL, O_NONBLOCK);

	if (self->fd)
	{
		ioctl(self->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY);
		ioctl(self->fd, AUDIO_PLAY);
	}
	return TRUE;
	/* ERRORS */
socket_pair:
	{
		GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ_WRITE, (NULL),
				GST_ERROR_SYSTEM);
		return FALSE;
	}
}

static gboolean
gst_dvbaudiosink_stop (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);

	GST_DEBUG_OBJECT (self, "stop");

	if (self->fd >= 0)
	{
		int video_fd = open("/dev/dvb/adapter0/video0", O_RDWR);

		ioctl(self->fd, AUDIO_STOP);
		ioctl(self->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX);

		if ( video_fd > 0 )
		{
			ioctl(video_fd, VIDEO_SLOWMOTION, 0);
			ioctl(video_fd, VIDEO_FAST_FORWARD, 0);
			close (video_fd);
		}
		close(self->fd);
	}

	if (self->prev_data)
		gst_buffer_unref(self->prev_data);

	return TRUE;
}

static GstStateChangeReturn
gst_dvbaudiosink_change_state (GstElement * element, GstStateChange transition)
{
	GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (element);

	switch (transition) {
	case GST_STATE_CHANGE_NULL_TO_READY:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_NULL_TO_READY");
		break;
	case GST_STATE_CHANGE_READY_TO_PAUSED:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_READY_TO_PAUSED");
		ioctl(self->fd, AUDIO_PAUSE);
		break;
	case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_PAUSED_TO_PLAYING");
		ioctl(self->fd, AUDIO_CONTINUE);
		break;
	default:
		break;
	}

	ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

	switch (transition) {
	case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_PLAYING_TO_PAUSED");
		ioctl(self->fd, AUDIO_PAUSE);
		break;
	case GST_STATE_CHANGE_PAUSED_TO_READY:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_PAUSED_TO_READY");
		SEND_COMMAND (self, CONTROL_STOP);
		break;
	case GST_STATE_CHANGE_READY_TO_NULL:
		GST_DEBUG_OBJECT (self,"GST_STATE_CHANGE_READY_TO_NULL");
		SEND_COMMAND (self, CONTROL_STOP);
		break;
	default:
		break;
	}

	return ret;
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and pad templates
 * register the features
 *
 * exchange the string 'plugin' with your elemnt name
 */
static gboolean
plugin_init (GstPlugin *plugin)
{
	return gst_element_register (plugin, "dvbaudiosink",
						 GST_RANK_PRIMARY,
						 GST_TYPE_DVBAUDIOSINK);
}

/* this is the structure that gstreamer looks for to register plugins
 *
 * exchange the strings 'plugin' and 'Template plugin' with you plugin name and
 * description
 */
GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	"dvb_audio_out",
	"DVB Audio Output",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer",
	"http://gstreamer.net/"
)
