
#include "audio_stream_ram.h"

AudioStreamRAM::AudioStreamRAM() :
		capacity(0),
		nframes(0),
		length(0),
		data(NULL) {
	mix_rate = AudioServer::get_singleton()->get_mix_rate();
}

AudioStreamRAM::~AudioStreamRAM() {
	if (data != NULL) memfree(data);
	data = NULL;
}

int AudioStreamRAM::_resample_from(int source_rate) {
	if (source_rate == 0) return -1;
	if (data == NULL) {
		return -1;
	}

	uint32_t new_length = uint32_t(nframes * (mix_rate / double(source_rate)));
	AudioFrame *new_data = (AudioFrame *)memalloc((new_length) * sizeof(AudioFrame));
	if (new_data == NULL) {
		memfree(data);
		data = NULL;
		return -2;
	}

	uint64_t mix_offset = 0;

	uint64_t mix_increment = uint64_t((source_rate / double(mix_rate)) * double(FP_LEN));

	for (uint32_t i = 0; i < new_length; ++i) {
		uint32_t idx = 4 + uint32_t(mix_offset >> FP_BITS);
		float mu = (mix_offset & FP_MASK) / float(FP_LEN);
		AudioFrame y0 = ((idx - 3) < nframes) ? data[idx - 3] : AudioFrame(0, 0);
		AudioFrame y1 = ((idx - 2) < nframes) ? data[idx - 2] : AudioFrame(0, 0);
		AudioFrame y2 = ((idx - 1) < nframes) ? data[idx - 1] : AudioFrame(0, 0);
		AudioFrame y3 = ((idx - 0) < nframes) ? data[idx - 0] : AudioFrame(0, 0);

		float mu2 = mu * mu;
		AudioFrame a0 = y3 - y2 - y0 + y1;
		AudioFrame a1 = y0 - y1 - a0;
		AudioFrame a2 = y2 - y0;
		AudioFrame a3 = y1;

		new_data[i] = (a0 * mu * mu2 + a1 * mu2 + a2 * mu + a3);

		mix_offset += mix_increment;
	}

	memfree(data);
	data = new_data;
	nframes = capacity = new_length;
	return nframes;
}

void AudioStreamRAM::update_length() {
	length = (nframes / (float)mix_rate);
}

void AudioStreamRAM::load(String path) {
	ERR_FAIL_COND_MSG(data != NULL, "reloading audio is forbidden");

	if (path.ends_with(".ogg")) {
		_decode_vorbis(path);
	} else if (path.ends_with(".wav")) {
		_decode_wave(path);
	}

	update_length();
}

bool AudioStreamRAM::is_valid() {
	return data != NULL;
}

Ref<AudioStreamPlayback> AudioStreamRAM::instance_playback() {
	Ref<AudioStreamPlaybackRAM> playback;
	playback.instance();
	playback->base = Ref<AudioStreamRAM>(this);
	playback->end_position = nframes;
	return playback;
}

String AudioStreamRAM::get_stream_name() const {
	return "RAMAudio";
}

float AudioStreamRAM::get_length() const {
	return length;
}

void AudioStreamRAM::_bind_methods() {
	ClassDB::bind_method("load", &AudioStreamRAM::load);
	ClassDB::bind_method("is_valid", &AudioStreamRAM::is_valid);
}

AudioStreamPlaybackRAM::AudioStreamPlaybackRAM() :
		active(false),
		loop(false),
		position(0),
		start_position(0) {
}

AudioStreamPlaybackRAM::~AudioStreamPlaybackRAM() {
}

void AudioStreamPlaybackRAM::stop() {
	active = false;
}

void AudioStreamPlaybackRAM::start(float p_from_pos) {
	if (base->data == NULL) {
		WARN_PRINT("attempting to play invalid audio");
	}

	seek(p_from_pos);
	loop_count = 0;
	active = true;
}

void AudioStreamPlaybackRAM::seek(float p_time) {
	position = start_position + p_time * base->mix_rate;
	if (position >= end_position) {
		position = end_position;
	}
}

inline void AudioStreamPlaybackRAM::_mix_loop(AudioFrame *p_buffer, float p_rate, int p_frames) {
	for (int i = 0; i < p_frames; ++i) {
		if (end_position <= start_position) {
			p_buffer[i] = AudioFrame(0, 0);
		} else {
			if (position >= end_position) {
				position = start_position;
				++loop_count;
			}
			p_buffer[i] = base->data[position++];
		}
	}
}

inline void AudioStreamPlaybackRAM::_mix(AudioFrame *p_buffer, float p_rate, int p_frames) {
	uint32_t end_of_mix = position + p_frames;
	int mix_frames = p_frames;

	if (end_position <= end_of_mix) {
		mix_frames = p_frames - (end_of_mix - end_position);
		end_of_mix = end_position;
	}

	int remain = p_frames - mix_frames;

	for (int i = 0; i < mix_frames; ++i) {
		p_buffer[i] = base->data[position + i];
	}

	if (remain > 0) {
		for (int i = mix_frames; i < p_frames; ++i) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		active = false;
	}

	position = end_of_mix;
}

void AudioStreamPlaybackRAM::mix(AudioFrame *p_buffer, float p_rate, int p_frames) {
	if (!active) {
		for (int i = 0; i < p_frames; ++i) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		return;
	}

	if (loop) {
		_mix_loop(p_buffer, p_rate, p_frames);
	} else {
		_mix(p_buffer, p_rate, p_frames);
	}
}

int AudioStreamPlaybackRAM::get_loop_count() const {
	return 0;
}
float AudioStreamPlaybackRAM::get_playback_position() const {
	return position / (float)base->mix_rate;
}

float AudioStreamPlaybackRAM::get_length() const {
	if (loop) return 0;

	float length = (end_position - start_position) / (float)base->mix_rate;

	if (length < 0.0213) length = 0.0213;
	return length;
}

void AudioStreamPlaybackRAM::set_slice(float p_start, float p_length) {
	uint32_t nframes = base->nframes;

	position = start_position = p_start * base->mix_rate;
	if (start_position > nframes) {
		position = start_position = nframes;
	}

	if (p_length < 0) {
		end_position = nframes;
	} else {
		end_position = start_position + p_length * base->mix_rate;
		if (end_position > nframes) {
			end_position = nframes;
		}
	}
}

void AudioStreamPlaybackRAM::set_loop(bool is_loop) {
	loop = is_loop;
}

bool AudioStreamPlaybackRAM::is_playing() const {
	return active;
}

void AudioStreamPlaybackRAM::_bind_methods() {
	ClassDB::bind_method("set_slice", &AudioStreamPlaybackRAM::set_slice);
	ClassDB::bind_method("set_loop", &AudioStreamPlaybackRAM::set_loop);
}