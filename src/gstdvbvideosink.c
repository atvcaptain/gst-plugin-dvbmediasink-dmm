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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/dvb/video.h>
#include <fcntl.h>
#include <string.h>

#include <gst/gst.h>

#include "gstdvbvideosink.h"

#ifndef VIDEO_GET_PTS
#define VIDEO_GET_PTS              _IOR('o', 57, gint64)
#endif

/* We add a control socket as in fdsrc to make it shutdown quickly when it's blocking on the fd.
 * Select is used to determine when the fd is ready for use. When the element state is changed,
 * it happens from another thread while fdsink is select'ing on the fd. The state-change thread 
 * sends a control message, so fdsink wakes up and changes state immediately otherwise
 * it would stay blocked until it receives some data. */

/* the select call is also performed on the control sockets, that way
 * we can send special commands to unblock the select call */
#define CONTROL_STOP						'S'		 /* stop the select call */
#define CONTROL_SOCKETS(sink)	 sink->control_sock
#define WRITE_SOCKET(sink)			sink->control_sock[1]
#define READ_SOCKET(sink)			 sink->control_sock[0]

#define SEND_COMMAND(sink, command)					\
G_STMT_START {															\
	unsigned char c; c = command;						 \
	write (WRITE_SOCKET(sink), &c, 1);				 \
} G_STMT_END

#define READ_COMMAND(sink, command, res)				\
G_STMT_START {																 \
	res = read(READ_SOCKET(sink), &command, 1);	 \
} G_STMT_END

GST_DEBUG_CATEGORY_STATIC (dvbvideosink_debug);
#define GST_CAT_DEFAULT dvbvideosink_debug

/* Filter signals and args */
enum {
	/* FILL ME */
	LAST_SIGNAL
};

enum {
	ARG_0,
	ARG_SILENT
};

#define COMMON_VIDEO_CAPS \
  "width = (int) [ 16, 4096 ], " \
  "height = (int) [ 16, 4096 ], " \
  "framerate = (fraction) [ 0, MAX ]"

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("video/mpeg, "
		"mpegversion = (int) { 1, 2, 4 }, "
		"systemstream = (boolean) false, "
	COMMON_VIDEO_CAPS "; "
		"video/x-wmv, "
	COMMON_VIDEO_CAPS "; "
		"video/x-h264, "
	COMMON_VIDEO_CAPS "; "
		"video/x-h263, "
	COMMON_VIDEO_CAPS "; "
		"video/x-divx, "
	COMMON_VIDEO_CAPS ", divxversion = (int) [ 3, 5 ]; "
		"video/x-xvid, " COMMON_VIDEO_CAPS )
);

#define DEBUG_INIT(bla) \
	GST_DEBUG_CATEGORY_INIT (dvbvideosink_debug, "dvbvideosink", 0, "dvbvideosink element");

GST_BOILERPLATE_FULL (GstDVBVideoSink, gst_dvbvideosink, GstBaseSink,
	GST_TYPE_BASE_SINK, DEBUG_INIT);

static void	gst_dvbvideosink_set_property (GObject *object, guint prop_id,
																									const GValue *value,
																									GParamSpec *pspec);
static void	gst_dvbvideosink_get_property (GObject *object, guint prop_id,
																									GValue *value,
																									GParamSpec *pspec);

static gboolean gst_dvbvideosink_start (GstBaseSink * sink);
static gboolean gst_dvbvideosink_stop (GstBaseSink * sink);
static gboolean gst_dvbvideosink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_dvbvideosink_render (GstBaseSink * sink,
	GstBuffer * buffer);
static gboolean gst_dvbvideosink_query (GstPad * pad, GstQuery * query);
static gboolean gst_dvbvideosink_set_caps (GstPad * pad, GstCaps * vscaps);
static gboolean gst_dvbvideosink_unlock (GstBaseSink * basesink);

static void
gst_dvbvideosink_base_init (gpointer klass)
{
	static GstElementDetails element_details = {
		"A DVB video sink",
		"Generic/DVBVideoSink",
		"Outputs a MPEG2 or .H264 PES / ES into a DVB video device for hardware playback",
		"Felix Domke <tmbinc@elitedvb.net>"
	};
	GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

	gst_element_class_add_pad_template (element_class,
		gst_static_pad_template_get (&sink_factory));
	gst_element_class_set_details (element_class, &element_details);
}

/* initialize the plugin's class */
static void
gst_dvbvideosink_class_init (GstDVBVideoSinkClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
	
	gobject_class->set_property = gst_dvbvideosink_set_property;
	gobject_class->get_property = gst_dvbvideosink_get_property;
	
	gobject_class = G_OBJECT_CLASS (klass);
	g_object_class_install_property (gobject_class, ARG_SILENT,
		g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
													FALSE, G_PARAM_READWRITE));

	gstbasesink_class->get_times = NULL;
	gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_dvbvideosink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_dvbvideosink_stop);
	gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_dvbvideosink_render);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_dvbvideosink_event);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_dvbvideosink_unlock);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void
gst_dvbvideosink_init (GstDVBVideoSink *klass,
		GstDVBVideoSinkClass * gclass)
{
	GstPad *pad = GST_BASE_SINK_PAD (klass);
	
	gst_pad_set_setcaps_function (pad, GST_DEBUG_FUNCPTR (gst_dvbvideosink_set_caps));
	gst_pad_set_query_function (pad, GST_DEBUG_FUNCPTR (gst_dvbvideosink_query));
	
	klass->silent = FALSE;
	klass->divx311_header = NULL;
	klass->must_send_header = FALSE;
	klass->codec_data = NULL;

	GST_BASE_SINK (klass)->sync = FALSE;
}

static void
gst_dvbvideosink_set_property (GObject *object, guint prop_id,
																	const GValue *value, GParamSpec *pspec)
{
	GstDVBVideoSink *filter;

	g_return_if_fail (GST_IS_DVBVIDEOSINK (object));
	filter = GST_DVBVIDEOSINK (object);

	switch (prop_id)
	{
	case ARG_SILENT:
		filter->silent = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_dvbvideosink_get_property (GObject *object, guint prop_id,
																	GValue *value, GParamSpec *pspec)
{
	GstDVBVideoSink *filter;

	g_return_if_fail (GST_IS_DVBVIDEOSINK (object));
	filter = GST_DVBVIDEOSINK (object);

	switch (prop_id) {
	case ARG_SILENT:
		g_value_set_boolean (value, filter->silent);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static gboolean
gst_dvbvideosink_query (GstPad * pad, GstQuery * query)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (GST_PAD_PARENT (pad));
//	GstFormat format;
	
	printf("QUERY: type: %d\n", GST_QUERY_TYPE(query));
	switch (GST_QUERY_TYPE (query)) {
	case GST_QUERY_POSITION:
	{
		gint64 cur = 0;
		GstFormat format;

		gst_query_parse_position (query, &format, NULL);
		
		if (format != GST_FORMAT_TIME)
			return gst_pad_query_default (pad, query);

		ioctl(self->fd, VIDEO_GET_PTS, &cur);
		printf("PTS: %08llx", cur);
		
		cur *= 11111;
		
		gst_query_set_position (query, format, cur);
		
		GST_DEBUG_OBJECT (self, "position format %d", format);
		return TRUE;
	}
	default:
		return gst_pad_query_default (pad, query);
	}
}

static gboolean gst_dvbvideosink_unlock (GstBaseSink * basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);

	SEND_COMMAND (self, CONTROL_STOP);

	return TRUE;
}

static gboolean
gst_dvbvideosink_event (GstBaseSink * sink, GstEvent * event)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (sink);
	GST_DEBUG_OBJECT (self, "EVENT %s", gst_event_type_get_name(GST_EVENT_TYPE (event)));

	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_FLUSH_START:
		ioctl(self->fd, VIDEO_CLEAR_BUFFER);
		self->must_send_header = TRUE;
		break;
	case GST_EVENT_FLUSH_STOP:
		ioctl(self->fd, VIDEO_CLEAR_BUFFER);
		self->must_send_header = TRUE;
		while (1)
		{
			gchar command;
			int res;

			READ_COMMAND (self, command, res);
			if (res < 0)
				break;
		}
	default:
		break;
	}
	return TRUE;
}

static GstFlowReturn
gst_dvbvideosink_render (GstBaseSink * sink, GstBuffer * buffer)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (sink);
	unsigned char *data = GST_BUFFER_DATA(buffer);
	unsigned int data_len = GST_BUFFER_SIZE (buffer);
	guint8 pes_header[2048];
	unsigned int pes_header_len=0;
	unsigned int payload_len=0;
	fd_set readfds;
	fd_set writefds;
	gint retval;

//	printf("write %d, timestamp: %08llx\n", GST_BUFFER_SIZE (buffer), (long long)GST_BUFFER_TIMESTAMP(buffer));

	FD_ZERO (&readfds);
	FD_SET (READ_SOCKET (self), &readfds);

	FD_ZERO (&writefds);
	FD_SET (self->fd, &writefds);
	
	do {
		GST_DEBUG_OBJECT (self, "going into select, have %d bytes to write",
				data_len);
		retval = select (FD_SETSIZE, &readfds, &writefds, NULL, NULL);
	} while ((retval == -1 && errno == EINTR));

	if (retval == -1)
		goto select_error;

	if (FD_ISSET (READ_SOCKET (self), &readfds)) {
		/* read all stop commands */
		while (TRUE) {
			gchar command;
			int res;

			READ_COMMAND (self, command, res);
			if (res < 0) {
				GST_LOG_OBJECT (self, "no more commands");
				/* no more commands */
				break;
			}
		}
		goto stopped;
	}

	if (self->fd < 0)
		return GST_FLOW_OK;

	pes_header[0] = 0;
	pes_header[1] = 0;
	pes_header[2] = 1;
	pes_header[3] = 0xE0;

		/* do we have a timestamp? */
	if (GST_BUFFER_TIMESTAMP(buffer) != GST_CLOCK_TIME_NONE) {
		unsigned long long pts = GST_BUFFER_TIMESTAMP(buffer) * 9LL / 100000 /* convert ns to 90kHz */;

		pes_header[6] = 0x80;
		pes_header[7] = 0xC0;
		
		pes_header[8] = 10;
		
		pes_header[9] = 0x31 | ((pts >> 29) & 0xE);
		pes_header[10] = pts >> 22;
		pes_header[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes_header[12] = pts >> 7;
		pes_header[13] = 0x01 | ((pts << 1) & 0xFE);

		int64_t dts = pts > 7508 ? pts - 7508 : pts; /* what to use as DTS-PTS offset? */

		pes_header[14] = 0x11 | ((dts >> 29) & 0xE);
		pes_header[15] = dts >> 22;
		pes_header[16] = 0x01 | ((dts >> 14) & 0xFE);
		pes_header[17] = dts >> 7;
		pes_header[18] = 0x01 | ((dts << 1) & 0xFE);

		pes_header_len = 19;

		if (self->divx311_header) {
			if (self->must_send_header) {
				write(self->fd, self->divx311_header, 63);
				self->must_send_header = FALSE;
			}
			pes_header[19] = 0;
			pes_header[20] = 0;
			pes_header[21] = 1;
			pes_header[22] = 0xb6;
			pes_header_len += 4;
		}
		else if (self->codec_data) {
			unsigned int pos = 0;
			while(TRUE) {
				unsigned int pack_len = (data[pos] << 24) | (data[pos+1] << 16) | (data[pos+2] << 8) | data[pos+3];
//				printf("patch %02x %02x %02x %02x\n",
//					data[pos],
//					data[pos + 1],
//					data[pos + 2],
//					data[pos + 3]);
				memcpy(data+pos, "\x00\x00\x00\x01", 4);
//				printf("pos %d, (%d) >= %d\n", pos, pos+pack_len, data_len);
				pos += 4;
				if ((pos + pack_len) >= data_len)
					break;
				pos += pack_len;
			}
			if (self->must_send_header) {
				unsigned char *codec_data = GST_BUFFER_DATA (self->codec_data);
				unsigned int codec_data_len = GST_BUFFER_SIZE (self->codec_data);
				unsigned int pos=0;
//				printf("1\n");
				if (codec_data_len > 7) {
					unsigned short len = (codec_data[6] << 8) | codec_data[7];
//					printf("2 %d bytes\n", len);
					if (codec_data_len >= (len + 8)) {
//						printf("3\n");
						memcpy(pes_header+pes_header_len, "\x00\x00\x00\x01", 4);
						pes_header_len += 4;
						memcpy(pes_header+pes_header_len, codec_data+8, len);
						if (!memcmp(pes_header+pes_header_len, "\x67\x64\x00", 3)) {
							pes_header[pes_header_len+3] = 0x29; // hardcode h264 level 4.1
							printf("h264 level patched!\n");
						}
						pes_header_len += len;
						pos = 8 + len;
						if (codec_data_len > (pos + 2)) {
							len = (codec_data[pos+1] << 8) | codec_data[pos+2];
//							printf("4 %d bytes\n", len);
							pos += 3;
							if (codec_data_len >= (pos+len)) {
								printf("codec data ok!\n");
								memcpy(pes_header+pes_header_len, "\x00\x00\x00\x01", 4);
								pes_header_len += 4;
								memcpy(pes_header+pes_header_len, codec_data+pos, len);
								pes_header_len += len;
							}
							else
								printf("codec_data to short(4)\n");
						}
						else
							printf("codec_data to short(3)!\n");
					}
					else
						printf("codec_data to short(2)!\n");
				}
				else
					printf("codec_data to short(1)!\n");
				self->must_send_header = FALSE;
			}
		}
	}
	else {
//		printf("no timestamp!\n");
		pes_header[6] = 0x80;
		pes_header[7] = 0x00;
		pes_header[8] = 0;
		pes_header_len = 9;
	}

	payload_len = data_len + pes_header_len - 6;

	if (payload_len <= 0xFFFF) {
		pes_header[4] = payload_len >> 8;
		pes_header[5] = payload_len & 0xFF;
	}
	else {
		pes_header[4] = 0;
		pes_header[5] = 0;
	}

	write(self->fd, pes_header, pes_header_len);
	write(self->fd, data, data_len);

	return GST_FLOW_OK;
select_error:
	{
		GST_ELEMENT_ERROR (self, RESOURCE, READ, (NULL),
				("select on file descriptor: %s.", g_strerror (errno)));
		GST_DEBUG_OBJECT (self, "Error during select");
		return GST_FLOW_ERROR;
	}
stopped:
	{
		GST_DEBUG_OBJECT (self, "Select stopped");
		ioctl(self->fd, VIDEO_CLEAR_BUFFER);
		return GST_FLOW_WRONG_STATE;
	}
}

static gboolean 
gst_dvbvideosink_set_caps (GstPad * pad, GstCaps * vscaps)
{
	GstStructure *structure = gst_caps_get_structure (vscaps, 0);
	const gchar *mimetype = gst_structure_get_name (structure);
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (GST_PAD_PARENT (pad));
	int streamtype = -1;

	if (!strcmp (mimetype, "video/mpeg")) {
		gint mpegversion;
		gst_structure_get_int (structure, "mpegversion", &mpegversion);
		switch (mpegversion) {
			case 1:
				streamtype = 6;
				printf("MIMETYPE video/mpeg1 -> VIDEO_SET_STREAMTYPE, 6\n");
			break;
			case 2:
				streamtype = 0;
				printf("MIMETYPE video/mpeg2 -> VIDEO_SET_STREAMTYPE, 0\n");
			break;
			case 4:
				streamtype = 4;
				printf("MIMETYPE video/mpeg4 -> VIDEO_SET_STREAMTYPE, 4\n");
			break;
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled mpeg version %i", mpegversion));
			break;
		}
	} else if (!strcmp (mimetype, "video/x-h264")) {
		const GValue *codec_data = gst_structure_get_value (structure, "codec_data");
		streamtype = 1;
		if (codec_data) {
			printf("H264 have codec data.. force mkv!\n");
			self->codec_data = gst_value_get_buffer (codec_data);
			gst_buffer_ref (self->codec_data);
		}
		self->must_send_header = TRUE;
		printf("MIMETYPE video/x-h264 VIDEO_SET_STREAMTYPE, 1\n");
	} else if (!strcmp (mimetype, "video/x-h263")) {
		streamtype = 2;
		printf("MIMETYPE video/x-h263 VIDEO_SET_STREAMTYPE, 2\n");
	} else if (!strcmp (mimetype, "video/x-xvid")) {
		streamtype = 10;
		printf("MIMETYPE video/x-xvid -> VIDEO_SET_STREAMTYPE, 10\n");
	} else if (!strcmp (mimetype, "video/x-divx")) {
		gint divxversion;
		gst_structure_get_int (structure, "divxversion", &divxversion);
		switch (divxversion) {
			case 3:
			{
				#define B_GET_BITS(w,e,b)  (((w)>>(b))&(((unsigned)(-1))>>((sizeof(unsigned))*8-(e+1-b))))
				#define B_SET_BITS(name,v,e,b)  (((unsigned)(v))<<(b))
				static const guint8 brcm_divx311_sequence_header[] = {
					0x00, 0x00, 0x01, 0xE0, 0x00, 0x39, 0x80, 0xC0, // PES HEADER
					0x0A, 0x3F, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0xFF, // ..
					0xFF, 0xFF, 0xFF, // ..
					0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x20, /* 0 .. 7 */
					0x08, 0xC8, 0x0D, 0x40, 0x00, 0x53, 0x88, 0x40, /* 8 .. 15 */
					0x0C, 0x40, 0x01, 0x90, 0x00, 0x97, 0x53, 0x0A, /* 16 .. 24 */
					0x00, 0x00, 0x00, 0x00,
					0x30, 0x7F, 0x00, 0x00, 0x01, 0xB2, 0x44, 0x69, /* 0 .. 7 */
					0x76, 0x58, 0x33, 0x31, 0x31, 0x41, 0x4E, 0x44  /* 8 .. 15 */
				};
				guint8 *data = malloc(63);
				gint height, width;
				gst_structure_get_int (structure, "height", &height);
				gst_structure_get_int (structure, "width", &width);
				memcpy(data, brcm_divx311_sequence_header, 63);
				self->divx311_header = data;
				data += 43;
				data[0] = B_GET_BITS(width,11,4);
				data[1] = B_SET_BITS("width [3..0]", B_GET_BITS(width,3,0), 7, 4) |
					B_SET_BITS("'10'", 0x02, 3, 2) |
					B_SET_BITS("height [11..10]", B_GET_BITS(height,11,10), 1, 0);
				data[2] = B_GET_BITS(height,9,2);
				data[3]= B_SET_BITS("height [1.0]", B_GET_BITS(height,1,0), 7, 6) |
					B_SET_BITS("'100000'", 0x20, 5, 0);
				streamtype = 13;
				self->must_send_header = TRUE;
				printf("MIMETYPE video/x-divx vers. 3 -> VIDEO_SET_STREAMTYPE, 13\n");
			}
			break;
			case 4:
				streamtype = 14;
				printf("MIMETYPE video/x-divx vers. 4 -> VIDEO_SET_STREAMTYPE, 14\n");
			break;
			case 5:
				streamtype = 15;
				printf("MIMETYPE video/x-divx vers. 5 -> VIDEO_SET_STREAMTYPE, 15\n");
			break;
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled divx version %i", divxversion));
			break;
		}
	}
	if (streamtype != -1) {
		if (ioctl(self->fd, VIDEO_SET_STREAMTYPE, streamtype) < 0)
			GST_ELEMENT_ERROR (self, STREAM, DECODE, (NULL), ("hardware decoder can't handle streamtype %i", streamtype));
	} else
		GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("unimplemented stream type %s", mimetype));

	return TRUE;
}

static gboolean
gst_dvbvideosink_start (GstBaseSink * basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);
	self->fd = open("/dev/dvb/adapter0/video0", O_RDWR);
//	self->fd = open("/dump.pes", O_RDWR|O_CREAT, 0555);

	gint control_sock[2];

	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
		goto socket_pair;

	READ_SOCKET (self) = control_sock[0];
	WRITE_SOCKET (self) = control_sock[1];

	fcntl (READ_SOCKET (self), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (self), F_SETFL, O_NONBLOCK);

	if (self->fd >= 0)
	{
		ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY);
		ioctl(self->fd, VIDEO_PLAY);
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
gst_dvbvideosink_stop (GstBaseSink * basesink)
{
	GstDVBVideoSink *self = GST_DVBVIDEOSINK (basesink);
	if (self->fd >= 0)
	{
		ioctl(self->fd, VIDEO_STOP);
		ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
		close(self->fd);
	}

	close (READ_SOCKET (self));
	close (WRITE_SOCKET (self));

	if (self->divx311_header)
		free(self->divx311_header);

	if (self->codec_data)
		gst_buffer_unref(self->codec_data);

	return TRUE;
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
	return gst_element_register (plugin, "dvbvideosink",
						 GST_RANK_NONE,
						 GST_TYPE_DVBVIDEOSINK);
}

/* this is the structure that gstreamer looks for to register plugins
 *
 * exchange the strings 'plugin' and 'Template plugin' with you plugin name and
 * description
 */
GST_PLUGIN_DEFINE (
	GST_VERSION_MAJOR,
	GST_VERSION_MINOR,
	"dvb_video_out",
	"DVB Video Output",
	plugin_init,
	VERSION,
	"LGPL",
	"GStreamer",
	"http://gstreamer.net/"
)
