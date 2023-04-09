extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include "media.h"
#include "frame.h"
#include "encoder.h"
#include "decoder.h"
#include "../private/ff_helpers.h"
#include "../private/ff_math_helpers.h"
#include "../private/utility/info.h"

#include <stdexcept>

ff::encoder::encoder(const char* name)
{
	codec = avcodec_find_encoder_by_name(name);
}

ff::encoder::encoder(int ID)
{
	codec = avcodec_find_encoder((AVCodecID)ID);
}

bool ff::encoder::try_feed(ff::frame& frame)
{
	int ret = avcodec_send_frame(codec_ctx, frame);

	if (ret < 0)
	{
		switch (ret)
		{
		case AVERROR_EOF:
		case AVERROR(EAGAIN):
			return false;
		default:
			ON_FF_ERROR_WITH_CODE("Could not feed a frame to the encoder.", ret);
		}
	}

	return true;
}

ff::packet ff::encoder::try_get_one()
{
	ff::packet pkt;
	int ret = avcodec_receive_packet(codec_ctx, pkt);

	if (ret >= 0) // we got the frame we want
	{
		return pkt;
	}
	else
	{
		switch (ret)
		{
		case AVERROR_EOF:
			eof_reached = true;
		case AVERROR(EAGAIN):
			return ff::packet(nullptr);
		default:
			ON_FF_ERROR_WITH_CODE("Could not encode a packet.", ret);
		}
	}
}

void ff::encoder::flush_codec()
{
	super::flush_codec();
	// reset eof state as the decoder is flushed.
	eof_reached = false;
}

void ff::encoder::start_draining()
{
	int ret = avcodec_send_frame(codec_ctx, nullptr);
	if (ret < 0)
	{
		ON_FF_ERROR_WITH_CODE("Could not enter the draining mode during encoding.", ret)
	}
}

int ff::encoder::get_desired_pixel_format() const
{
	return get_codec()->pix_fmts[0];
}

void ff::encoder::get_best_audio_channel(::AVChannelLayout* dst) const
{
	const AVChannelLayout* p, *best_ch_layout = nullptr;
	int best_nb_channels = 0;

	if (!codec->ch_layouts) // If the codec doesn't have any channel layout configuration
	{
		// gives a stereo channel configuration
		AVChannelLayout layout{};
		layout.order = AV_CHANNEL_ORDER_NATIVE;
		layout.nb_channels = 2;
		layout.u.mask = AV_CH_LAYOUT_STEREO;

		if (av_channel_layout_copy(dst, &layout) < 0)
		{
			ON_FF_ERROR("Could not copy audio channel layout.")
		}
	}
	else // If the codec has some
	{
		p = codec->ch_layouts;
		// find the channel layout with the largest number of channels
		while (p->nb_channels) // the array is terminated by a zeroed channel layout
		{
			int nb_channels = p->nb_channels;

			if (nb_channels > best_nb_channels) 
			{
				best_ch_layout = p;
				best_nb_channels = nb_channels;
			}
			++p;
		}
		if (!best_ch_layout) // Could not select the best one
		{
			// Then use the first one
			best_ch_layout = codec->ch_layouts;
		}
		if (av_channel_layout_copy(dst, best_ch_layout) < 0)
		{
			ON_FF_ERROR("Could not copy audio channel layout.")
		}
	}

}

int ff::encoder::get_best_audio_sample_rate() const
{
	int best_samplerate = 0;

	if (!codec->supported_samplerates)
	{
		return ffhelpers::common_audio_sample_rate;
	}

	const int* p = codec->supported_samplerates;
	// find the highest rate in the array.
	while (*p) // the array is terminated by 0
	{
		if (best_samplerate < *p)
		{
			best_samplerate = *p;
		}
		++p;
	}

	if (best_samplerate == 0) // the encoder does not know its supported sample rates.
	{
		return ffhelpers::common_audio_sample_rate;
	}

	return best_samplerate;
}

int ff::encoder::get_desired_audio_sample_format() const
{
	return codec->sample_fmts ? (int)codec->sample_fmts[0] : (int)AV_SAMPLE_FMT_S16;
}

int ff::encoder::get_required_number_of_samples_per_channel() const
{
	// see its comment
	return codec_ctx->frame_size;
}

bool ff::encoder::is_pixel_format_supported(int fmt) const
{
	if (!codec->pix_fmts) // unknown
	{
		return false;
	}
	else
	{
		const AVPixelFormat* pf = codec->pix_fmts;
		while (*pf != -1) // the array is -1-terminated.
		{
			if ((AVPixelFormat)fmt == *pf)
			{
				return true;
			}
			++pf;
		}
	}

	return false;
}

bool ff::encoder::is_audio_sample_format_supported(int fmt) const
{
	if (!codec->sample_fmts) // unknown
	{
		return false;
	}
	else
	{
		const AVSampleFormat* pf = codec->sample_fmts;
		while (*pf != -1) // the array is -1-terminated.
		{
			if ((AVSampleFormat)fmt == *pf)
			{
				return true;
			}
			++pf;
		}
	}

	return false;
}

bool ff::encoder::is_audio_sample_rate_supported(int rate) const
{
	if (!codec->supported_samplerates) // unknown
	{
		return false;
	}
	else
	{
		const int* psr = codec->supported_samplerates;
		while (*psr != 0) // terminated by 0
		{
			if (rate == *psr)
			{
				return true;
			}
			++psr;
		}
	}

	return false;
}

bool ff::encoder::is_audio_channel_layout_supported(const::AVChannelLayout* layout) const
{
	if (!codec->ch_layouts)
	{
		return false;
	}
	else
	{
		const AVChannelLayout* pcl = codec->ch_layouts;
		while (pcl->nb_channels != 0) // terminated by a zeroed layout
		{
			if (!av_channel_layout_compare(layout, pcl))
			{
				return true;
			}
			++pcl;
		}
	}

	return false;
}

bool ff::encoder::is_video_setting_supported(const video_info& info) const
{
	return is_pixel_format_supported(info.pix_fmt);
}

bool ff::encoder::is_audio_setting_supported(const audio_info&info) const
{
	return
		is_audio_sample_format_supported(info.sample_fmt) &&
		is_audio_sample_rate_supported(info.sample_rate) &&
		is_audio_channel_layout_supported(&info.ch_layout);
}

ff::transcode_encoder::transcode_encoder(const decoder& d, const output_media& m, const char* name) : encoder(name)
{
	fill_encoder_info(d, m);
}

ff::transcode_encoder::transcode_encoder(const decoder& d, const output_media& m, int ID) : encoder(ID)
{
	fill_encoder_info(d, m);
}

void ff::transcode_encoder::fill_encoder_info(const decoder& dec, const output_media& m)
{
	/* Allocate a codec context for the encoder */
	codec_ctx = avcodec_alloc_context3(codec);
	if (!codec_ctx)
	{
		ON_FF_ERROR("Failed to allocate the encoder context.")
	}

	// Copy infomation from the decoder to encoder.
	// If some part of the info is unavailble, we decide from the encoder's supported formats.
	{
		auto dec_ctx = dec.get_codec_ctx();

		// If the time base is set because of a rate change, then we will not use the decoder's time base.
		bool time_base_set = false;

		//if (dec_ctx->bit_rate != 0)
		//{
		//	 codec_ctx->bit_rate = dec_ctx->bit_rate;
		//}
		switch (codec->type)
		{
		case AVMEDIA_TYPE_VIDEO:	
			codec_ctx->pix_fmt = is_pixel_format_supported(dec_ctx->pix_fmt) ?
				dec_ctx->pix_fmt : (AVPixelFormat)get_desired_pixel_format();
			codec_ctx->width = dec_ctx->width;
			codec_ctx->height = dec_ctx->height;

			// TODO: check if framerate is supported and change the time base if a different one should be used.
			break;

		case AVMEDIA_TYPE_AUDIO:
			codec_ctx->sample_fmt = is_audio_sample_format_supported(dec_ctx->sample_fmt) ?
				dec_ctx->sample_fmt : (AVSampleFormat)get_desired_audio_sample_format();
			codec_ctx->sample_rate = is_audio_sample_rate_supported(dec_ctx->sample_rate) ?
				dec_ctx->sample_rate : get_best_audio_sample_rate();
			if (is_audio_channel_layout_supported(&dec_ctx->ch_layout))
			{
				int err = 0;
				if ((err = av_channel_layout_copy(&codec_ctx->ch_layout, &dec_ctx->ch_layout)) < 0)
				{
					ON_FF_ERROR_WITH_CODE("Could not copy channel layout.", err);
				}
			}
			else
			{
				get_best_audio_channel(&codec_ctx->ch_layout);
			}

			/* Some container formats (like MP4) require global headers to be present.
			* Mark the encoder so that it behaves accordingly. */
			if (m.get_format_ctx()->oformat->flags & AVFMT_GLOBALHEADER)
			{
				codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}

			if (dec_ctx->time_base != codec_ctx->time_base)
			{
				codec_ctx->time_base.num = 1;
				codec_ctx->time_base.den = codec_ctx->sample_rate;
				time_base_set = true;
			}

			break;

		default:
			break;
		}

		if (!time_base_set) // if time base is not supposed to be changed, then use the decoder's.
		{
			if (dec_ctx->time_base.num != 0 && dec_ctx->time_base.den != 0)
			{
				codec_ctx->time_base = dec_ctx->time_base;
			}
			else
			{
				codec_ctx->time_base = ffhelpers::ff_global_time_base;
			}
		}

	}


	//avcodec_parameters_free(&par);
}

void ff::transcode_encoder::create()
{
	configure_multithreading(16);

	int ret = 0;
	/* Init the encoder */
	if ((ret = avcodec_open2(codec_ctx, codec, NULL)) < 0)
	{
		ON_FF_ERROR("Failed to open the encoder.")
	}
}
