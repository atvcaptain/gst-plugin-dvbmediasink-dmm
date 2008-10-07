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
#include <fcntl.h>

#include <gst/gst.h>

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

#include "gstdvbaudiosink.h"

GST_DEBUG_CATEGORY_STATIC (dvbaudiosink_debug);
#define GST_CAT_DEFAULT dvbaudiosink_debug

/* Filter signals and args */
enum {
	/* FILL ME */
	LAST_SIGNAL
};

enum {
	ARG_0,
	ARG_SILENT
};

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/mpeg, "
		"mpegversion = (int) { 1, 2, 4 }; "
		"audio/x-private1-ac3;"
		"audio/x-ac3")
);

#define DEBUG_INIT(bla) \
	GST_DEBUG_CATEGORY_INIT (dvbaudiosink_debug, "dvbaudiosink", 0, "dvbaudiosink element");

GST_BOILERPLATE_FULL (GstDVBAudioSink, gst_dvbaudiosink, GstBaseSink,
	GST_TYPE_BASE_SINK, DEBUG_INIT);

static void	gst_dvbaudiosink_set_property (GObject *object, guint prop_id,
																									const GValue *value,
																									GParamSpec *pspec);
static void	gst_dvbaudiosink_get_property (GObject *object, guint prop_id,
																									GValue *value,
																									GParamSpec *pspec);

static gboolean gst_dvbaudiosink_start (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_stop (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_dvbaudiosink_render (GstBaseSink * sink,
	GstBuffer * buffer);
static gboolean gst_dvbaudiosink_query (GstPad * pad, GstQuery * query);
static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink);
static gboolean gst_dvbaudiosink_set_caps (GstBaseSink * sink, GstCaps * caps);

gboolean bypass_set = FALSE;

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

	gst_element_class_add_pad_template (element_class,
		gst_static_pad_template_get (&sink_factory));
	gst_element_class_set_details (element_class, &element_details);
}

/* initialize the plugin's class */
static void
gst_dvbaudiosink_class_init (GstDVBAudioSinkClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
	GstBaseSinkClass *gstbasesink_class = GST_BASE_SINK_CLASS (klass);
	
	gobject_class->set_property = gst_dvbaudiosink_set_property;
	gobject_class->get_property = gst_dvbaudiosink_get_property;
	
	gobject_class = G_OBJECT_CLASS (klass);
	g_object_class_install_property (gobject_class, ARG_SILENT,
		g_param_spec_boolean ("silent", "Silent", "Produce verbose output ?",
													FALSE, G_PARAM_READWRITE));

	gstbasesink_class->get_times = NULL;
	gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_stop);
	gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_render);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_event);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_unlock);
	gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_set_caps);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void
gst_dvbaudiosink_init (GstDVBAudioSink *klass,
		GstDVBAudioSinkClass * gclass)
{
	GstPad *pad = GST_BASE_SINK_PAD (klass);
	
	gst_pad_set_query_function (pad, GST_DEBUG_FUNCPTR (gst_dvbaudiosink_query));
	
	klass->silent = FALSE;
	GST_BASE_SINK (klass)->sync = FALSE;
}

static void
gst_dvbaudiosink_set_property (GObject *object, guint prop_id,
																	const GValue *value, GParamSpec *pspec)
{
	GstDVBAudioSink *filter;

	g_return_if_fail (GST_IS_DVBAUDIOSINK (object));
	filter = GST_DVBAUDIOSINK (object);

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
gst_dvbaudiosink_get_property (GObject *object, guint prop_id,
																	GValue *value, GParamSpec *pspec)
{
	GstDVBAudioSink *filter;

	g_return_if_fail (GST_IS_DVBAUDIOSINK (object));
	filter = GST_DVBAUDIOSINK (object);

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
gst_dvbaudiosink_query (GstPad * pad, GstQuery * query)
{
	GstDVBAudioSink *self;
//	GstFormat format;
	
	self = GST_DVBAUDIOSINK (GST_PAD_PARENT (pad));
	switch (GST_QUERY_TYPE (query)) {
	default:
		return gst_pad_query_default (pad, query);
	}
}

static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);

	SEND_COMMAND (self, CONTROL_STOP);

	return TRUE;
}

static gboolean 
gst_dvbaudiosink_set_caps (GstBaseSink * basesink, GstCaps * caps)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);

	GstStructure *structure;
	const char *type;
	int bypass;
	
	if (self->fd < 0)
		return FALSE;
	
	structure = gst_caps_get_structure (caps, 0);	
	type = gst_structure_get_name (structure);

	self->skip = 0;
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
				printf("MIMETYPE %s version %d layer %d\n",type,mpegversion,layer);
				break;
			}
			case 2:
				bypass = 1;
				printf("MIMETYPE %s version %d\n",type,mpegversion);
				break;
			case 4:
				bypass = 8;
				printf("MIMETYPE %s version %d (AAC)\n",type,mpegversion);
				break;
			default:
				GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("unhandled mpeg version %i", mpegversion));
				break;
		}
	}
	else if (!strcmp(type, "audio/x-ac3") || !strcmp(type, "audio/ac3"))
	{
		printf("MIMETYPE %s\n",type);
		bypass = 0;
	}
	else if (!strcmp(type, "audio/x-private1-ac3"))
	{
		printf("MIMETYPE %s (DVD Audio - 2 byte skipping)\n",type);
		bypass = 0;
		self->skip = 2;
	} else
	{
		GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("unimplemented stream type %s", type));
		return FALSE;
	}

	GST_DEBUG_OBJECT(self, "setting dvb mode 0x%02x\n", bypass);

	if (ioctl(self->fd, AUDIO_SET_BYPASS_MODE, bypass) < 0)
	{
		GST_ELEMENT_ERROR (self, STREAM, DECODE, (NULL), ("hardware decoder can't be set to bypass mode %i", bypass));
		return FALSE;
	}
	bypass_set = TRUE;
	return TRUE;
}

static gboolean
gst_dvbaudiosink_event (GstBaseSink * sink, GstEvent * event)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	GST_DEBUG_OBJECT (self, "EVENT %s", gst_event_type_get_name(GST_EVENT_TYPE (event)));

	switch (GST_EVENT_TYPE (event)) {
	case GST_EVENT_FLUSH_START:
		ioctl(self->fd, AUDIO_CLEAR_BUFFER);
		break;
	case GST_EVENT_FLUSH_STOP:
		ioctl(self->fd, AUDIO_CLEAR_BUFFER);
		while (1)
		{
			gchar command;
			int res;

			READ_COMMAND (self, command, res);
			if (res < 0)
				break;
		}
		break;
	default:
		break;
	}
	return TRUE;
}

static GstFlowReturn
gst_dvbaudiosink_render (GstBaseSink * sink, GstBuffer * buffer)
{
	unsigned char pes_header[19];
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	int skip = self->skip;
	unsigned int size = GST_BUFFER_SIZE (buffer) - skip;
	fd_set readfds;
	fd_set writefds;
	gint retval;

//	printf("write %d, timestamp: %08llx\n", GST_BUFFER_SIZE (buffer), (long long)GST_BUFFER_TIMESTAMP(buffer));

	if ( !bypass_set )
	{
		GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("hardware decoder not setup (no caps in pipeline?)"));
		return GST_FLOW_ERROR;
	}

	FD_ZERO (&readfds);
	FD_SET (READ_SOCKET (self), &readfds);

	FD_ZERO (&writefds);
	FD_SET (self->fd, &writefds);

	do {
		GST_DEBUG_OBJECT (self, "going into select, have %d bytes to write",
				size);
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
	pes_header[3] = 0xC0;

#if 1
		/* do we have a timestamp? */
	if (GST_BUFFER_TIMESTAMP(buffer) != GST_CLOCK_TIME_NONE)
	{
		unsigned long long pts = GST_BUFFER_TIMESTAMP(buffer) * 9LL / 100000 /* convert ns to 90kHz */;

		pes_header[4] = (size + 8) >> 8;
		pes_header[5] = (size + 8) & 0xFF;
		
		pes_header[6] = 0x80;
		pes_header[7] = 0x80;
		
		pes_header[8] = 5;
		
		pes_header[9]	= 0x31 | ((pts >> 29) & 0xE);
		pes_header[10] = pts >> 22;
		pes_header[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes_header[12] = pts >> 7;
		pes_header[13] = 0x01 | ((pts << 1) & 0xFE);
		write(self->fd, pes_header, 14);
	} else
	{
		pes_header[4] = (size + 3) >> 8;
		pes_header[5] = (size + 3) & 0xFF;
		pes_header[6] = 0x80;
		pes_header[7] = 0x00;
		pes_header[8] = 0;
		write(self->fd, pes_header, 9);
	}
#endif

	write(self->fd, GST_BUFFER_DATA (buffer) + skip, GST_BUFFER_SIZE (buffer) - skip);

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
		ioctl(self->fd, AUDIO_CLEAR_BUFFER);
		return GST_FLOW_WRONG_STATE;
	}
}

static gboolean
gst_dvbaudiosink_start (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	self->fd = open("/dev/dvb/adapter0/audio0", O_RDWR);
//	self->fd = open("/dump.pes", O_RDWR|O_CREAT, 0555);
	
	gint control_sock[2];

	if (socketpair (PF_UNIX, SOCK_STREAM, 0, control_sock) < 0)
		goto socket_pair;

	READ_SOCKET (self) = control_sock[0];
	WRITE_SOCKET (self) = control_sock[1];

	fcntl (READ_SOCKET (self), F_SETFL, O_NONBLOCK);
	fcntl (WRITE_SOCKET (self), F_SETFL, O_NONBLOCK);

	if (self->fd)
	{
		ioctl(self->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_MEMORY);
		ioctl(self->fd, AUDIO_PLAY);
		ioctl(self->fd, AUDIO_SET_BYPASS_MODE, 0);
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
	if (self->fd >= 0)
	{
		ioctl(self->fd, AUDIO_STOP);
		ioctl(self->fd, AUDIO_SELECT_SOURCE, AUDIO_SOURCE_DEMUX);
		close(self->fd);
	}
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
	return gst_element_register (plugin, "dvbaudiosink",
						 GST_RANK_NONE,
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
