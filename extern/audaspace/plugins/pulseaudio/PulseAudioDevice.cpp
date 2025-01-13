/*******************************************************************************
 * Copyright 2009-2016 Jörg Müller
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

#include "PulseAudioDevice.h"

#include "Exception.h"
#include "IReader.h"
#include "PulseAudioLibrary.h"

#include "devices/DeviceManager.h"
#include "devices/IDeviceFactory.h"

AUD_NAMESPACE_BEGIN

PulseAudioDevice::PulseAudioSynchronizer::PulseAudioSynchronizer(PulseAudioDevice* device) : m_device(device)
{
}

void PulseAudioDevice::PulseAudioSynchronizer::play()
{
	/* Make sure that our start time is up to date. */
	AUD_pa_stream_get_time(m_device->m_stream, &m_time_start);
	m_playing = true;
}

void PulseAudioDevice::PulseAudioSynchronizer::stop()
{
	std::shared_ptr<IHandle> dummy_handle;
	m_seek_pos = getPosition(dummy_handle);
	m_playing = false;
}

void PulseAudioDevice::PulseAudioSynchronizer::seek(std::shared_ptr<IHandle> handle, double time)
{
	/* Update start time here as we might update the seek position while playing back. */
	AUD_pa_stream_get_time(m_device->m_stream, &m_time_start);
	m_seek_pos = time;
	handle->seek(time);
}

double PulseAudioDevice::PulseAudioSynchronizer::getPosition(std::shared_ptr<IHandle> /*handle*/)
{
	pa_usec_t time;
	if(!m_playing)
	{
		return m_seek_pos;
	}
	AUD_pa_stream_get_time(m_device->m_stream, &time);
	return (time - m_time_start) * 1.0e-6 + m_seek_pos;
}

void PulseAudioDevice::updateRingBuffer()
{
	unsigned int samplesize = AUD_DEVICE_SAMPLE_SIZE(m_specs);

	std::unique_lock<std::mutex> lock(m_mixingLock);

	Buffer buffer;

	while(m_valid)
	{
		{
			std::lock_guard<ILockable> device_lock(*this);

			if(m_playback)
			{
				size_t size = m_ring_buffer.getWriteSize();

				size_t sample_count = size / samplesize;

				if(sample_count > 0)
				{
					size = sample_count * samplesize;

					buffer.assureSize(size);

					mix(reinterpret_cast<data_t*>(buffer.getBuffer()), sample_count);

					m_ring_buffer.write(reinterpret_cast<data_t*>(buffer.getBuffer()), size);
				}
			}
			else
			{
				if(m_ring_buffer.getReadSize() == 0 && !m_corked)
				{
					AUD_pa_threaded_mainloop_lock(m_mainloop);
					AUD_pa_stream_cork(m_stream, 1, nullptr, nullptr);
					AUD_pa_stream_flush(m_stream, nullptr, nullptr);
					AUD_pa_threaded_mainloop_unlock(m_mainloop);
					m_corked = true;
				}
			}
		}

		m_mixingCondition.wait(lock);
	}
}

void PulseAudioDevice::PulseAudio_state_callback(pa_context* context, void* data)
{
	PulseAudioDevice* device = (PulseAudioDevice*) data;

	device->m_state = AUD_pa_context_get_state(context);

	AUD_pa_threaded_mainloop_signal(device->m_mainloop, 0);
}

void PulseAudioDevice::PulseAudio_request(pa_stream* stream, size_t total_bytes, void* data)
{
	PulseAudioDevice* device = (PulseAudioDevice*) data;

	data_t* buffer;

	size_t sample_size = AUD_DEVICE_SAMPLE_SIZE(device->m_specs);

	while(total_bytes > 0)
	{
		size_t num_bytes = total_bytes;

		AUD_pa_stream_begin_write(stream, reinterpret_cast<void**>(&buffer), &num_bytes);

		size_t readsamples = device->m_ring_buffer.getReadSize();

		readsamples = std::min(readsamples, size_t(num_bytes)) / sample_size;

		device->m_ring_buffer.read(buffer, readsamples * sample_size);

		if(readsamples * sample_size < num_bytes)
			std::memset(buffer + readsamples * sample_size, 0, num_bytes - readsamples * sample_size);

		if(device->m_mixingLock.try_lock())
		{
			device->m_mixingCondition.notify_all();
			device->m_mixingLock.unlock();
		}

		AUD_pa_stream_write(stream, reinterpret_cast<void*>(buffer), num_bytes, nullptr, 0, PA_SEEK_RELATIVE);

		total_bytes -= num_bytes;
	}
}

void PulseAudioDevice::playing(bool playing)
{
	std::lock_guard<ILockable> lock(*this);

	m_playback = playing;

	if(playing)
	{
		AUD_pa_threaded_mainloop_lock(m_mainloop);
		AUD_pa_stream_cork(m_stream, 0, nullptr, nullptr);
		AUD_pa_threaded_mainloop_unlock(m_mainloop);
		m_corked = false;
	}
}

PulseAudioDevice::PulseAudioDevice(const std::string &name, DeviceSpecs specs, int buffersize) :
	m_synchronizer(this),
	m_playback(false),
	m_corked(true),
	m_state(PA_CONTEXT_UNCONNECTED),
	m_valid(true),
	m_underflows(0)
{
	m_mainloop = AUD_pa_threaded_mainloop_new();

	AUD_pa_threaded_mainloop_lock(m_mainloop);

	m_context = AUD_pa_context_new(AUD_pa_threaded_mainloop_get_api(m_mainloop), name.c_str());

	if(!m_context)
	{
		AUD_pa_threaded_mainloop_unlock(m_mainloop);
		AUD_pa_threaded_mainloop_free(m_mainloop);

		AUD_THROW(DeviceException, "Could not connect to PulseAudio.");
	}

	AUD_pa_context_set_state_callback(m_context, PulseAudio_state_callback, this);

	AUD_pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

	AUD_pa_threaded_mainloop_start(m_mainloop);

	while(m_state != PA_CONTEXT_READY)
	{
		switch(m_state)
		{
		case PA_CONTEXT_FAILED:
		case PA_CONTEXT_TERMINATED:
			AUD_pa_threaded_mainloop_unlock(m_mainloop);
			AUD_pa_threaded_mainloop_stop(m_mainloop);

			AUD_pa_context_disconnect(m_context);
			AUD_pa_context_unref(m_context);

			AUD_pa_threaded_mainloop_free(m_mainloop);

			AUD_THROW(DeviceException, "Could not connect to PulseAudio.");
			break;
		default:
			AUD_pa_threaded_mainloop_wait(m_mainloop);
			break;
		}
	}

	if(specs.channels == CHANNELS_INVALID)
		specs.channels = CHANNELS_STEREO;
	if(specs.format == FORMAT_INVALID)
		specs.format = FORMAT_FLOAT32;
	if(specs.rate == RATE_INVALID)
		specs.rate = RATE_48000;

	m_specs = specs;

	pa_sample_spec sample_spec;

	sample_spec.channels = specs.channels;
	sample_spec.format = PA_SAMPLE_FLOAT32;
	sample_spec.rate = specs.rate;

	switch(m_specs.format)
	{
	case FORMAT_U8:
		sample_spec.format = PA_SAMPLE_U8;
		break;
	case FORMAT_S16:
		sample_spec.format = PA_SAMPLE_S16NE;
		break;
	case FORMAT_S24:
		sample_spec.format = PA_SAMPLE_S24NE;
		break;
	case FORMAT_S32:
		sample_spec.format = PA_SAMPLE_S32NE;
		break;
	case FORMAT_FLOAT32:
		sample_spec.format = PA_SAMPLE_FLOAT32;
		break;
	case FORMAT_FLOAT64:
		m_specs.format = FORMAT_FLOAT32;
		break;
	default:
		break;
	}

	m_stream = AUD_pa_stream_new(m_context, "Playback", &sample_spec, nullptr);

	if(!m_stream)
	{
		AUD_pa_threaded_mainloop_unlock(m_mainloop);
		AUD_pa_threaded_mainloop_stop(m_mainloop);

		AUD_pa_context_disconnect(m_context);
		AUD_pa_context_unref(m_context);

		AUD_pa_threaded_mainloop_free(m_mainloop);

		AUD_THROW(DeviceException, "Could not create PulseAudio stream.");
	}

	AUD_pa_stream_set_write_callback(m_stream, PulseAudio_request, this);

	buffersize *= AUD_DEVICE_SAMPLE_SIZE(m_specs);
	m_buffersize = buffersize;

	pa_buffer_attr buffer_attr;

	buffer_attr.fragsize = -1U;
	buffer_attr.maxlength = -1U;
	buffer_attr.minreq = -1U;
	buffer_attr.prebuf = -1U;
	buffer_attr.tlength = buffersize;

	m_ring_buffer.resize(buffersize);

	if(AUD_pa_stream_connect_playback(m_stream, nullptr, &buffer_attr, static_cast<pa_stream_flags_t>(PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE), nullptr, nullptr) < 0)
	{
		AUD_pa_threaded_mainloop_unlock(m_mainloop);
		AUD_pa_threaded_mainloop_stop(m_mainloop);

		AUD_pa_context_disconnect(m_context);
		AUD_pa_context_unref(m_context);

		AUD_pa_threaded_mainloop_free(m_mainloop);

		AUD_THROW(DeviceException, "Could not connect PulseAudio stream.");
	}

	AUD_pa_threaded_mainloop_unlock(m_mainloop);

	create();

	m_mixingThread = std::thread(&PulseAudioDevice::updateRingBuffer, this);
}

PulseAudioDevice::~PulseAudioDevice()
{
	m_valid = false;

	m_mixingLock.lock();
	m_mixingCondition.notify_all();
	m_mixingLock.unlock();

	m_mixingThread.join();

	AUD_pa_threaded_mainloop_stop(m_mainloop);

	AUD_pa_context_disconnect(m_context);
	AUD_pa_context_unref(m_context);

	AUD_pa_threaded_mainloop_free(m_mainloop);

	destroy();
}

ISynchronizer *PulseAudioDevice::getSynchronizer()
{
	return &m_synchronizer;
}

class PulseAudioDeviceFactory : public IDeviceFactory
{
private:
	DeviceSpecs m_specs;
	int m_buffersize;
	std::string m_name;

public:
	PulseAudioDeviceFactory() :
		m_buffersize(AUD_DEFAULT_BUFFER_SIZE),
		m_name("Audaspace")
	{
		m_specs.format = FORMAT_FLOAT32;
		m_specs.channels = CHANNELS_STEREO;
		m_specs.rate = RATE_48000;
	}

	virtual std::shared_ptr<IDevice> openDevice()
	{
		return std::shared_ptr<IDevice>(new PulseAudioDevice(m_name, m_specs, m_buffersize));
	}

	virtual int getPriority()
	{
		return 1 << 15;
	}

	virtual void setSpecs(DeviceSpecs specs)
	{
		m_specs = specs;
	}

	virtual void setBufferSize(int buffersize)
	{
		m_buffersize = buffersize;
	}

	virtual void setName(const std::string &name)
	{
		m_name = name;
	}
};

void PulseAudioDevice::registerPlugin()
{
	if(loadPulseAudio())
		DeviceManager::registerDevice("PulseAudio", std::shared_ptr<IDeviceFactory>(new PulseAudioDeviceFactory));
}

#ifdef PULSEAUDIO_PLUGIN
extern "C" AUD_PLUGIN_API void registerPlugin()
{
	PulseAudioDevice::registerPlugin();
}

extern "C" AUD_PLUGIN_API const char* getName()
{
	return "PulseAudio";
}
#endif

AUD_NAMESPACE_END
