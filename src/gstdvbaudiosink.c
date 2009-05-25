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

/* We add a control socket as in fdsrc to make it shutdown quickly when it's blocking on the fd.
 * Poll is used to determine when the fd is ready for use. When the element state is changed,
 * it happens from another thread while fdsink is select'ing on the fd. The state-change thread 
 * sends a control message, so fdsink wakes up and changes state immediately otherwise
 * it would stay blocked until it receives some data. */

/* the poll call is also performed on the control sockets, that way
 * we can send special commands to unblock the select call */
#define CONTROL_STOP		'S'			/* stop the select call */
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

#define GST_DVBAUDIOSINK_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_DVBAUDIOSINK, GstDVBAudioSinkPrivate))

struct _GstDVBAudioSinkPrivate
{

	gboolean bypass_set;
	hardwaretype_t model;

};

/* Filter signals and args */
enum {
	/* FILL ME */
	LAST_SIGNAL
};

enum {
	ARG_0,
	ARG_SILENT
};

guint AdtsSamplingRates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0 };

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("audio/mpeg; "
		"audio/x-ac3; "
		"audio/x-private1-ac3; "
		"audio/x-dts; "
		"audio/x-private1-dts" )
);

enum
{
  PROP_0,
  PROP_BUFFER_TIME,
  PROP_LATENCY_TIME,
  PROP_PROVIDE_CLOCK,
  PROP_SLAVE_METHOD
};

#define DEFAULT_PROVIDE_CLOCK   TRUE
#define DEFAULT_SLAVE_METHOD    GST_BASE_AUDIO_SINK_SLAVE_SKEW

#define DEBUG_INIT(bla) \
	GST_DEBUG_CATEGORY_INIT (dvbaudiosink_debug, "dvbaudiosink", 0, "dvbaudiosink element");

GST_BOILERPLATE_FULL (GstDVBAudioSink, gst_dvbaudiosink, GstBaseSink, GST_TYPE_BASE_SINK, DEBUG_INIT);

static void	gst_dvbaudiosink_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
static void	gst_dvbaudiosink_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
static gboolean gst_dvbaudiosink_start (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_stop (GstBaseSink * sink);
static gboolean gst_dvbaudiosink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_dvbaudiosink_render (GstBaseSink * sink, GstBuffer * buffer);
static gboolean gst_dvbaudiosink_query (GstElement * element, GstQuery * query);
static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink);
static gboolean gst_dvbaudiosink_unlock_stop (GstBaseSink * basesink);
static gboolean gst_dvbaudiosink_set_caps (GstBaseSink * sink, GstCaps * caps);
static GstCaps *gst_dvbaudiosink_get_caps (GstBaseSink * bsink);
static GstClock * gst_dvbaudiosink_provide_clock (GstElement * elem);
static GstClockTime  gst_dvbaudiosink_get_time (GstClock * clock, GstDVBAudioSink * sink);

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
	g_type_class_add_private (klass, sizeof (GstDVBAudioSinkPrivate));
	
	gobject_class->set_property = gst_dvbaudiosink_set_property;
	gobject_class->get_property = gst_dvbaudiosink_get_property;
	
	gobject_class = G_OBJECT_CLASS (klass);
	g_object_class_install_property (gobject_class, ARG_SILENT, g_param_spec_boolean
		("silent", "Silent", "Produce verbose output ?", FALSE, G_PARAM_READWRITE));
	g_object_class_install_property (gobject_class, PROP_PROVIDE_CLOCK, g_param_spec_boolean
		("provide-clock", "Provide Clock", "Provide a clock to be used as the global pipeline clock", DEFAULT_PROVIDE_CLOCK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gstbasesink_class->get_times = NULL;
	gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_start);
	gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_stop);
	gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_render);
	gstbasesink_class->event = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_event);
	gstbasesink_class->unlock = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_unlock);
	gstbasesink_class->unlock_stop = GST_DEBUG_FUNCPTR ( gst_dvbaudiosink_unlock_stop);
	gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_set_caps);
	gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_get_caps);
	GST_ELEMENT_CLASS (klass)->provide_clock = GST_DEBUG_FUNCPTR (gst_dvbaudiosink_provide_clock);
	GST_ELEMENT_CLASS (klass)->query = GST_DEBUG_FUNCPTR(gst_dvbaudiosink_query);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set functions
 * initialize structure
 */
static void
gst_dvbaudiosink_init (GstDVBAudioSink *klass, GstDVBAudioSinkClass * gclass)
{
	klass->priv = GST_DVBAUDIOSINK_GET_PRIVATE (klass);
	klass->priv->bypass_set = FALSE;
	klass->provide_clock = DEFAULT_PROVIDE_CLOCK;
	klass->silent = FALSE;
	klass->aac_adts_header_valid = FALSE;

	GST_BASE_SINK (klass)->sync = TRUE;

	klass->provided_clock = gst_audio_clock_new ("GstDVBAudioSinkClock", (GstAudioClockGetTimeFunc) gst_dvbaudiosink_get_time, klass);

	klass->priv->model = DMLEGACY;
	int fd = open("/proc/stb/info/model", O_RDONLY);
	if ( fd > 0 )
	{
		gchar string[8] = { 0, };
		ssize_t rd = read(fd, string, 6);
		if ( rd >= 5 )
		{
			if ( !strncasecmp(string, "DM7025", 6) )
				klass->priv->model = DM7025;
			else if ( !strncasecmp(string, "DM8000", 6) )
				klass->priv->model = DM8000;
			else if ( !strncasecmp(string, "DM800", 5) )
				klass->priv->model = DM800;
		}
		close(fd);
		GST_INFO_OBJECT (klass, "found hardware model %s (%i)",string,klass->priv->model);
	}
}

static GstClock *
gst_dvbaudiosink_provide_clock (GstElement * element)
{
  GstClock *clock;
  GstDVBAudioSink *self = GST_DVBAUDIOSINK (element);

  GST_OBJECT_LOCK (self);
  if (!self->provide_clock)
    goto clock_disabled;

  clock = GST_CLOCK_CAST (gst_object_ref (self->provided_clock));
  GST_OBJECT_UNLOCK (self);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (self, "clock provide disabled");
    GST_OBJECT_UNLOCK (self);
    return NULL;
  }
}

static void
gst_dvbaudiosink_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	GstDVBAudioSink *self;

	g_return_if_fail (GST_IS_DVBAUDIOSINK (object));
	self = GST_DVBAUDIOSINK (object);

	switch (prop_id)
	{
	case ARG_SILENT:
		self->silent = g_value_get_boolean (value);
		break;
	case PROP_PROVIDE_CLOCK:
		gst_dvbaudiosink_set_provide_clock (self, g_value_get_boolean (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
gst_dvbaudiosink_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GstDVBAudioSink *self;

	g_return_if_fail (GST_IS_DVBAUDIOSINK (object));
	self = GST_DVBAUDIOSINK (object);

	switch (prop_id) {
	case ARG_SILENT:
		g_value_set_boolean (value, self->silent);
		break;
	case PROP_PROVIDE_CLOCK:
		g_value_set_boolean (value, gst_dvbaudiosink_get_provide_clock (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static gboolean
gst_dvbaudiosink_query (GstElement * element, GstQuery * query)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (element);

	switch (GST_QUERY_TYPE (query)) {
	case GST_QUERY_POSITION:
	{
		gint64 cur = 0, res = 0;
		static gint64 last_pos = 0;
		GstFormat format;

		gst_query_parse_position (query, &format, NULL);

		if (format != GST_FORMAT_TIME)
			goto query_default;

		ioctl(self->fd, AUDIO_GET_PTS, &cur);

		/* workaround until driver fixed */
		if (cur)
			last_pos = cur;
		else
			cur = last_pos;

		res = cur *11111;

		gst_query_set_position (query, format, res);

		GST_LOG_OBJECT (self, "GST_QUERY_POSITION pts=%lld: %" G_GUINT64_FORMAT ", time: %" GST_TIME_FORMAT, cur, GST_TIME_ARGS (res));
		return TRUE;
	}
	default:
query_default:
		return GST_ELEMENT_CLASS (parent_class)->query (element, query);
	}
}

static GstClockTime
gst_dvbaudiosink_get_time (GstClock * clock, GstDVBAudioSink * sink)
{
	GstClockTime result;
	
	gint64 cur = 0;
	ioctl(sink->fd, AUDIO_GET_PTS, &cur);

	result = cur * 11111;

	GST_LOG_OBJECT (sink, "get_time pts=%lld: %" G_GUINT64_FORMAT ", time: %" GST_TIME_FORMAT, cur, GST_TIME_ARGS (result));

	return result;
}

void
gst_dvbaudiosink_set_provide_clock (GstDVBAudioSink * sink, gboolean provide)
{
  g_return_if_fail (GST_IS_DVBAUDIOSINK (sink));

  GST_OBJECT_LOCK (sink);
  sink->provide_clock = provide;
  GST_OBJECT_UNLOCK (sink);
}

gboolean
gst_dvbaudiosink_get_provide_clock (GstDVBAudioSink * sink)
{
  gboolean result;

  g_return_val_if_fail (GST_IS_DVBAUDIOSINK (sink), FALSE);

  GST_OBJECT_LOCK (sink);
  result = sink->provide_clock;
  GST_OBJECT_UNLOCK (sink);

  return result;
}

static gboolean gst_dvbaudiosink_unlock (GstBaseSink * basesink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);

	SEND_COMMAND (self, CONTROL_STOP);

	return TRUE;
}

static gboolean gst_dvbaudiosink_unlock_stop (GstBaseSink * sink)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	while (TRUE)
	{
		gchar command;
		int res;
		
		READ_COMMAND (self, command, res);
		if (res < 0)
		{
		GST_LOG_OBJECT (self, "no more commands");
		/* no more commands */
		break;
		}
	}
	return TRUE;
}

static GstCaps *gst_dvbaudiosink_get_caps (GstBaseSink * basesink)
{
	GstElementClass *element_class;
	GstPadTemplate *pad_template;
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	GstCaps *in_caps, *caps;

	element_class = GST_ELEMENT_GET_CLASS (self);
	pad_template = gst_element_class_get_pad_template(element_class, "sink");
	g_return_val_if_fail (pad_template != NULL, NULL);
	
	in_caps = gst_caps_copy (gst_pad_template_get_caps (pad_template));
	in_caps = gst_caps_make_writable (in_caps);

	GstStructure *s;
	gint i;
	
	caps = gst_caps_new_empty ();

	for (i = 0; i < gst_caps_get_size (in_caps); ++i)
	{	
		s = gst_caps_get_structure (in_caps, i);
		if ( gst_structure_has_name (s, "audio/mpeg") )
		{
			GstStructure *mp1_struct = gst_structure_copy (s);

			gst_structure_set (mp1_struct, "mpegversion", G_TYPE_INT, 1, NULL);
			gst_structure_set (mp1_struct, "layer", GST_TYPE_INT_RANGE, 1, 2, NULL);
			gst_structure_set (mp1_struct, "rate", GST_TYPE_INT_RANGE, 0, 48000, NULL);
			gst_caps_append_structure (caps, mp1_struct);

			if ( self->priv->model >= DM800 )
			{
				GstStructure *mp3_struct = gst_structure_copy (s);
				gst_structure_set (mp3_struct, "mpegversion", G_TYPE_INT, 1, NULL);
				gst_structure_set (mp3_struct, "layer", G_TYPE_INT, 3, NULL);
				GValue value = { 0 };
				GValue rate_value = { 0 };
				g_value_init (&rate_value, GST_TYPE_LIST);
				g_value_init (&value, G_TYPE_INT);
				g_value_set_int (&value, 32000);
				gst_value_list_append_value (&rate_value, &value);
				g_value_set_int (&value, 44100);
				gst_value_list_append_value (&rate_value, &value);
				g_value_set_int (&value, 48000);
				gst_value_list_append_value (&rate_value, &value);
				gst_structure_set_value (mp3_struct, "rate", &rate_value);
				g_value_unset (&value);
				g_value_unset (&rate_value);
				gst_caps_append_structure (caps, mp3_struct);
	
				GstStructure *mp2_struct = gst_structure_copy (s);
				gst_structure_set (mp2_struct, "mpegversion", G_TYPE_INT, 2, NULL);
				g_value_init (&rate_value, GST_TYPE_LIST);
				g_value_init (&value, G_TYPE_INT);
				gint rate_idx=0;
				do {
					g_value_set_int (&value, AdtsSamplingRates[rate_idx]);
					gst_value_list_append_value (&rate_value, &value);
					++rate_idx;
				} while (AdtsSamplingRates[rate_idx]);
				gst_structure_set_value (mp2_struct, "rate", &rate_value);
				g_value_unset (&value);
				g_value_unset (&rate_value);
				gst_caps_append_structure (caps, mp2_struct);
	
				GstStructure *mp4_struct = gst_structure_copy (mp2_struct);
				gst_structure_set (mp4_struct, "mpegversion", G_TYPE_INT, 4, NULL);
				gst_caps_append_structure (caps, mp4_struct);
			}
		}
		if ( gst_structure_has_name (s, "audio/x-ac3" ) || gst_structure_has_name (s, "audio/x-private1-ac3" ) )
		{
			GstStructure *ac3_struct = gst_structure_copy (s);
			gst_caps_append_structure (caps, ac3_struct);
		}
		if ( ( self->priv->model == DM8000 ) && ( gst_structure_has_name (s, "audio/x-dts" ) || gst_structure_has_name (s, "audio/x-private1-dts") ) )
		{
			GstStructure *dts_struct = gst_structure_copy (s);
			gst_caps_append_structure (caps, dts_struct);
		}
	}

	GST_DEBUG_OBJECT (self, "old caps: %s\nnew caps: %s\n", gst_caps_to_string(in_caps), gst_caps_to_string(caps));

	gst_caps_unref (in_caps);

	return caps;
}

static gboolean 
gst_dvbaudiosink_set_caps (GstBaseSink * basesink, GstCaps * caps)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (basesink);
	GstStructure *structure = gst_caps_get_structure (caps, 0);
	const char *type = gst_structure_get_name (structure);
	int bypass = -1;

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
//					printf("have codec data -> obj_type = %d, rate_idx = %d, channels = %d\n",
//						obj_type, rate_idx, channels);
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
					GST_WARNING_OBJECT (self, "no codec data");
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
	} else
	{
		GST_ELEMENT_ERROR (self, STREAM, TYPE_NOT_FOUND, (NULL), ("unimplemented stream type %s", type));
		return FALSE;
	}

	GST_INFO_OBJECT(self, "setting dvb mode 0x%02x\n", bypass);

	if (ioctl(self->fd, AUDIO_SET_BYPASS_MODE, bypass) < 0)
	{
		GST_ELEMENT_WARNING (self, STREAM, DECODE, (NULL), ("hardware decoder can't be set to bypass mode %i.", bypass));
// 		return FALSE;
	}
	self->priv->bypass_set = TRUE;
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
		break;
	case GST_EVENT_EOS:
	{
		struct pollfd pfd[2];
		int retval;
		pfd[0].fd = READ_SOCKET(self);
		pfd[0].events = POLLIN;
#if 0
			/* needs driver support */
		pfd[1].fd = self->fd;
		pfd[1].events = POLLHUP;

		do {
			GST_DEBUG_OBJECT (self, "going into poll to wait for EOS");
			retval = poll(pfd, 2, -1);
		} while ((retval == -1 && errno == EINTR));
		
		if (pfd[0].revents & POLLIN) /* flush */
			break;
		GST_DEBUG_OBJECT (self, "EOS wait ended because of buffer empty. now waiting for PTS %llx", self->pts_eos);
#else
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
			if (pfd[0].revents & POLLIN)
				break;
		} while (1);
#endif
		break;
	}
	case GST_EVENT_NEWSEGMENT:{
		GstFormat fmt;
		gboolean update;
		gdouble rate, applied_rate;
		gint64 cur, stop, time;
		int skip = 0, repeat = 0, ret;
		gst_event_parse_new_segment_full (event, &update, &rate, &applied_rate,	&fmt, &cur, &stop, &time);
		GST_LOG_OBJECT (self, "GST_EVENT_NEWSEGMENT rate=%f applied_rate=%f\n", rate, applied_rate);
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

static GstFlowReturn
gst_dvbaudiosink_render (GstBaseSink * sink, GstBuffer * buffer)
{
	GstDVBAudioSink *self = GST_DVBAUDIOSINK (sink);
	unsigned char pes_header[64];
	int skip = self->skip;
	unsigned int size = GST_BUFFER_SIZE (buffer) - skip;
	unsigned char *data = GST_BUFFER_DATA (buffer) + skip;
	gint retval;
	size_t pes_header_size;
	struct pollfd pfd[2];

//	int i=0;
//	for (;i < (size > 0x1F ? 0x1F : size); ++i)
//		printf("%02x ", data[i]);
//	printf("%d bytes\n", size);
//	printf("timestamp: %08llx\n", (long long)GST_BUFFER_TIMESTAMP(buffer));

	if ( !self->priv->bypass_set )
	{
		GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL), ("hardware decoder not setup (no caps in pipeline?)"));
		return GST_FLOW_ERROR;
	}

	pfd[0].fd = READ_SOCKET(self);
	pfd[0].events = POLLIN;
	pfd[1].fd = self->fd;
	pfd[1].events = POLLOUT;

	do {
		GST_LOG_OBJECT (self, "going into poll, have %d bytes to write", size);
		retval = poll(pfd, 2, -1);
	} while ((retval == -1 && errno == EINTR));

	if (retval == -1)
		goto poll_error;

	if (pfd[0].revents & POLLIN) {
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

	if (self->aac_adts_header_valid)
		size += 7;

		/* do we have a timestamp? */
	if (GST_BUFFER_TIMESTAMP(buffer) != GST_CLOCK_TIME_NONE)
	{
		unsigned long long pts = GST_BUFFER_TIMESTAMP(buffer) * 9LL / 100000 /* convert ns to 90kHz */;

		self->pts_eos = pts;

		pes_header[4] = (size + 8) >> 8;
		pes_header[5] = (size + 8) & 0xFF;
		
		pes_header[6] = 0x80;
		pes_header[7] = 0x80;
		
		pes_header[8] = 5;
		
		pes_header[9]  = 0x21 | ((pts >> 29) & 0xE);
		pes_header[10] = pts >> 22;
		pes_header[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes_header[12] = pts >> 7;
		pes_header[13] = 0x01 | ((pts << 1) & 0xFE);
		pes_header_size = 14;
	} else
	{
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

	write(self->fd, pes_header, pes_header_size);
	write(self->fd, data, GST_BUFFER_SIZE (buffer) - skip);

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
stopped:
	{
		GST_WARNING_OBJECT (self, "Select stopped");
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
//		ioctl(self->fd, AUDIO_SET_BYPASS_MODE, 0);
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

		int video_fd = open("/dev/dvb/adapter0/video0", O_RDWR);
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
