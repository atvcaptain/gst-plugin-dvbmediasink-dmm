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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
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
#include <linux/dvb/video.h>
#include <fcntl.h>

#include <gst/gst.h>

#include "gstdvbvideosink.h"

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

static GstStaticPadTemplate sink_factory =
GST_STATIC_PAD_TEMPLATE (
	"sink",
	GST_PAD_SINK,
	GST_PAD_ALWAYS,
	GST_STATIC_CAPS ("video/mpeg, "
		"mpegversion = (int) [ 1, 2 ], " "systemstream = (boolean) false")
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


static void
gst_dvbvideosink_base_init (gpointer klass)
{
	static GstElementDetails element_details = {
		"A DVB video sink",
		"Generic/DVBVideoSink",
		"Outputs a MPEG2 PES / ES into a DVB video device for hardware playback",
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
	GstPad *pad;
	
	pad = GST_BASE_SINK_PAD (klass);
	
	gst_pad_set_query_function (pad, GST_DEBUG_FUNCPTR (gst_dvbvideosink_query));
	
	klass->silent = FALSE;
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
	GstDVBVideoSink *self;
//	GstFormat format;
	
	self = GST_DVBVIDEOSINK (GST_PAD_PARENT (pad));
	switch (GST_QUERY_TYPE (query)) {
	default:
		return gst_pad_query_default (pad, query);
	}
}

static gboolean
gst_dvbvideosink_event (GstBaseSink * sink, GstEvent * event)
{
	GstEventType type;
	GstDVBVideoSink *self;
	self = GST_DVBVIDEOSINK (sink);
	switch (type) {
	case GST_EVENT_FLUSH_START:
	case GST_EVENT_FLUSH_STOP:
		printf("DVB video sink FLUSH\n");
		ioctl(self->fd, VIDEO_CLEAR_BUFFER);
		break;
	default:
		printf("dvb video sink: unknown event type %d\n", type); 
		return FALSE;
	}
	return TRUE;
}

static GstFlowReturn
gst_dvbvideosink_render (GstBaseSink * sink, GstBuffer * buffer)
{
	unsigned char pes_header[19];
	GstDVBVideoSink *self;
	unsigned int size;
	self = GST_DVBVIDEOSINK (sink);
	
	size = GST_BUFFER_SIZE (buffer);
	
//	printf("write %d, timestamp: %08llx\n", GST_BUFFER_SIZE (buffer), (long long)GST_BUFFER_TIMESTAMP(buffer));

	if (self->fd < 0)
		return GST_FLOW_OK;

	pes_header[0] = 0;
	pes_header[1] = 0;
	pes_header[2] = 1;
	pes_header[3] = 0xE0;
	
		/* do we have a timestamp? */
	if (GST_BUFFER_TIMESTAMP(buffer) != GST_CLOCK_TIME_NONE)
	{
		unsigned long long pts = GST_BUFFER_TIMESTAMP(buffer) * 9LL / 100000; /* convert ns to 90kHz */
		
		pes_header[4] = (size + 13) >> 8;
		pes_header[5] = (size + 13) & 0xFF;
		
		pes_header[6] = 0x80;
		pes_header[7] = 0xC0;
		
		pes_header[8] = 10;
		
		pes_header[9]  = 0x31 | ((pts >> 29) & 0xE);
		pes_header[10] = pts >> 22;
		pes_header[11] = 0x01 | ((pts >> 14) & 0xFE);
		pes_header[12] = pts >> 7;
		pes_header[13] = 0x01 | ((pts << 1) & 0xFE);
		
		int64_t dts = pts; /* what to use as DTS-PTS offset? */
		
		pes_header[14] = 0x11 | ((dts >> 29) & 0xE);
		pes_header[15] = dts >> 22;
		pes_header[16] = 0x01 | ((dts >> 14) & 0xFE);
		pes_header[17] = dts >> 7;
		pes_header[18] = 0x01 | ((dts << 1) & 0xFE);
		write(self->fd, pes_header, 19);
	} else
	{
		pes_header[4] = (size + 3) >> 8;
		pes_header[5] = (size + 3) & 0xFF;
		pes_header[6] = 0x80;
		pes_header[7] = 0x00;
		pes_header[8] = 0;
		write(self->fd, pes_header, 9);
	}
	
	write(self->fd, GST_BUFFER_DATA (buffer), GST_BUFFER_SIZE (buffer));

	return GST_FLOW_OK;
}

static gboolean
gst_dvbvideosink_start (GstBaseSink * basesink)
{
	GstDVBVideoSink *self;
	self = GST_DVBVIDEOSINK (basesink);
	printf("start\n");
	self->fd = open("/dev/dvb/adapter0/video0", O_RDWR);
//	self->fd = open("/dump.pes", O_RDWR|O_CREAT, 0555);
	
	if (self->fd >= 0)
	{
		ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_MEMORY);
		ioctl(self->fd, VIDEO_PLAY);
	}
	return TRUE;
}

static gboolean
gst_dvbvideosink_stop (GstBaseSink * basesink)
{
	GstDVBVideoSink *self;
	self = GST_DVBVIDEOSINK (basesink);
	printf("stop\n");
	if (self->fd >= 0)
	{
		ioctl(self->fd, VIDEO_STOP);
		ioctl(self->fd, VIDEO_SELECT_SOURCE, VIDEO_SOURCE_DEMUX);
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
